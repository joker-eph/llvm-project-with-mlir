//===- Block.h - MLIR Block and Region Classes ------------------*- C++ -*-===//
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
// This file defines Block and Region classes.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_BLOCK_H
#define MLIR_IR_BLOCK_H

#include "mlir/IR/Value.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"

//===----------------------------------------------------------------------===//
// ilist_traits for Instruction
//===----------------------------------------------------------------------===//

namespace llvm {
namespace ilist_detail {
// Explicitly define the node access for the instruction list so that we can
// break the dependence on the Instruction class in this header. This allows for
// instructions to have trailing Regions without a circular include
// dependence.
template <>
struct SpecificNodeAccess<
    typename compute_node_options<::mlir::Instruction>::type> : NodeAccess {
protected:
  using OptionsT = typename compute_node_options<mlir::Instruction>::type;
  using pointer = typename OptionsT::pointer;
  using const_pointer = typename OptionsT::const_pointer;
  using node_type = ilist_node_impl<OptionsT>;

  static node_type *getNodePtr(pointer N);
  static const node_type *getNodePtr(const_pointer N);

  static pointer getValuePtr(node_type *N);
  static const_pointer getValuePtr(const node_type *N);
};
} // end namespace ilist_detail

template <> struct ilist_traits<::mlir::Instruction> {
  using Instruction = ::mlir::Instruction;
  using inst_iterator = simple_ilist<Instruction>::iterator;

  static void deleteNode(Instruction *inst);
  void addNodeToList(Instruction *inst);
  void removeNodeFromList(Instruction *inst);
  void transferNodesFromList(ilist_traits<Instruction> &otherList,
                             inst_iterator first, inst_iterator last);

private:
  mlir::Block *getContainingBlock();
};
} // end namespace llvm

namespace mlir {
class BlockAndValueMapping;
class Region;
class Function;

using BlockOperand = IROperandImpl<Block>;

template <typename BlockType> class PredecessorIterator;
template <typename BlockType> class SuccessorIterator;

/// `Block` represents an ordered list of `Instruction`s.
class Block : public IRObjectWithUseList,
              public llvm::ilist_node_with_parent<Block, Region> {
public:
  explicit Block() {}
  ~Block();

  void clear() {
    // Drop all references from within this block.
    dropAllReferences();

    // Clear instructions in the reverse order so that uses are destroyed
    // before their defs.
    while (!empty())
      instructions.pop_back();
  }

  /// Blocks are maintained in a Region.
  Region *getParent() const { return parentValidInstOrderPair.getPointer(); }

  /// Returns the closest surrounding instruction that contains this block or
  /// nullptr if this is a top-level block.
  Instruction *getContainingInst();

  const Instruction *getContainingInst() const {
    return const_cast<Block *>(this)->getContainingInst();
  }

  /// Returns the function that this block is part of, even if the block is
  /// nested under an operation region.
  Function *getFunction() const;

  /// Insert this block (which must not already be in a function) right before
  /// the specified block.
  void insertBefore(Block *block);

  /// Unlink this Block from its Function and delete it.
  void eraseFromFunction();

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
  // Instruction list management
  //===--------------------------------------------------------------------===//

  /// This is the list of instructions in the block.
  using InstListType = llvm::iplist<Instruction>;
  InstListType &getInstructions() { return instructions; }
  const InstListType &getInstructions() const { return instructions; }

  // Iteration over the instructions in the block.
  using iterator = InstListType::iterator;
  using const_iterator = InstListType::const_iterator;
  using reverse_iterator = InstListType::reverse_iterator;
  using const_reverse_iterator = InstListType::const_reverse_iterator;

  iterator begin() { return instructions.begin(); }
  iterator end() { return instructions.end(); }
  const_iterator begin() const { return instructions.begin(); }
  const_iterator end() const { return instructions.end(); }
  reverse_iterator rbegin() { return instructions.rbegin(); }
  reverse_iterator rend() { return instructions.rend(); }
  const_reverse_iterator rbegin() const { return instructions.rbegin(); }
  const_reverse_iterator rend() const { return instructions.rend(); }

  bool empty() const { return instructions.empty(); }
  void push_back(Instruction *inst) { instructions.push_back(inst); }
  void push_front(Instruction *inst) { instructions.push_front(inst); }

  Instruction &back() { return instructions.back(); }
  const Instruction &back() const { return const_cast<Block *>(this)->back(); }
  Instruction &front() { return instructions.front(); }
  const Instruction &front() const {
    return const_cast<Block *>(this)->front();
  }

  /// Returns 'inst' if 'inst' lies in this block, or otherwise finds the
  /// ancestor instruction of 'inst' that lies in this block. Returns nullptr if
  /// the latter fails.
  /// TODO: This is very specific functionality that should live somewhere else,
  /// probably in Dominance.cpp.
  Instruction *findAncestorInstInBlock(Instruction *inst);
  const Instruction *findAncestorInstInBlock(const Instruction &inst) const {
    return const_cast<Block *>(this)->findAncestorInstInBlock(
        const_cast<Instruction *>(&inst));
  }

  /// This drops all operand uses from instructions within this block, which is
  /// an essential step in breaking cyclic dependences between references when
  /// they are to be deleted.
  void dropAllReferences();

  /// Returns true if the ordering of the child instructions is valid, false
  /// otherwise.
  bool isInstOrderValid() const { return parentValidInstOrderPair.getInt(); }

  /// Invalidates the current ordering of instructions.
  void invalidateInstOrder() {
    // Validate the current ordering.
    assert(!verifyInstOrder());
    parentValidInstOrderPair.setInt(false);
  }

  /// Verifies the current ordering of child instructions matches the
  /// validInstOrder flag. Returns false if the order is valid, true otherwise.
  bool verifyInstOrder() const;

  /// Recomputes the ordering of child instructions within the block.
  void recomputeInstOrder();

  //===--------------------------------------------------------------------===//
  // Terminator management
  //===--------------------------------------------------------------------===//

  /// Get the terminator instruction of this block. This function asserts that
  /// the block has a valid terminator instruction.
  Instruction *getTerminator();

  const Instruction *getTerminator() const {
    return const_cast<Block *>(this)->getTerminator();
  }

  //===--------------------------------------------------------------------===//
  // Predecessors and successors.
  //===--------------------------------------------------------------------===//

  // Predecessor iteration.
  using const_pred_iterator = PredecessorIterator<const Block>;
  const_pred_iterator pred_begin() const;
  const_pred_iterator pred_end() const;
  llvm::iterator_range<const_pred_iterator> getPredecessors() const;

  using pred_iterator = PredecessorIterator<Block>;
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
  Block *getSinglePredecessor();

  const Block *getSinglePredecessor() const {
    return const_cast<Block *>(this)->getSinglePredecessor();
  }

  // Indexed successor access.
  unsigned getNumSuccessors() const;
  const Block *getSuccessor(unsigned i) const {
    return const_cast<Block *>(this)->getSuccessor(i);
  }
  Block *getSuccessor(unsigned i);

  // Successor iteration.
  using const_succ_iterator = SuccessorIterator<const Block>;
  const_succ_iterator succ_begin() const;
  const_succ_iterator succ_end() const;
  llvm::iterator_range<const_succ_iterator> getSuccessors() const;

  using succ_iterator = SuccessorIterator<Block>;
  succ_iterator succ_begin();
  succ_iterator succ_end();
  llvm::iterator_range<succ_iterator> getSuccessors();

  //===--------------------------------------------------------------------===//
  // Instruction Walkers
  //===--------------------------------------------------------------------===//

  /// Walk the instructions of this block in preorder, calling the callback for
  /// each operation.
  void walk(const std::function<void(Instruction *)> &callback);

  /// Walk the instructions in the specified [begin, end) range of
  /// this block, calling the callback for each operation.
  void walk(Block::iterator begin, Block::iterator end,
            const std::function<void(Instruction *)> &callback);

  /// Walk the instructions in this block in postorder, calling the callback for
  /// each operation.
  void walkPostOrder(const std::function<void(Instruction *)> &callback);

  /// Walk the instructions in the specified [begin, end) range of this block
  /// in postorder, calling the callback for each operation.
  void walkPostOrder(Block::iterator begin, Block::iterator end,
                     const std::function<void(Instruction *)> &callback);

  //===--------------------------------------------------------------------===//
  // Other
  //===--------------------------------------------------------------------===//

  /// Split the block into two blocks before the specified instruction or
  /// iterator.
  ///
  /// Note that all instructions BEFORE the specified iterator stay as part of
  /// the original basic block, and the rest of the instructions in the original
  /// block are moved to the new block, including the old terminator.  The
  /// original block is left without a terminator.
  ///
  /// The newly formed Block is returned, and the specified iterator is
  /// invalidated.
  Block *splitBlock(iterator splitBefore);
  Block *splitBlock(Instruction *splitBeforeInst) {
    return splitBlock(iterator(splitBeforeInst));
  }

  /// Returns pointer to member of instruction list.
  static InstListType Block::*getSublistAccess(Instruction *) {
    return &Block::instructions;
  }

  void print(raw_ostream &os) const;
  void dump() const;

  /// Print out the name of the block without printing its body.
  /// NOTE: The printType argument is ignored.  We keep it for compatibility
  /// with LLVM dominator machinery that expects it to exist.
  void printAsOperand(raw_ostream &os, bool printType = true);

private:
  /// Pair of the parent object that owns this block and a bit that signifies if
  /// the instructions within this block have a valid ordering.
  llvm::PointerIntPair<Region *, /*IntBits=*/1, bool> parentValidInstOrderPair;

  /// This is the list of instructions in the block.
  InstListType instructions;

  /// This is the list of arguments to the block.
  std::vector<BlockArgument *> arguments;

  Block(const Block &) = delete;
  void operator=(const Block &) = delete;

  friend struct llvm::ilist_traits<Block>;
};

} // end namespace mlir

//===----------------------------------------------------------------------===//
// ilist_traits for Block
//===----------------------------------------------------------------------===//

namespace llvm {

template <>
struct ilist_traits<::mlir::Block> : public ilist_alloc_traits<::mlir::Block> {
  using Block = ::mlir::Block;
  using block_iterator = simple_ilist<::mlir::Block>::iterator;

  void addNodeToList(Block *block);
  void removeNodeFromList(Block *block);
  void transferNodesFromList(ilist_traits<Block> &otherList,
                             block_iterator first, block_iterator last);

private:
  mlir::Region *getContainingRegion();
};
} // end namespace llvm

namespace mlir {

/// This class contains a list of basic blocks and has a notion of the object it
/// is part of - a Function or an operation region.
class Region {
public:
  explicit Region(Function *container);
  explicit Region(Instruction *container);

  using RegionType = llvm::iplist<Block>;
  RegionType &getBlocks() { return blocks; }
  const RegionType &getBlocks() const { return blocks; }

  // Iteration over the block in the function.
  using iterator = RegionType::iterator;
  using const_iterator = RegionType::const_iterator;
  using reverse_iterator = RegionType::reverse_iterator;
  using const_reverse_iterator = RegionType::const_reverse_iterator;

  iterator begin() { return blocks.begin(); }
  iterator end() { return blocks.end(); }
  const_iterator begin() const { return blocks.begin(); }
  const_iterator end() const { return blocks.end(); }
  reverse_iterator rbegin() { return blocks.rbegin(); }
  reverse_iterator rend() { return blocks.rend(); }
  const_reverse_iterator rbegin() const { return blocks.rbegin(); }
  const_reverse_iterator rend() const { return blocks.rend(); }

  bool empty() const { return blocks.empty(); }
  void push_back(Block *block) { blocks.push_back(block); }
  void push_front(Block *block) { blocks.push_front(block); }

  Block &back() { return blocks.back(); }
  const Block &back() const { return const_cast<Region *>(this)->back(); }

  Block &front() { return blocks.front(); }
  const Block &front() const { return const_cast<Region *>(this)->front(); }

  /// getSublistAccess() - Returns pointer to member of region.
  static RegionType Region::*getSublistAccess(Block *) {
    return &Region::blocks;
  }

  /// A Region is either a function body or a part of an operation.  If it is
  /// part of an operation, then return the operation, otherwise return null.
  Instruction *getContainingInst();
  const Instruction *getContainingInst() const {
    return const_cast<Region *>(this)->getContainingInst();
  }

  /// A Region is either a function body or a part of an operation.  If it is
  /// a Function body, then return this function, otherwise return null.
  Function *getContainingFunction() const;

  /// Clone the internal blocks from this region into dest. Any
  /// cloned blocks are appended to the back of dest. If the mapper
  /// contains entries for block arguments, these arguments are not included
  /// in the respective cloned block.
  void cloneInto(Region *dest, BlockAndValueMapping &mapper,
                 MLIRContext *context) const;

private:
  RegionType blocks;

  /// This is the object we are part of.
  llvm::PointerUnion<Function *, Instruction *> container;
};

//===----------------------------------------------------------------------===//
// Predecessors
//===----------------------------------------------------------------------===//

/// Implement a predecessor iterator as a forward iterator.  This works by
/// walking the use lists of the blocks.  The entries on this list are the
/// BlockOperands that are embedded into terminator instructions.  From the
/// operand, we can get the terminator that contains it, and it's parent block
/// is the predecessor.
template <typename BlockType>
class PredecessorIterator
    : public llvm::iterator_facade_base<PredecessorIterator<BlockType>,
                                        std::forward_iterator_tag,
                                        BlockType *> {
public:
  PredecessorIterator(BlockOperand *firstOperand)
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
  using BBUseIterator = ValueUseIterator<BlockOperand>;
  BBUseIterator bbUseIterator;
};

inline auto Block::pred_begin() const -> const_pred_iterator {
  return const_pred_iterator((BlockOperand *)getFirstUse());
}

inline auto Block::pred_end() const -> const_pred_iterator {
  return const_pred_iterator(nullptr);
}

inline auto Block::getPredecessors() const
    -> llvm::iterator_range<const_pred_iterator> {
  return {pred_begin(), pred_end()};
}

inline auto Block::pred_begin() -> pred_iterator {
  return pred_iterator((BlockOperand *)getFirstUse());
}

inline auto Block::pred_end() -> pred_iterator {
  return pred_iterator(nullptr);
}

inline auto Block::getPredecessors() -> llvm::iterator_range<pred_iterator> {
  return {pred_begin(), pred_end()};
}

//===----------------------------------------------------------------------===//
// Successors
//===----------------------------------------------------------------------===//

/// This template implements the successor iterators for Block.
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

inline auto Block::succ_begin() const -> const_succ_iterator {
  return const_succ_iterator(this, 0);
}

inline auto Block::succ_end() const -> const_succ_iterator {
  return const_succ_iterator(this, getNumSuccessors());
}

inline auto Block::getSuccessors() const
    -> llvm::iterator_range<const_succ_iterator> {
  return {succ_begin(), succ_end()};
}

inline auto Block::succ_begin() -> succ_iterator {
  return succ_iterator(this, 0);
}

inline auto Block::succ_end() -> succ_iterator {
  return succ_iterator(this, getNumSuccessors());
}

inline auto Block::getSuccessors() -> llvm::iterator_range<succ_iterator> {
  return {succ_begin(), succ_end()};
}

} // end namespace mlir

#endif // MLIR_IR_BLOCK_H
