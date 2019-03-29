//===- StmtBlock.h ----------------------------------------------*- C++ -*-===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file defines StmtBlock and *Stmt classes that extend Statement.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_STMTBLOCK_H
#define MLIR_IR_STMTBLOCK_H

#include "mlir/IR/Statement.h"

namespace mlir {
class IfStmt;
class StmtBlockList;
using CFGFunction = Function;
using MLFunction = Function;

template <typename BlockType> class PredecessorIterator;
template <typename BlockType> class SuccessorIterator;

/// Blocks represents an ordered list of Instructions.
class StmtBlock
    : public IRObjectWithUseList,
      public llvm::ilist_node_with_parent<StmtBlock, StmtBlockList> {
public:
  explicit StmtBlock() {}
  ~StmtBlock();

  void clear() {
    // Clear statements in the reverse order so that uses are destroyed
    // before their defs.
    while (!empty())
      statements.pop_back();
  }

  StmtBlockList *getParent() const { return parent; }

  /// Returns the closest surrounding statement that contains this block or
  /// nullptr if this is a top-level statement block.
  Statement *getContainingStmt();

  const Statement *getContainingStmt() const {
    return const_cast<StmtBlock *>(this)->getContainingStmt();
  }

  /// Returns the function that this statement block is part of.  The function
  /// is determined by traversing the chain of parent statements.
  MLFunction *getFunction();
  const MLFunction *getFunction() const {
    return const_cast<StmtBlock *>(this)->getFunction();
  }

  //===--------------------------------------------------------------------===//
  // Block argument management
  //===--------------------------------------------------------------------===//

  // This is the list of arguments to the block.
  using BlockArgListType = ArrayRef<BlockArgument *>;

  // FIXME: Not const correct.
  BlockArgListType getArguments() const { return arguments; }

  using args_iterator = BlockArgListType::iterator;
  using reverse_args_iterator = BlockArgListType::reverse_iterator;
  args_iterator args_begin() const { return getArguments().begin(); }
  args_iterator args_end() const { return getArguments().end(); }
  reverse_args_iterator args_rbegin() const { return getArguments().rbegin(); }
  reverse_args_iterator args_rend() const { return getArguments().rend(); }

  bool args_empty() const { return arguments.empty(); }

  /// Add one value to the argument list.
  BlockArgument *addArgument(Type type);

  /// Add one argument to the argument list for each type specified in the list.
  llvm::iterator_range<args_iterator> addArguments(ArrayRef<Type> types);

  /// Erase the argument at 'index' and remove it from the argument list.
  void eraseArgument(unsigned index);

  unsigned getNumArguments() const { return arguments.size(); }
  BlockArgument *getArgument(unsigned i) { return arguments[i]; }
  const BlockArgument *getArgument(unsigned i) const { return arguments[i]; }

  //===--------------------------------------------------------------------===//
  // Statement list management
  //===--------------------------------------------------------------------===//

  /// This is the list of statements in the block.
  using StmtListType = llvm::iplist<Statement>;
  StmtListType &getStatements() { return statements; }
  const StmtListType &getStatements() const { return statements; }

  // Iteration over the statements in the block.
  using iterator = StmtListType::iterator;
  using const_iterator = StmtListType::const_iterator;
  using reverse_iterator = StmtListType::reverse_iterator;
  using const_reverse_iterator = StmtListType::const_reverse_iterator;

  iterator               begin() { return statements.begin(); }
  iterator               end() { return statements.end(); }
  const_iterator         begin() const { return statements.begin(); }
  const_iterator         end() const { return statements.end(); }
  reverse_iterator       rbegin() { return statements.rbegin(); }
  reverse_iterator       rend() { return statements.rend(); }
  const_reverse_iterator rbegin() const { return statements.rbegin(); }
  const_reverse_iterator rend() const { return statements.rend(); }

  bool empty() const { return statements.empty(); }
  void push_back(Statement *stmt) { statements.push_back(stmt); }
  void push_front(Statement *stmt) { statements.push_front(stmt); }

  Statement       &back() { return statements.back(); }
  const Statement &back() const {
    return const_cast<StmtBlock *>(this)->back();
  }
  Statement       &front() { return statements.front(); }
  const Statement &front() const {
    return const_cast<StmtBlock *>(this)->front();
  }

  /// Returns the statement's position in this block or -1 if the statement is
  /// not present.
  int64_t findStmtPosInBlock(const Statement &stmt) const {
    int64_t j = 0;
    for (const auto &s : statements) {
      if (&s == &stmt)
        return j;
      j++;
    }
    return -1;
  }

  /// Returns 'stmt' if 'stmt' lies in this block, or otherwise finds the
  /// ancestor statement of 'stmt' that lies in this block. Returns nullptr if
  /// the latter fails.
  const Statement *findAncestorStmtInBlock(const Statement &stmt) const;
  Statement *findAncestorStmtInBlock(Statement *stmt) {
    return const_cast<Statement *>(findAncestorStmtInBlock(*stmt));
  }

  //===--------------------------------------------------------------------===//
  // Terminator management
  //===--------------------------------------------------------------------===//

  /// Get the terminator instruction of this block, or null if the block is
  /// malformed.
  OperationStmt *getTerminator();

  const OperationStmt *getTerminator() const {
    return const_cast<StmtBlock *>(this)->getTerminator();
  }

  //===--------------------------------------------------------------------===//
  // Predecessors and successors.
  //===--------------------------------------------------------------------===//

  // Predecessor iteration.
  using const_pred_iterator = PredecessorIterator<const StmtBlock>;
  const_pred_iterator pred_begin() const;
  const_pred_iterator pred_end() const;
  llvm::iterator_range<const_pred_iterator> getPredecessors() const;

  using pred_iterator = PredecessorIterator<StmtBlock>;
  pred_iterator pred_begin();
  pred_iterator pred_end();
  llvm::iterator_range<pred_iterator> getPredecessors();

  /// Return true if this block has no predecessors.
  bool hasNoPredecessors() const;

  /// If this block has exactly one predecessor, return it.  Otherwise, return
  /// null.
  ///
  /// Note that if a block has duplicate predecessors from a single block (e.g.
  /// if you have a conditional branch with the same block as the true/false
  /// destinations) is not considered to be a single predecessor.
  StmtBlock *getSinglePredecessor();

  const StmtBlock *getSinglePredecessor() const {
    return const_cast<StmtBlock *>(this)->getSinglePredecessor();
  }

  // Indexed successor access.
  unsigned getNumSuccessors() const;
  const StmtBlock *getSuccessor(unsigned i) const {
    return const_cast<StmtBlock *>(this)->getSuccessor(i);
  }
  StmtBlock *getSuccessor(unsigned i);

  // Successor iteration.
  using const_succ_iterator = SuccessorIterator<const StmtBlock>;
  const_succ_iterator succ_begin() const;
  const_succ_iterator succ_end() const;
  llvm::iterator_range<const_succ_iterator> getSuccessors() const;

  using succ_iterator = SuccessorIterator<StmtBlock>;
  succ_iterator succ_begin();
  succ_iterator succ_end();
  llvm::iterator_range<succ_iterator> getSuccessors();

  //===--------------------------------------------------------------------===//
  // Other
  //===--------------------------------------------------------------------===//

  /// Unlink this BasicBlock from its CFGFunction and delete it.
  void eraseFromFunction();

  /// Split the basic block into two basic blocks before the specified
  /// instruction or iterator.
  ///
  /// Note that all instructions BEFORE the specified iterator stay as part of
  /// the original basic block, an unconditional branch is added to the original
  /// block (going to the new block), and the rest of the instructions in the
  /// original block are moved to the new BB, including the old terminator.  The
  /// newly formed BasicBlock is returned.
  ///
  /// This function invalidates the specified iterator.
  BasicBlock *splitBasicBlock(iterator splitBefore);
  BasicBlock *splitBasicBlock(Instruction *splitBeforeInst) {
    return splitBasicBlock(iterator(splitBeforeInst));
  }

  /// getSublistAccess() - Returns pointer to member of statement list
  static StmtListType StmtBlock::*getSublistAccess(Statement *) {
    return &StmtBlock::statements;
  }

  void print(raw_ostream &os) const;
  void dump() const;

  /// Print out the name of the basic block without printing its body.
  /// NOTE: The printType argument is ignored.  We keep it for compatibility
  /// with LLVM dominator machinery that expects it to exist.
  void printAsOperand(raw_ostream &os, bool printType = true);

private:
  /// This is the parent function/IfStmt/ForStmt that owns this block.
  StmtBlockList *parent = nullptr;

  /// This is the list of statements in the block.
  StmtListType statements;

  /// This is the list of arguments to the block.
  std::vector<BlockArgument *> arguments;

  StmtBlock(const StmtBlock &) = delete;
  void operator=(const StmtBlock &) = delete;

  friend struct llvm::ilist_traits<StmtBlock>;
};

} // end namespace mlir

//===----------------------------------------------------------------------===//
// ilist_traits for StmtBlock
//===----------------------------------------------------------------------===//

namespace llvm {

template <>
struct ilist_traits<::mlir::StmtBlock>
    : public ilist_alloc_traits<::mlir::StmtBlock> {
  using StmtBlock = ::mlir::StmtBlock;
  using block_iterator = simple_ilist<::mlir::StmtBlock>::iterator;

  void addNodeToList(StmtBlock *block);
  void removeNodeFromList(StmtBlock *block);
  void transferNodesFromList(ilist_traits<StmtBlock> &otherList,
                             block_iterator first, block_iterator last);

private:
  mlir::StmtBlockList *getContainingBlockList();
};
} // end namespace llvm

namespace mlir {

/// This class contains a list of basic blocks and has a notion of the object it
/// is part of - an MLFunction or IfStmt or ForStmt.
class StmtBlockList {
public:
  explicit StmtBlockList(Function *container);
  explicit StmtBlockList(Statement *container);

  using BlockListType = llvm::iplist<StmtBlock>;
  BlockListType &getBlocks() { return blocks; }
  const BlockListType &getBlocks() const { return blocks; }

  // Iteration over the block in the function.
  using iterator = BlockListType::iterator;
  using const_iterator = BlockListType::const_iterator;
  using reverse_iterator = BlockListType::reverse_iterator;
  using const_reverse_iterator = BlockListType::const_reverse_iterator;

  iterator begin() { return blocks.begin(); }
  iterator end() { return blocks.end(); }
  const_iterator begin() const { return blocks.begin(); }
  const_iterator end() const { return blocks.end(); }
  reverse_iterator rbegin() { return blocks.rbegin(); }
  reverse_iterator rend() { return blocks.rend(); }
  const_reverse_iterator rbegin() const { return blocks.rbegin(); }
  const_reverse_iterator rend() const { return blocks.rend(); }

  bool empty() const { return blocks.empty(); }
  void push_back(StmtBlock *block) { blocks.push_back(block); }
  void push_front(StmtBlock *block) { blocks.push_front(block); }

  StmtBlock &back() { return blocks.back(); }
  const StmtBlock &back() const {
    return const_cast<StmtBlockList *>(this)->back();
  }

  StmtBlock &front() { return blocks.front(); }
  const StmtBlock &front() const {
    return const_cast<StmtBlockList *>(this)->front();
  }

  /// getSublistAccess() - Returns pointer to member of block list.
  static BlockListType StmtBlockList::*getSublistAccess(StmtBlock *) {
    return &StmtBlockList::blocks;
  }

  /// A StmtBlockList is part of a Function or and IfStmt/ForStmt.  If it is
  /// part of an IfStmt/ForStmt, then return it, otherwise return null.
  Statement *getContainingStmt();
  const Statement *getContainingStmt() const {
    return const_cast<StmtBlockList *>(this)->getContainingStmt();
  }

  /// A StmtBlockList is part of a Function or and IfStmt/ForStmt.  If it is
  /// part of an Function, then return it, otherwise return null.
  Function *getContainingFunction();
  const Function *getContainingFunction() const {
    return const_cast<StmtBlockList *>(this)->getContainingFunction();
  }

  // TODO(clattner): This is only to help ML -> CFG migration, remove in the
  // near future.  This makes StmtBlockList work more like BasicBlock did.
  CFGFunction *getFunction();
  const CFGFunction *getFunction() const {
    return const_cast<StmtBlockList *>(this)->getFunction();
  }

private:
  BlockListType blocks;

  /// This is the object we are part of.
  llvm::PointerUnion<Function *, Statement *> container;
};

//===----------------------------------------------------------------------===//
// Predecessors
//===----------------------------------------------------------------------===//

/// Implement a predecessor iterator as a forward iterator.  This works by
/// walking the use lists of the blocks.  The entries on this list are the
/// StmtBlockOperands that are embedded into terminator instructions.  From the
/// operand, we can get the terminator that contains it, and it's parent block
/// is the predecessor.
template <typename BlockType>
class PredecessorIterator
    : public llvm::iterator_facade_base<PredecessorIterator<BlockType>,
                                        std::forward_iterator_tag,
                                        BlockType *> {
public:
  PredecessorIterator(StmtBlockOperand *firstOperand)
      : bbUseIterator(firstOperand) {}

  PredecessorIterator &operator=(const PredecessorIterator &rhs) {
    bbUseIterator = rhs.bbUseIterator;
  }

  bool operator==(const PredecessorIterator &rhs) const {
    return bbUseIterator == rhs.bbUseIterator;
  }

  BlockType *operator*() const {
    // The use iterator points to an operand of a terminator.  The predecessor
    // we return is the block that the terminator is embedded into.
    return bbUseIterator.getUser()->getBlock();
  }

  PredecessorIterator &operator++() {
    ++bbUseIterator;
    return *this;
  }

  /// Get the successor number in the predecessor terminator.
  unsigned getSuccessorIndex() const {
    return bbUseIterator->getOperandNumber();
  }

private:
  using BBUseIterator = ValueUseIterator<StmtBlockOperand, OperationStmt>;
  BBUseIterator bbUseIterator;
};

inline auto StmtBlock::pred_begin() const -> const_pred_iterator {
  return const_pred_iterator((StmtBlockOperand *)getFirstUse());
}

inline auto StmtBlock::pred_end() const -> const_pred_iterator {
  return const_pred_iterator(nullptr);
}

inline auto StmtBlock::getPredecessors() const
    -> llvm::iterator_range<const_pred_iterator> {
  return {pred_begin(), pred_end()};
}

inline auto StmtBlock::pred_begin() -> pred_iterator {
  return pred_iterator((StmtBlockOperand *)getFirstUse());
}

inline auto StmtBlock::pred_end() -> pred_iterator {
  return pred_iterator(nullptr);
}

inline auto StmtBlock::getPredecessors()
    -> llvm::iterator_range<pred_iterator> {
  return {pred_begin(), pred_end()};
}

//===----------------------------------------------------------------------===//
// Successors
//===----------------------------------------------------------------------===//

/// This template implments the successor iterators for StmtBlock.
template <typename BlockType>
class SuccessorIterator final
    : public IndexedAccessorIterator<SuccessorIterator<BlockType>, BlockType,
                                     BlockType> {
public:
  /// Initializes the result iterator to the specified index.
  SuccessorIterator(BlockType *object, unsigned index)
      : IndexedAccessorIterator<SuccessorIterator<BlockType>, BlockType,
                                BlockType>(object, index) {}

  SuccessorIterator(const SuccessorIterator &other)
      : SuccessorIterator(other.object, other.index) {}

  /// Support converting to the const variant. This will be a no-op for const
  /// variant.
  operator SuccessorIterator<const BlockType>() const {
    return SuccessorIterator<const BlockType>(this->object, this->index);
  }

  BlockType *operator*() const {
    return this->object->getSuccessor(this->index);
  }

  /// Get the successor number in the terminator.
  unsigned getSuccessorIndex() const { return this->index; }
};

inline auto StmtBlock::succ_begin() const -> const_succ_iterator {
  return const_succ_iterator(this, 0);
}

inline auto StmtBlock::succ_end() const -> const_succ_iterator {
  return const_succ_iterator(this, getNumSuccessors());
}

inline auto StmtBlock::getSuccessors() const
    -> llvm::iterator_range<const_succ_iterator> {
  return {succ_begin(), succ_end()};
}

inline auto StmtBlock::succ_begin() -> succ_iterator {
  return succ_iterator(this, 0);
}

inline auto StmtBlock::succ_end() -> succ_iterator {
  return succ_iterator(this, getNumSuccessors());
}

inline auto StmtBlock::getSuccessors() -> llvm::iterator_range<succ_iterator> {
  return {succ_begin(), succ_end()};
}

} // end namespace mlir
#endif  // MLIR_IR_STMTBLOCK_H
