/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * Lower warp memory to use local memory
 * and shuffle intrinsics.
 *
 * \file lower_warp_memory.cc
 */
// Thanks to Andrew Adams and Vinod Grover for
// explaining the concept of warp shuffle.
#include <tvm/ir.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_visitor.h>
#include <tvm/ir_pass.h>
#include <unordered_set>
#include "ir_util.h"
#include "../arithmetic/compute_expr.h"
#include "../runtime/thread_storage_scope.h"

namespace tvm {
namespace ir {

// Rewrite Rule
//
// There is no special warp memory in most GPUs.
// Instead, we can stripe the data into threads
// and store the data into local memory.
//
// This requires us to do the following rewriting:
// - Rewrite allocation to use local memory.
// - Rewrite store of warp memory to local store.
// - Rewrite load of warp memory to local plus a shuffle.
//
// Define a generic shuffle intrinsic warp_shuffle(data, warp_index).
// We can use the following rewriting rule
//
// Before rewrite,
//
//   alloc warp warp_mem[n * warp_size * m]
//   store warp_mem[m * warp_index + (warp_size * m) * y + x]
//   load warp_mem[m * z + (warp_size * m) * y + x]
//   subject to x \in [0, m), y \in [0, n)
//
// After rewrite:
//
//   alloc local local_mem[n * m]
//   store warp_mem[m * y + x]
//   warp_shuffle(load warp_mem[m * y + x], z)
//   subject to (m * y + x) is invariant to warp_index

// Algorithm
//
// To implement this rewrite rule, we can do the follow step:
// For each warp memory alloc
// - Use linear pattern detector on load index to find m
// - Deduce n given warp_size and alloc size
// - Now that we have m, n, warp_size, we can proceed with the rewrite

// Visitor to find m in pattern
// store warp_mem[m * warp_index + (warp_size * m) * y + x]
class WarpStoreCoeffFinder : private IRVisitor {
 public:
  WarpStoreCoeffFinder(const Variable* buffer,
                       Var warp_index,
                       arith::Analyzer* analyzer)
      : buffer_(buffer),
        warp_index_(warp_index),
        analyzer_(analyzer) {
  }
  // find the warp co-efficient in the statement given the warp size
  int Find(const Stmt& stmt) {
    this->Visit(stmt);
    return warp_coeff_;
  }

 private:
  /// Visitor implementation
  void Visit_(const Store *op) final {
    if (op->buffer_var.get() == buffer_) {
      if (op->value.dtype().lanes() == 1) {
        UpdatePattern(op->index);
      } else {
        Expr base;
        CHECK(GetRamp1Base(op->index, op->value.dtype().lanes(), &base))
            << "LowerWarpMemory failed due to store index=" << op->index
            << ", can only handle continuous store";
        UpdatePattern(base);
      }
    } else {
      IRVisitor::Visit_(op);
    }
  }

  void UpdatePattern(const Expr& index) {
    Array<Expr> m =
        arith::DetectLinearEquation(index, {warp_index_});
    CHECK_EQ(m.size(), 2U)
        << "LowerWarpMemory failed due to store index=" << index;
    int coeff = 0;
    Expr mcoeff = analyzer_->canonical_simplify(m[0]);

    CHECK(arith::GetConstInt(mcoeff, &coeff) && coeff > 0)
        << "LowerWarpMemory failed due to store index=" << index
        << ", require positive constant coefficient on warp index " << warp_index_
        << " but get " << mcoeff;

    if (warp_coeff_ != 0) {
      CHECK_EQ(warp_coeff_, coeff)
          << "LowerWarpMemory failed due to two different store coefficient to warp index";
    } else {
      warp_coeff_ = coeff;
    }
  }

  // The buffer variable
  const Variable* buffer_;
  // the warp index
  Var warp_index_;
  // the coefficient
  int warp_coeff_{0};
  // analyzer.
  arith::Analyzer* analyzer_;
};


// Visitor to find the warp index
class WarpIndexFinder : private IRVisitor {
 public:
  explicit WarpIndexFinder(int warp_size)
      : warp_size_(warp_size) {
  }
  // find the warp co-efficient in the statement given the warp size
  IterVar Find(const Stmt& stmt) {
    this->Visit(stmt);
    CHECK(warp_index_.defined())
        << "Cannot find warp index(threadIdx.x) within the scope of warp memory";
    return warp_index_;
  }

 private:
  /// Visitor implementation
  void Visit_(const AttrStmt *op) final {
    if (op->attr_key == attr::thread_extent) {
      IterVar iv = Downcast<IterVar>(op->node);
      if (iv->thread_tag == "threadIdx.x") {
        int value;
        CHECK(arith::GetConstInt(op->value, &value) &&
              value == warp_size_)
            << "Expect threadIdx.x 's size to be equal to warp size("
            << warp_size_ << ")" << " to enable warp memory"
            << " but get " << op->value << " instead";
        if (warp_index_.defined()) {
          CHECK(warp_index_.same_as(iv))
              << "Find two instance of " << warp_index_->thread_tag
              << " in the same kernel. "
              << "Please create it using thread_axis once and reuse the axis "
              << "across multiple binds in the same kernel";
        } else {
          warp_index_ = iv;
        }
      }
    }
    IRVisitor::Visit_(op);
  }
  // warp size
  int warp_size_{0};
  // the warp index
  IterVar warp_index_{nullptr};
};
// Mutator to change the read pattern
class WarpAccessRewriter : protected IRMutator {
 public:
  explicit WarpAccessRewriter(int warp_size, arith::Analyzer* analyzer)
      : warp_size_(warp_size), analyzer_(analyzer) {}
  // Rewrite the allocate statement which transforms
  // warp memory to local memory.
  Stmt Rewrite(const Allocate* op, const Stmt& stmt) {
    buffer_ = op->buffer_var.get();
    int alloc_size = op->constant_allocation_size();
    CHECK_GT(alloc_size, 0)
        << "warp memory only support constant alloc size";
    alloc_size *= op->dtype.lanes();
    warp_index_ = WarpIndexFinder(warp_size_).Find(op->body)->var;
    warp_coeff_ = WarpStoreCoeffFinder(
        buffer_, warp_index_, analyzer_).Find(op->body);
    CHECK_EQ(alloc_size % (warp_size_ * warp_coeff_), 0)
        << "Warp memory must be multiple of warp size";
    warp_group_ = alloc_size / (warp_size_ * warp_coeff_);
    return Allocate::make(
        op->buffer_var,
        op->dtype,
        {make_const(DataType::Int(32), alloc_size / warp_size_)},
        op->condition,
        this->Mutate(op->body));
  }

 protected:
  Expr Mutate_(const Variable* op, const Expr& expr) {
    CHECK(op != buffer_)
        << "Cannot access address of warp memory directly";
    return IRMutator::Mutate_(op, expr);
  }

  Stmt Mutate_(const Store* op, const Stmt& stmt) {
    if (op->buffer_var.get() == buffer_) {
      Expr local_index, group;
      std::tie(local_index, group) = SplitIndexByGroup(op->index);
      return Store::make(op->buffer_var, op->value, local_index, op->predicate);
    } else {
      return IRMutator::Mutate_(op, stmt);
    }
  }

  Expr Mutate_(const Load* op, const Expr& expr) {
    if (op->buffer_var.get() == buffer_) {
      Expr local_index, group;
      std::tie(local_index, group) = SplitIndexByGroup(op->index);
      // invariance: local index must do not contain warp id
      CHECK(!ExprUseVar(local_index, {warp_index_.get()}))
          << "LowerWarpMemory failed to rewrite load to shuffle for index "
          << op->index << " local_index=" << local_index;
      Expr load_value = Load::make(
          op->dtype, op->buffer_var, local_index, op->predicate);
      return Call::make(load_value.dtype(),
                        intrinsic::tvm_warp_shuffle,
                        {load_value, group},
                        Call::Intrinsic);
    } else {
      return IRMutator::Mutate_(op, expr);
    }
  }
  // Split the index to the two component
  // <local_index, source_index>
  // local index is the index in the local
  // source index is the corresponding source index
  // in this access pattern.
  std::pair<Expr, Expr> SplitIndexByGroup(const Expr& index) {
    if (index.dtype().lanes() != 1) {
      Expr base, local_index, group;
      CHECK(GetRamp1Base(index, index.dtype().lanes(), &base));
      std::tie(local_index, group) = SplitIndexByGroup(base);
      local_index =
          Ramp::make(local_index, make_const(local_index.dtype(), 1), index.dtype().lanes());
      return std::make_pair(local_index, group);
    }
    Expr m = make_const(index.dtype(), warp_coeff_);

    // simple case, warp index is on the highest.
    if (warp_group_ == 1) {
      Expr x = analyzer_->canonical_simplify(indexmod(index, m));
      Expr z = analyzer_->canonical_simplify(indexdiv(index, m));
      return std::make_pair(x, z);
    } else {
      Expr x = analyzer_->canonical_simplify(indexmod(index, m));
      Expr y = index / make_const(index.dtype(), warp_coeff_ * warp_size_);
      y = y * m + x;
      Expr z = indexdiv(indexmod(index, make_const(index.dtype(), warp_coeff_ * warp_size_)),
                        m);
      return std::make_pair(analyzer_->canonical_simplify(y),
                            analyzer_->canonical_simplify(z));
    }
  }

 private:
  // the warp size
  int warp_size_{0};
  // The buffer variable
  const Variable* buffer_;
  // Warp index
  Var warp_index_;
  // the coefficient m
  int warp_coeff_{0};
  // the coefficient n
  int warp_group_{0};
  // Internal analyzer
  arith::Analyzer* analyzer_;
};


// Bind bound information of variables to make analyzer more effective
// TODO(tqchen): consider a pass to inline the bound info into the expr
// so analysis can be context independent.
class BindVarBoundInfo : public IRVisitor {
 public:
  explicit BindVarBoundInfo(arith::Analyzer* analyzer)
      : analyzer_(analyzer) {}

  void Visit_(const For* op) final {
    const Var& loop_var = op->loop_var;
    analyzer_->Bind(loop_var, Range::make_by_min_extent(op->min, op->extent));
    IRVisitor::Visit_(op);
  }

  void Visit_(const AttrStmt* op) {
    if (op->attr_key == attr::thread_extent ||
        op->attr_key == attr::virtual_thread) {
      IterVar iv = Downcast<IterVar>(op->node);
      CHECK_NE(iv->thread_tag.length(), 0U);
      if (!var_dom_.count(iv->var.get())) {
        Range dom = Range::make_by_min_extent(0, op->value);
        var_dom_[iv->var.get()] = dom;
        analyzer_->Bind(iv->var, dom);
      }
    }
    IRVisitor::Visit_(op);
  }

 protected:
  // internal analyzer.
  arith::Analyzer* analyzer_;
  // variable domain
  std::unordered_map<const Variable*, Range> var_dom_;
};

// Mutator to change the read pattern
class WarpMemoryRewriter : private IRMutator {
 public:
  explicit WarpMemoryRewriter(int warp_size)
      : warp_size_(warp_size) {
  }

  Stmt Rewrite(Stmt stmt) {
    if (warp_size_ == 1) return stmt;
    BindVarBoundInfo(&analyzer_).Visit(stmt);
    stmt = this->Mutate(stmt);
    stmt = CanonicalSimplify(stmt);
    return stmt;
  }

 private:
  Stmt Mutate_(const Allocate* op, const Stmt& stmt) {
    if (warp_buffer_.count(op->buffer_var.get())) {
      WarpAccessRewriter rewriter(warp_size_, &analyzer_);
      return rewriter.Rewrite(op, stmt);
    } else {
      return IRMutator::Mutate_(op, stmt);
    }
  }

  Stmt Mutate_(const AttrStmt* op, const Stmt& stmt) {
    using runtime::StorageScope;
    if (op->attr_key == attr::storage_scope) {
      const Variable* buf = op->node.as<Variable>();
      StorageScope scope = StorageScope::make(op->value.as<StringImm>()->value);
      if (scope.rank == runtime::StorageRank::kWarp) {
        warp_buffer_.insert(buf);
        Stmt ret = IRMutator::Mutate_(op, stmt);
        op = ret.as<AttrStmt>();
        return AttrStmt::make(
            op->node, op->attr_key, StringImm::make("local"), op->body);
      }
    }
    return IRMutator::Mutate_(op, stmt);
  }

  int warp_size_{0};
  std::unordered_set<const Variable*> warp_buffer_;
  arith::Analyzer analyzer_;
  // variable domain
  std::unordered_map<const Variable*, Range> var_dom_;
};

LoweredFunc
LowerWarpMemory(LoweredFunc f, int warp_size) {
  CHECK_EQ(f->func_type, kDeviceFunc);
  auto n = make_node<LoweredFuncNode>(*f.operator->());
  n->body = WarpMemoryRewriter(warp_size).Rewrite(n->body);
  return LoweredFunc(n);
}

}  // namespace ir
}  // namespace tvm
