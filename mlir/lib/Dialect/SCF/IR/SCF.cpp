//===- SCF.cpp - Structured Control Flow Operations -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Conversion/ConvertToEmitC/ToEmitCInterface.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/Bufferization/IR/BufferDeallocationOpInterface.h"
#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/DeviceMappingInterface.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/ValueBoundsOpInterface.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace mlir;
using namespace mlir::scf;

#include "mlir/Dialect/SCF/IR/SCFOpsDialect.cpp.inc"

//===----------------------------------------------------------------------===//
// SCFDialect Dialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
struct SCFInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;
  // We don't have any special restrictions on what can be inlined into
  // destination regions (e.g. while/conditional bodies). Always allow it.
  bool isLegalToInline(Region *dest, Region *src, bool wouldBeCloned,
                       IRMapping &valueMapping) const final {
    return true;
  }
  // Operations in scf dialect are always legal to inline since they are
  // pure.
  bool isLegalToInline(Operation *, Region *, bool, IRMapping &) const final {
    return true;
  }
  // Handle the given inlined terminator by replacing it with a new operation
  // as necessary. Required when the region has only one block.
  void handleTerminator(Operation *op, ValueRange valuesToRepl) const final {
    auto retValOp = dyn_cast<scf::YieldOp>(op);
    if (!retValOp)
      return;

    for (auto retValue : llvm::zip(valuesToRepl, retValOp.getOperands())) {
      std::get<0>(retValue).replaceAllUsesWith(std::get<1>(retValue));
    }
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// SCFDialect
//===----------------------------------------------------------------------===//

void SCFDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "mlir/Dialect/SCF/IR/SCFOps.cpp.inc"
      >();
  addInterfaces<SCFInlinerInterface>();
  declarePromisedInterface<ConvertToEmitCPatternInterface, SCFDialect>();
  declarePromisedInterfaces<bufferization::BufferDeallocationOpInterface,
                            InParallelOp, ReduceReturnOp>();
  declarePromisedInterfaces<bufferization::BufferizableOpInterface, ConditionOp,
                            ExecuteRegionOp, ForOp, IfOp, IndexSwitchOp,
                            ForallOp, InParallelOp, WhileOp, YieldOp>();
  declarePromisedInterface<ValueBoundsOpInterface, ForOp>();
}

/// Default callback for IfOp builders. Inserts a yield without arguments.
void mlir::scf::buildTerminatedBody(OpBuilder &builder, Location loc) {
  builder.create<scf::YieldOp>(loc);
}

/// Verifies that the first block of the given `region` is terminated by a
/// TerminatorTy. Reports errors on the given operation if it is not the case.
template <typename TerminatorTy>
static TerminatorTy verifyAndGetTerminator(Operation *op, Region &region,
                                           StringRef errorMessage) {
  Operation *terminatorOperation = nullptr;
  if (!region.empty() && !region.front().empty()) {
    terminatorOperation = &region.front().back();
    if (auto yield = dyn_cast_or_null<TerminatorTy>(terminatorOperation))
      return yield;
  }
  auto diag = op->emitOpError(errorMessage);
  if (terminatorOperation)
    diag.attachNote(terminatorOperation->getLoc()) << "terminator here";
  return nullptr;
}

//===----------------------------------------------------------------------===//
// ExecuteRegionOp
//===----------------------------------------------------------------------===//

/// Replaces the given op with the contents of the given single-block region,
/// using the operands of the block terminator to replace operation results.
static void replaceOpWithRegion(PatternRewriter &rewriter, Operation *op,
                                Region &region, ValueRange blockArgs = {}) {
  assert(region.hasOneBlock() && "expected single-block region");
  Block *block = &region.front();
  Operation *terminator = block->getTerminator();
  ValueRange results = terminator->getOperands();
  rewriter.inlineBlockBefore(block, op, blockArgs);
  rewriter.replaceOp(op, results);
  rewriter.eraseOp(terminator);
}

///
/// (ssa-id `=`)? `execute_region` `->` function-result-type `{`
///    block+
/// `}`
///
/// Example:
///   scf.execute_region -> i32 {
///     %idx = load %rI[%i] : memref<128xi32>
///     return %idx : i32
///   }
///
ParseResult ExecuteRegionOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Introduce the body region and parse it.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, /*arguments=*/{}, /*argTypes=*/{}) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

void ExecuteRegionOp::print(OpAsmPrinter &p) {
  p.printOptionalArrowTypeList(getResultTypes());

  p << ' ';
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/true);

  p.printOptionalAttrDict((*this)->getAttrs());
}

LogicalResult ExecuteRegionOp::verify() {
  if (getRegion().empty())
    return emitOpError("region needs to have at least one block");
  if (getRegion().front().getNumArguments() > 0)
    return emitOpError("region cannot have any arguments");
  return success();
}

// Inline an ExecuteRegionOp if it only contains one block.
//     "test.foo"() : () -> ()
//      %v = scf.execute_region -> i64 {
//        %x = "test.val"() : () -> i64
//        scf.yield %x : i64
//      }
//      "test.bar"(%v) : (i64) -> ()
//
//  becomes
//
//     "test.foo"() : () -> ()
//     %x = "test.val"() : () -> i64
//     "test.bar"(%x) : (i64) -> ()
//
struct SingleBlockExecuteInliner : public OpRewritePattern<ExecuteRegionOp> {
  using OpRewritePattern<ExecuteRegionOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExecuteRegionOp op,
                                PatternRewriter &rewriter) const override {
    if (!op.getRegion().hasOneBlock())
      return failure();
    replaceOpWithRegion(rewriter, op, op.getRegion());
    return success();
  }
};

// Inline an ExecuteRegionOp if its parent can contain multiple blocks.
// TODO generalize the conditions for operations which can be inlined into.
// func @func_execute_region_elim() {
//     "test.foo"() : () -> ()
//     %v = scf.execute_region -> i64 {
//       %c = "test.cmp"() : () -> i1
//       cf.cond_br %c, ^bb2, ^bb3
//     ^bb2:
//       %x = "test.val1"() : () -> i64
//       cf.br ^bb4(%x : i64)
//     ^bb3:
//       %y = "test.val2"() : () -> i64
//       cf.br ^bb4(%y : i64)
//     ^bb4(%z : i64):
//       scf.yield %z : i64
//     }
//     "test.bar"(%v) : (i64) -> ()
//   return
// }
//
//  becomes
//
// func @func_execute_region_elim() {
//    "test.foo"() : () -> ()
//    %c = "test.cmp"() : () -> i1
//    cf.cond_br %c, ^bb1, ^bb2
//  ^bb1:  // pred: ^bb0
//    %x = "test.val1"() : () -> i64
//    cf.br ^bb3(%x : i64)
//  ^bb2:  // pred: ^bb0
//    %y = "test.val2"() : () -> i64
//    cf.br ^bb3(%y : i64)
//  ^bb3(%z: i64):  // 2 preds: ^bb1, ^bb2
//    "test.bar"(%z) : (i64) -> ()
//    return
//  }
//
struct MultiBlockExecuteInliner : public OpRewritePattern<ExecuteRegionOp> {
  using OpRewritePattern<ExecuteRegionOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExecuteRegionOp op,
                                PatternRewriter &rewriter) const override {
    if (!isa<FunctionOpInterface, ExecuteRegionOp>(op->getParentOp()))
      return failure();

    Block *prevBlock = op->getBlock();
    Block *postBlock = rewriter.splitBlock(prevBlock, op->getIterator());
    rewriter.setInsertionPointToEnd(prevBlock);

    rewriter.create<cf::BranchOp>(op.getLoc(), &op.getRegion().front());

    for (Block &blk : op.getRegion()) {
      if (YieldOp yieldOp = dyn_cast<YieldOp>(blk.getTerminator())) {
        rewriter.setInsertionPoint(yieldOp);
        rewriter.create<cf::BranchOp>(yieldOp.getLoc(), postBlock,
                                      yieldOp.getResults());
        rewriter.eraseOp(yieldOp);
      }
    }

    rewriter.inlineRegionBefore(op.getRegion(), postBlock);
    SmallVector<Value> blockArgs;

    for (auto res : op.getResults())
      blockArgs.push_back(postBlock->addArgument(res.getType(), res.getLoc()));

    rewriter.replaceOp(op, blockArgs);
    return success();
  }
};

void ExecuteRegionOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.add<SingleBlockExecuteInliner, MultiBlockExecuteInliner>(context);
}

void ExecuteRegionOp::getSuccessorRegions(
    RegionBranchPoint point, SmallVectorImpl<RegionSuccessor> &regions) {
  // If the predecessor is the ExecuteRegionOp, branch into the body.
  if (point.isParent()) {
    regions.push_back(RegionSuccessor(&getRegion()));
    return;
  }

  // Otherwise, the region branches back to the parent operation.
  regions.push_back(RegionSuccessor(getResults()));
}

//===----------------------------------------------------------------------===//
// ConditionOp
//===----------------------------------------------------------------------===//

MutableOperandRange
ConditionOp::getMutableSuccessorOperands(RegionBranchPoint point) {
  assert((point.isParent() || point == getParentOp().getAfter()) &&
         "condition op can only exit the loop or branch to the after"
         "region");
  // Pass all operands except the condition to the successor region.
  return getArgsMutable();
}

void ConditionOp::getSuccessorRegions(
    ArrayRef<Attribute> operands, SmallVectorImpl<RegionSuccessor> &regions) {
  FoldAdaptor adaptor(operands, *this);

  WhileOp whileOp = getParentOp();

  // Condition can either lead to the after region or back to the parent op
  // depending on whether the condition is true or not.
  auto boolAttr = dyn_cast_or_null<BoolAttr>(adaptor.getCondition());
  if (!boolAttr || boolAttr.getValue())
    regions.emplace_back(&whileOp.getAfter(),
                         whileOp.getAfter().getArguments());
  if (!boolAttr || !boolAttr.getValue())
    regions.emplace_back(whileOp.getResults());
}

//===----------------------------------------------------------------------===//
// ForOp
//===----------------------------------------------------------------------===//

void ForOp::build(OpBuilder &builder, OperationState &result, Value lb,
                  Value ub, Value step, ValueRange initArgs,
                  BodyBuilderFn bodyBuilder) {
  OpBuilder::InsertionGuard guard(builder);

  result.addOperands({lb, ub, step});
  result.addOperands(initArgs);
  for (Value v : initArgs)
    result.addTypes(v.getType());
  Type t = lb.getType();
  Region *bodyRegion = result.addRegion();
  Block *bodyBlock = builder.createBlock(bodyRegion);
  bodyBlock->addArgument(t, result.location);
  for (Value v : initArgs)
    bodyBlock->addArgument(v.getType(), v.getLoc());

  // Create the default terminator if the builder is not provided and if the
  // iteration arguments are not provided. Otherwise, leave this to the caller
  // because we don't know which values to return from the loop.
  if (initArgs.empty() && !bodyBuilder) {
    ForOp::ensureTerminator(*bodyRegion, builder, result.location);
  } else if (bodyBuilder) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(bodyBlock);
    bodyBuilder(builder, result.location, bodyBlock->getArgument(0),
                bodyBlock->getArguments().drop_front());
  }
}

LogicalResult ForOp::verify() {
  // Check that the number of init args and op results is the same.
  if (getInitArgs().size() != getNumResults())
    return emitOpError(
        "mismatch in number of loop-carried values and defined values");

  return success();
}

LogicalResult ForOp::verifyRegions() {
  // Check that the body defines as single block argument for the induction
  // variable.
  if (getInductionVar().getType() != getLowerBound().getType())
    return emitOpError(
        "expected induction variable to be same type as bounds and step");

  if (getNumRegionIterArgs() != getNumResults())
    return emitOpError(
        "mismatch in number of basic block args and defined values");

  auto initArgs = getInitArgs();
  auto iterArgs = getRegionIterArgs();
  auto opResults = getResults();
  unsigned i = 0;
  for (auto e : llvm::zip(initArgs, iterArgs, opResults)) {
    if (std::get<0>(e).getType() != std::get<2>(e).getType())
      return emitOpError() << "types mismatch between " << i
                           << "th iter operand and defined value";
    if (std::get<1>(e).getType() != std::get<2>(e).getType())
      return emitOpError() << "types mismatch between " << i
                           << "th iter region arg and defined value";

    ++i;
  }
  return success();
}

std::optional<SmallVector<Value>> ForOp::getLoopInductionVars() {
  return SmallVector<Value>{getInductionVar()};
}

std::optional<SmallVector<OpFoldResult>> ForOp::getLoopLowerBounds() {
  return SmallVector<OpFoldResult>{OpFoldResult(getLowerBound())};
}

std::optional<SmallVector<OpFoldResult>> ForOp::getLoopSteps() {
  return SmallVector<OpFoldResult>{OpFoldResult(getStep())};
}

std::optional<SmallVector<OpFoldResult>> ForOp::getLoopUpperBounds() {
  return SmallVector<OpFoldResult>{OpFoldResult(getUpperBound())};
}

std::optional<ResultRange> ForOp::getLoopResults() { return getResults(); }

/// Promotes the loop body of a forOp to its containing block if the forOp
/// it can be determined that the loop has a single iteration.
LogicalResult ForOp::promoteIfSingleIteration(RewriterBase &rewriter) {
  std::optional<int64_t> tripCount =
      constantTripCount(getLowerBound(), getUpperBound(), getStep());
  if (!tripCount.has_value() || tripCount != 1)
    return failure();

  // Replace all results with the yielded values.
  auto yieldOp = cast<scf::YieldOp>(getBody()->getTerminator());
  rewriter.replaceAllUsesWith(getResults(), getYieldedValues());

  // Replace block arguments with lower bound (replacement for IV) and
  // iter_args.
  SmallVector<Value> bbArgReplacements;
  bbArgReplacements.push_back(getLowerBound());
  llvm::append_range(bbArgReplacements, getInitArgs());

  // Move the loop body operations to the loop's containing block.
  rewriter.inlineBlockBefore(getBody(), getOperation()->getBlock(),
                             getOperation()->getIterator(), bbArgReplacements);

  // Erase the old terminator and the loop.
  rewriter.eraseOp(yieldOp);
  rewriter.eraseOp(*this);

  return success();
}

/// Prints the initialization list in the form of
///   <prefix>(%inner = %outer, %inner2 = %outer2, <...>)
/// where 'inner' values are assumed to be region arguments and 'outer' values
/// are regular SSA values.
static void printInitializationList(OpAsmPrinter &p,
                                    Block::BlockArgListType blocksArgs,
                                    ValueRange initializers,
                                    StringRef prefix = "") {
  assert(blocksArgs.size() == initializers.size() &&
         "expected same length of arguments and initializers");
  if (initializers.empty())
    return;

  p << prefix << '(';
  llvm::interleaveComma(llvm::zip(blocksArgs, initializers), p, [&](auto it) {
    p << std::get<0>(it) << " = " << std::get<1>(it);
  });
  p << ")";
}

void ForOp::print(OpAsmPrinter &p) {
  p << " " << getInductionVar() << " = " << getLowerBound() << " to "
    << getUpperBound() << " step " << getStep();

  printInitializationList(p, getRegionIterArgs(), getInitArgs(), " iter_args");
  if (!getInitArgs().empty())
    p << " -> (" << getInitArgs().getTypes() << ')';
  p << ' ';
  if (Type t = getInductionVar().getType(); !t.isIndex())
    p << " : " << t << ' ';
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/!getInitArgs().empty());
  p.printOptionalAttrDict((*this)->getAttrs());
}

ParseResult ForOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  Type type;

  OpAsmParser::Argument inductionVariable;
  OpAsmParser::UnresolvedOperand lb, ub, step;

  // Parse the induction variable followed by '='.
  if (parser.parseOperand(inductionVariable.ssaName) || parser.parseEqual() ||
      // Parse loop bounds.
      parser.parseOperand(lb) || parser.parseKeyword("to") ||
      parser.parseOperand(ub) || parser.parseKeyword("step") ||
      parser.parseOperand(step))
    return failure();

  // Parse the optional initial iteration arguments.
  SmallVector<OpAsmParser::Argument, 4> regionArgs;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> operands;
  regionArgs.push_back(inductionVariable);

  bool hasIterArgs = succeeded(parser.parseOptionalKeyword("iter_args"));
  if (hasIterArgs) {
    // Parse assignment list and results type list.
    if (parser.parseAssignmentList(regionArgs, operands) ||
        parser.parseArrowTypeList(result.types))
      return failure();
  }

  if (regionArgs.size() != result.types.size() + 1)
    return parser.emitError(
        parser.getNameLoc(),
        "mismatch in number of loop-carried values and defined values");

  // Parse optional type, else assume Index.
  if (parser.parseOptionalColon())
    type = builder.getIndexType();
  else if (parser.parseType(type))
    return failure();

  // Set block argument types, so that they are known when parsing the region.
  regionArgs.front().type = type;
  for (auto [iterArg, type] :
       llvm::zip_equal(llvm::drop_begin(regionArgs), result.types))
    iterArg.type = type;

  // Parse the body region.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, regionArgs))
    return failure();
  ForOp::ensureTerminator(*body, builder, result.location);

  // Resolve input operands. This should be done after parsing the region to
  // catch invalid IR where operands were defined inside of the region.
  if (parser.resolveOperand(lb, type, result.operands) ||
      parser.resolveOperand(ub, type, result.operands) ||
      parser.resolveOperand(step, type, result.operands))
    return failure();
  if (hasIterArgs) {
    for (auto argOperandType : llvm::zip_equal(llvm::drop_begin(regionArgs),
                                               operands, result.types)) {
      Type type = std::get<2>(argOperandType);
      std::get<0>(argOperandType).type = type;
      if (parser.resolveOperand(std::get<1>(argOperandType), type,
                                result.operands))
        return failure();
    }
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

SmallVector<Region *> ForOp::getLoopRegions() { return {&getRegion()}; }

Block::BlockArgListType ForOp::getRegionIterArgs() {
  return getBody()->getArguments().drop_front(getNumInductionVars());
}

MutableArrayRef<OpOperand> ForOp::getInitsMutable() {
  return getInitArgsMutable();
}

FailureOr<LoopLikeOpInterface>
ForOp::replaceWithAdditionalYields(RewriterBase &rewriter,
                                   ValueRange newInitOperands,
                                   bool replaceInitOperandUsesInLoop,
                                   const NewYieldValuesFn &newYieldValuesFn) {
  // Create a new loop before the existing one, with the extra operands.
  OpBuilder::InsertionGuard g(rewriter);
  rewriter.setInsertionPoint(getOperation());
  auto inits = llvm::to_vector(getInitArgs());
  inits.append(newInitOperands.begin(), newInitOperands.end());
  scf::ForOp newLoop = rewriter.create<scf::ForOp>(
      getLoc(), getLowerBound(), getUpperBound(), getStep(), inits,
      [](OpBuilder &, Location, Value, ValueRange) {});
  newLoop->setAttrs(getPrunedAttributeList(getOperation(), {}));

  // Generate the new yield values and append them to the scf.yield operation.
  auto yieldOp = cast<scf::YieldOp>(getBody()->getTerminator());
  ArrayRef<BlockArgument> newIterArgs =
      newLoop.getBody()->getArguments().take_back(newInitOperands.size());
  {
    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(yieldOp);
    SmallVector<Value> newYieldedValues =
        newYieldValuesFn(rewriter, getLoc(), newIterArgs);
    assert(newInitOperands.size() == newYieldedValues.size() &&
           "expected as many new yield values as new iter operands");
    rewriter.modifyOpInPlace(yieldOp, [&]() {
      yieldOp.getResultsMutable().append(newYieldedValues);
    });
  }

  // Move the loop body to the new op.
  rewriter.mergeBlocks(getBody(), newLoop.getBody(),
                       newLoop.getBody()->getArguments().take_front(
                           getBody()->getNumArguments()));

  if (replaceInitOperandUsesInLoop) {
    // Replace all uses of `newInitOperands` with the corresponding basic block
    // arguments.
    for (auto it : llvm::zip(newInitOperands, newIterArgs)) {
      rewriter.replaceUsesWithIf(std::get<0>(it), std::get<1>(it),
                                 [&](OpOperand &use) {
                                   Operation *user = use.getOwner();
                                   return newLoop->isProperAncestor(user);
                                 });
    }
  }

  // Replace the old loop.
  rewriter.replaceOp(getOperation(),
                     newLoop->getResults().take_front(getNumResults()));
  return cast<LoopLikeOpInterface>(newLoop.getOperation());
}

ForOp mlir::scf::getForInductionVarOwner(Value val) {
  auto ivArg = llvm::dyn_cast<BlockArgument>(val);
  if (!ivArg)
    return ForOp();
  assert(ivArg.getOwner() && "unlinked block argument");
  auto *containingOp = ivArg.getOwner()->getParentOp();
  return dyn_cast_or_null<ForOp>(containingOp);
}

OperandRange ForOp::getEntrySuccessorOperands(RegionBranchPoint point) {
  return getInitArgs();
}

void ForOp::getSuccessorRegions(RegionBranchPoint point,
                                SmallVectorImpl<RegionSuccessor> &regions) {
  // Both the operation itself and the region may be branching into the body or
  // back into the operation itself. It is possible for loop not to enter the
  // body.
  regions.push_back(RegionSuccessor(&getRegion(), getRegionIterArgs()));
  regions.push_back(RegionSuccessor(getResults()));
}

SmallVector<Region *> ForallOp::getLoopRegions() { return {&getRegion()}; }

/// Promotes the loop body of a forallOp to its containing block if it can be
/// determined that the loop has a single iteration.
LogicalResult scf::ForallOp::promoteIfSingleIteration(RewriterBase &rewriter) {
  for (auto [lb, ub, step] :
       llvm::zip(getMixedLowerBound(), getMixedUpperBound(), getMixedStep())) {
    auto tripCount = constantTripCount(lb, ub, step);
    if (!tripCount.has_value() || *tripCount != 1)
      return failure();
  }

  promote(rewriter, *this);
  return success();
}

Block::BlockArgListType ForallOp::getRegionIterArgs() {
  return getBody()->getArguments().drop_front(getRank());
}

MutableArrayRef<OpOperand> ForallOp::getInitsMutable() {
  return getOutputsMutable();
}

/// Promotes the loop body of a scf::ForallOp to its containing block.
void mlir::scf::promote(RewriterBase &rewriter, scf::ForallOp forallOp) {
  OpBuilder::InsertionGuard g(rewriter);
  scf::InParallelOp terminator = forallOp.getTerminator();

  // Replace block arguments with lower bounds (replacements for IVs) and
  // outputs.
  SmallVector<Value> bbArgReplacements = forallOp.getLowerBound(rewriter);
  bbArgReplacements.append(forallOp.getOutputs().begin(),
                           forallOp.getOutputs().end());

  // Move the loop body operations to the loop's containing block.
  rewriter.inlineBlockBefore(forallOp.getBody(), forallOp->getBlock(),
                             forallOp->getIterator(), bbArgReplacements);

  // Replace the terminator with tensor.insert_slice ops.
  rewriter.setInsertionPointAfter(forallOp);
  SmallVector<Value> results;
  results.reserve(forallOp.getResults().size());
  for (auto &yieldingOp : terminator.getYieldingOps()) {
    auto parallelInsertSliceOp =
        cast<tensor::ParallelInsertSliceOp>(yieldingOp);

    Value dst = parallelInsertSliceOp.getDest();
    Value src = parallelInsertSliceOp.getSource();
    if (llvm::isa<TensorType>(src.getType())) {
      results.push_back(rewriter.create<tensor::InsertSliceOp>(
          forallOp.getLoc(), dst.getType(), src, dst,
          parallelInsertSliceOp.getOffsets(), parallelInsertSliceOp.getSizes(),
          parallelInsertSliceOp.getStrides(),
          parallelInsertSliceOp.getStaticOffsets(),
          parallelInsertSliceOp.getStaticSizes(),
          parallelInsertSliceOp.getStaticStrides()));
    } else {
      llvm_unreachable("unsupported terminator");
    }
  }
  rewriter.replaceAllUsesWith(forallOp.getResults(), results);

  // Erase the old terminator and the loop.
  rewriter.eraseOp(terminator);
  rewriter.eraseOp(forallOp);
}

LoopNest mlir::scf::buildLoopNest(
    OpBuilder &builder, Location loc, ValueRange lbs, ValueRange ubs,
    ValueRange steps, ValueRange iterArgs,
    function_ref<ValueVector(OpBuilder &, Location, ValueRange, ValueRange)>
        bodyBuilder) {
  assert(lbs.size() == ubs.size() &&
         "expected the same number of lower and upper bounds");
  assert(lbs.size() == steps.size() &&
         "expected the same number of lower bounds and steps");

  // If there are no bounds, call the body-building function and return early.
  if (lbs.empty()) {
    ValueVector results =
        bodyBuilder ? bodyBuilder(builder, loc, ValueRange(), iterArgs)
                    : ValueVector();
    assert(results.size() == iterArgs.size() &&
           "loop nest body must return as many values as loop has iteration "
           "arguments");
    return LoopNest{{}, std::move(results)};
  }

  // First, create the loop structure iteratively using the body-builder
  // callback of `ForOp::build`. Do not create `YieldOp`s yet.
  OpBuilder::InsertionGuard guard(builder);
  SmallVector<scf::ForOp, 4> loops;
  SmallVector<Value, 4> ivs;
  loops.reserve(lbs.size());
  ivs.reserve(lbs.size());
  ValueRange currentIterArgs = iterArgs;
  Location currentLoc = loc;
  for (unsigned i = 0, e = lbs.size(); i < e; ++i) {
    auto loop = builder.create<scf::ForOp>(
        currentLoc, lbs[i], ubs[i], steps[i], currentIterArgs,
        [&](OpBuilder &nestedBuilder, Location nestedLoc, Value iv,
            ValueRange args) {
          ivs.push_back(iv);
          // It is safe to store ValueRange args because it points to block
          // arguments of a loop operation that we also own.
          currentIterArgs = args;
          currentLoc = nestedLoc;
        });
    // Set the builder to point to the body of the newly created loop. We don't
    // do this in the callback because the builder is reset when the callback
    // returns.
    builder.setInsertionPointToStart(loop.getBody());
    loops.push_back(loop);
  }

  // For all loops but the innermost, yield the results of the nested loop.
  for (unsigned i = 0, e = loops.size() - 1; i < e; ++i) {
    builder.setInsertionPointToEnd(loops[i].getBody());
    builder.create<scf::YieldOp>(loc, loops[i + 1].getResults());
  }

  // In the body of the innermost loop, call the body building function if any
  // and yield its results.
  builder.setInsertionPointToStart(loops.back().getBody());
  ValueVector results = bodyBuilder
                            ? bodyBuilder(builder, currentLoc, ivs,
                                          loops.back().getRegionIterArgs())
                            : ValueVector();
  assert(results.size() == iterArgs.size() &&
         "loop nest body must return as many values as loop has iteration "
         "arguments");
  builder.setInsertionPointToEnd(loops.back().getBody());
  builder.create<scf::YieldOp>(loc, results);

  // Return the loops.
  ValueVector nestResults;
  llvm::append_range(nestResults, loops.front().getResults());
  return LoopNest{std::move(loops), std::move(nestResults)};
}

LoopNest mlir::scf::buildLoopNest(
    OpBuilder &builder, Location loc, ValueRange lbs, ValueRange ubs,
    ValueRange steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilder) {
  // Delegate to the main function by wrapping the body builder.
  return buildLoopNest(builder, loc, lbs, ubs, steps, {},
                       [&bodyBuilder](OpBuilder &nestedBuilder,
                                      Location nestedLoc, ValueRange ivs,
                                      ValueRange) -> ValueVector {
                         if (bodyBuilder)
                           bodyBuilder(nestedBuilder, nestedLoc, ivs);
                         return {};
                       });
}

SmallVector<Value>
mlir::scf::replaceAndCastForOpIterArg(RewriterBase &rewriter, scf::ForOp forOp,
                                      OpOperand &operand, Value replacement,
                                      const ValueTypeCastFnTy &castFn) {
  assert(operand.getOwner() == forOp);
  Type oldType = operand.get().getType(), newType = replacement.getType();

  // 1. Create new iter operands, exactly 1 is replaced.
  assert(operand.getOperandNumber() >= forOp.getNumControlOperands() &&
         "expected an iter OpOperand");
  assert(operand.get().getType() != replacement.getType() &&
         "Expected a different type");
  SmallVector<Value> newIterOperands;
  for (OpOperand &opOperand : forOp.getInitArgsMutable()) {
    if (opOperand.getOperandNumber() == operand.getOperandNumber()) {
      newIterOperands.push_back(replacement);
      continue;
    }
    newIterOperands.push_back(opOperand.get());
  }

  // 2. Create the new forOp shell.
  scf::ForOp newForOp = rewriter.create<scf::ForOp>(
      forOp.getLoc(), forOp.getLowerBound(), forOp.getUpperBound(),
      forOp.getStep(), newIterOperands);
  newForOp->setAttrs(forOp->getAttrs());
  Block &newBlock = newForOp.getRegion().front();
  SmallVector<Value, 4> newBlockTransferArgs(newBlock.getArguments().begin(),
                                             newBlock.getArguments().end());

  // 3. Inject an incoming cast op at the beginning of the block for the bbArg
  // corresponding to the `replacement` value.
  OpBuilder::InsertionGuard g(rewriter);
  rewriter.setInsertionPointToStart(&newBlock);
  BlockArgument newRegionIterArg = newForOp.getTiedLoopRegionIterArg(
      &newForOp->getOpOperand(operand.getOperandNumber()));
  Value castIn = castFn(rewriter, newForOp.getLoc(), oldType, newRegionIterArg);
  newBlockTransferArgs[newRegionIterArg.getArgNumber()] = castIn;

  // 4. Steal the old block ops, mapping to the newBlockTransferArgs.
  Block &oldBlock = forOp.getRegion().front();
  rewriter.mergeBlocks(&oldBlock, &newBlock, newBlockTransferArgs);

  // 5. Inject an outgoing cast op at the end of the block and yield it instead.
  auto clonedYieldOp = cast<scf::YieldOp>(newBlock.getTerminator());
  rewriter.setInsertionPoint(clonedYieldOp);
  unsigned yieldIdx =
      newRegionIterArg.getArgNumber() - forOp.getNumInductionVars();
  Value castOut = castFn(rewriter, newForOp.getLoc(), newType,
                         clonedYieldOp.getOperand(yieldIdx));
  SmallVector<Value> newYieldOperands = clonedYieldOp.getOperands();
  newYieldOperands[yieldIdx] = castOut;
  rewriter.create<scf::YieldOp>(newForOp.getLoc(), newYieldOperands);
  rewriter.eraseOp(clonedYieldOp);

  // 6. Inject an outgoing cast op after the forOp.
  rewriter.setInsertionPointAfter(newForOp);
  SmallVector<Value> newResults = newForOp.getResults();
  newResults[yieldIdx] =
      castFn(rewriter, newForOp.getLoc(), oldType, newResults[yieldIdx]);

  return newResults;
}

namespace {
// Fold away ForOp iter arguments when:
// 1) The op yields the iter arguments.
// 2) The argument's corresponding outer region iterators (inputs) are yielded.
// 3) The iter arguments have no use and the corresponding (operation) results
// have no use.
//
// These arguments must be defined outside of the ForOp region and can just be
// forwarded after simplifying the op inits, yields and returns.
//
// The implementation uses `inlineBlockBefore` to steal the content of the
// original ForOp and avoid cloning.
struct ForOpIterArgsFolder : public OpRewritePattern<scf::ForOp> {
  using OpRewritePattern<scf::ForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::ForOp forOp,
                                PatternRewriter &rewriter) const final {
    bool canonicalize = false;

    // An internal flat vector of block transfer
    // arguments `newBlockTransferArgs` keeps the 1-1 mapping of original to
    // transformed block argument mappings. This plays the role of a
    // IRMapping for the particular use case of calling into
    // `inlineBlockBefore`.
    int64_t numResults = forOp.getNumResults();
    SmallVector<bool, 4> keepMask;
    keepMask.reserve(numResults);
    SmallVector<Value, 4> newBlockTransferArgs, newIterArgs, newYieldValues,
        newResultValues;
    newBlockTransferArgs.reserve(1 + numResults);
    newBlockTransferArgs.push_back(Value()); // iv placeholder with null value
    newIterArgs.reserve(forOp.getInitArgs().size());
    newYieldValues.reserve(numResults);
    newResultValues.reserve(numResults);
    DenseMap<std::pair<Value, Value>, std::pair<Value, Value>> initYieldToArg;
    for (auto [init, arg, result, yielded] :
         llvm::zip(forOp.getInitArgs(),       // iter from outside
                   forOp.getRegionIterArgs(), // iter inside region
                   forOp.getResults(),        // op results
                   forOp.getYieldedValues()   // iter yield
                   )) {
      // Forwarded is `true` when:
      // 1) The region `iter` argument is yielded.
      // 2) The region `iter` argument the corresponding input is yielded.
      // 3) The region `iter` argument has no use, and the corresponding op
      // result has no use.
      bool forwarded = (arg == yielded) || (init == yielded) ||
                       (arg.use_empty() && result.use_empty());
      if (forwarded) {
        canonicalize = true;
        keepMask.push_back(false);
        newBlockTransferArgs.push_back(init);
        newResultValues.push_back(init);
        continue;
      }

      // Check if a previous kept argument always has the same values for init
      // and yielded values.
      if (auto it = initYieldToArg.find({init, yielded});
          it != initYieldToArg.end()) {
        canonicalize = true;
        keepMask.push_back(false);
        auto [sameArg, sameResult] = it->second;
        rewriter.replaceAllUsesWith(arg, sameArg);
        rewriter.replaceAllUsesWith(result, sameResult);
        // The replacement value doesn't matter because there are no uses.
        newBlockTransferArgs.push_back(init);
        newResultValues.push_back(init);
        continue;
      }

      // This value is kept.
      initYieldToArg.insert({{init, yielded}, {arg, result}});
      keepMask.push_back(true);
      newIterArgs.push_back(init);
      newYieldValues.push_back(yielded);
      newBlockTransferArgs.push_back(Value()); // placeholder with null value
      newResultValues.push_back(Value());      // placeholder with null value
    }

    if (!canonicalize)
      return failure();

    scf::ForOp newForOp = rewriter.create<scf::ForOp>(
        forOp.getLoc(), forOp.getLowerBound(), forOp.getUpperBound(),
        forOp.getStep(), newIterArgs);
    newForOp->setAttrs(forOp->getAttrs());
    Block &newBlock = newForOp.getRegion().front();

    // Replace the null placeholders with newly constructed values.
    newBlockTransferArgs[0] = newBlock.getArgument(0); // iv
    for (unsigned idx = 0, collapsedIdx = 0, e = newResultValues.size();
         idx != e; ++idx) {
      Value &blockTransferArg = newBlockTransferArgs[1 + idx];
      Value &newResultVal = newResultValues[idx];
      assert((blockTransferArg && newResultVal) ||
             (!blockTransferArg && !newResultVal));
      if (!blockTransferArg) {
        blockTransferArg = newForOp.getRegionIterArgs()[collapsedIdx];
        newResultVal = newForOp.getResult(collapsedIdx++);
      }
    }

    Block &oldBlock = forOp.getRegion().front();
    assert(oldBlock.getNumArguments() == newBlockTransferArgs.size() &&
           "unexpected argument size mismatch");

    // No results case: the scf::ForOp builder already created a zero
    // result terminator. Merge before this terminator and just get rid of the
    // original terminator that has been merged in.
    if (newIterArgs.empty()) {
      auto newYieldOp = cast<scf::YieldOp>(newBlock.getTerminator());
      rewriter.inlineBlockBefore(&oldBlock, newYieldOp, newBlockTransferArgs);
      rewriter.eraseOp(newBlock.getTerminator()->getPrevNode());
      rewriter.replaceOp(forOp, newResultValues);
      return success();
    }

    // No terminator case: merge and rewrite the merged terminator.
    auto cloneFilteredTerminator = [&](scf::YieldOp mergedTerminator) {
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPoint(mergedTerminator);
      SmallVector<Value, 4> filteredOperands;
      filteredOperands.reserve(newResultValues.size());
      for (unsigned idx = 0, e = keepMask.size(); idx < e; ++idx)
        if (keepMask[idx])
          filteredOperands.push_back(mergedTerminator.getOperand(idx));
      rewriter.create<scf::YieldOp>(mergedTerminator.getLoc(),
                                    filteredOperands);
    };

    rewriter.mergeBlocks(&oldBlock, &newBlock, newBlockTransferArgs);
    auto mergedYieldOp = cast<scf::YieldOp>(newBlock.getTerminator());
    cloneFilteredTerminator(mergedYieldOp);
    rewriter.eraseOp(mergedYieldOp);
    rewriter.replaceOp(forOp, newResultValues);
    return success();
  }
};

/// Util function that tries to compute a constant diff between u and l.
/// Returns std::nullopt when the difference between two AffineValueMap is
/// dynamic.
static std::optional<int64_t> computeConstDiff(Value l, Value u) {
  IntegerAttr clb, cub;
  if (matchPattern(l, m_Constant(&clb)) && matchPattern(u, m_Constant(&cub))) {
    llvm::APInt lbValue = clb.getValue();
    llvm::APInt ubValue = cub.getValue();
    return (ubValue - lbValue).getSExtValue();
  }

  // Else a simple pattern match for x + c or c + x
  llvm::APInt diff;
  if (matchPattern(
          u, m_Op<arith::AddIOp>(matchers::m_Val(l), m_ConstantInt(&diff))) ||
      matchPattern(
          u, m_Op<arith::AddIOp>(m_ConstantInt(&diff), matchers::m_Val(l))))
    return diff.getSExtValue();
  return std::nullopt;
}

/// Rewriting pattern that erases loops that are known not to iterate, replaces
/// single-iteration loops with their bodies, and removes empty loops that
/// iterate at least once and only return values defined outside of the loop.
struct SimplifyTrivialLoops : public OpRewritePattern<ForOp> {
  using OpRewritePattern<ForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ForOp op,
                                PatternRewriter &rewriter) const override {
    // If the upper bound is the same as the lower bound, the loop does not
    // iterate, just remove it.
    if (op.getLowerBound() == op.getUpperBound()) {
      rewriter.replaceOp(op, op.getInitArgs());
      return success();
    }

    std::optional<int64_t> diff =
        computeConstDiff(op.getLowerBound(), op.getUpperBound());
    if (!diff)
      return failure();

    // If the loop is known to have 0 iterations, remove it.
    if (*diff <= 0) {
      rewriter.replaceOp(op, op.getInitArgs());
      return success();
    }

    std::optional<llvm::APInt> maybeStepValue = op.getConstantStep();
    if (!maybeStepValue)
      return failure();

    // If the loop is known to have 1 iteration, inline its body and remove the
    // loop.
    llvm::APInt stepValue = *maybeStepValue;
    if (stepValue.sge(*diff)) {
      SmallVector<Value, 4> blockArgs;
      blockArgs.reserve(op.getInitArgs().size() + 1);
      blockArgs.push_back(op.getLowerBound());
      llvm::append_range(blockArgs, op.getInitArgs());
      replaceOpWithRegion(rewriter, op, op.getRegion(), blockArgs);
      return success();
    }

    // Now we are left with loops that have more than 1 iterations.
    Block &block = op.getRegion().front();
    if (!llvm::hasSingleElement(block))
      return failure();
    // If the loop is empty, iterates at least once, and only returns values
    // defined outside of the loop, remove it and replace it with yield values.
    if (llvm::any_of(op.getYieldedValues(),
                     [&](Value v) { return !op.isDefinedOutsideOfLoop(v); }))
      return failure();
    rewriter.replaceOp(op, op.getYieldedValues());
    return success();
  }
};

/// Fold scf.for iter_arg/result pairs that go through incoming/ougoing
/// a tensor.cast op pair so as to pull the tensor.cast inside the scf.for:
///
/// ```
///   %0 = tensor.cast %t0 : tensor<32x1024xf32> to tensor<?x?xf32>
///   %1 = scf.for %i = %c0 to %c1024 step %c32 iter_args(%iter_t0 = %0)
///      -> (tensor<?x?xf32>) {
///     %2 = call @do(%iter_t0) : (tensor<?x?xf32>) -> tensor<?x?xf32>
///     scf.yield %2 : tensor<?x?xf32>
///   }
///   use_of(%1)
/// ```
///
/// folds into:
///
/// ```
///   %0 = scf.for %arg2 = %c0 to %c1024 step %c32 iter_args(%arg3 = %arg0)
///       -> (tensor<32x1024xf32>) {
///     %2 = tensor.cast %arg3 : tensor<32x1024xf32> to tensor<?x?xf32>
///     %3 = call @do(%2) : (tensor<?x?xf32>) -> tensor<?x?xf32>
///     %4 = tensor.cast %3 : tensor<?x?xf32> to tensor<32x1024xf32>
///     scf.yield %4 : tensor<32x1024xf32>
///   }
///   %1 = tensor.cast %0 : tensor<32x1024xf32> to tensor<?x?xf32>
///   use_of(%1)
/// ```
struct ForOpTensorCastFolder : public OpRewritePattern<ForOp> {
  using OpRewritePattern<ForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ForOp op,
                                PatternRewriter &rewriter) const override {
    for (auto it : llvm::zip(op.getInitArgsMutable(), op.getResults())) {
      OpOperand &iterOpOperand = std::get<0>(it);
      auto incomingCast = iterOpOperand.get().getDefiningOp<tensor::CastOp>();
      if (!incomingCast ||
          incomingCast.getSource().getType() == incomingCast.getType())
        continue;
      // If the dest type of the cast does not preserve static information in
      // the source type.
      if (!tensor::preservesStaticInformation(
              incomingCast.getDest().getType(),
              incomingCast.getSource().getType()))
        continue;
      if (!std::get<1>(it).hasOneUse())
        continue;

      // Create a new ForOp with that iter operand replaced.
      rewriter.replaceOp(
          op, replaceAndCastForOpIterArg(
                  rewriter, op, iterOpOperand, incomingCast.getSource(),
                  [](OpBuilder &b, Location loc, Type type, Value source) {
                    return b.create<tensor::CastOp>(loc, type, source);
                  }));
      return success();
    }
    return failure();
  }
};

} // namespace

void ForOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                        MLIRContext *context) {
  results.add<ForOpIterArgsFolder, SimplifyTrivialLoops, ForOpTensorCastFolder>(
      context);
}

std::optional<APInt> ForOp::getConstantStep() {
  IntegerAttr step;
  if (matchPattern(getStep(), m_Constant(&step)))
    return step.getValue();
  return {};
}

std::optional<MutableArrayRef<OpOperand>> ForOp::getYieldedValuesMutable() {
  return cast<scf::YieldOp>(getBody()->getTerminator()).getResultsMutable();
}

Speculation::Speculatability ForOp::getSpeculatability() {
  // `scf.for (I = Start; I < End; I += 1)` terminates for all values of Start
  // and End.
  if (auto constantStep = getConstantStep())
    if (*constantStep == 1)
      return Speculation::RecursivelySpeculatable;

  // For Step != 1, the loop may not terminate.  We can add more smarts here if
  // needed.
  return Speculation::NotSpeculatable;
}

//===----------------------------------------------------------------------===//
// ForallOp
//===----------------------------------------------------------------------===//

LogicalResult ForallOp::verify() {
  unsigned numLoops = getRank();
  // Check number of outputs.
  if (getNumResults() != getOutputs().size())
    return emitOpError("produces ")
           << getNumResults() << " results, but has only "
           << getOutputs().size() << " outputs";

  // Check that the body defines block arguments for thread indices and outputs.
  auto *body = getBody();
  if (body->getNumArguments() != numLoops + getOutputs().size())
    return emitOpError("region expects ") << numLoops << " arguments";
  for (int64_t i = 0; i < numLoops; ++i)
    if (!body->getArgument(i).getType().isIndex())
      return emitOpError("expects ")
             << i << "-th block argument to be an index";
  for (unsigned i = 0; i < getOutputs().size(); ++i)
    if (body->getArgument(i + numLoops).getType() != getOutputs()[i].getType())
      return emitOpError("type mismatch between ")
             << i << "-th output and corresponding block argument";
  if (getMapping().has_value() && !getMapping()->empty()) {
    if (getDeviceMappingAttrs().size() != numLoops)
      return emitOpError() << "mapping attribute size must match op rank";
    if (failed(getDeviceMaskingAttr()))
      return emitOpError() << getMappingAttrName()
                           << " supports at most one device masking attribute";
  }

  // Verify mixed static/dynamic control variables.
  Operation *op = getOperation();
  if (failed(verifyListOfOperandsOrIntegers(op, "lower bound", numLoops,
                                            getStaticLowerBound(),
                                            getDynamicLowerBound())))
    return failure();
  if (failed(verifyListOfOperandsOrIntegers(op, "upper bound", numLoops,
                                            getStaticUpperBound(),
                                            getDynamicUpperBound())))
    return failure();
  if (failed(verifyListOfOperandsOrIntegers(op, "step", numLoops,
                                            getStaticStep(), getDynamicStep())))
    return failure();

  return success();
}

void ForallOp::print(OpAsmPrinter &p) {
  Operation *op = getOperation();
  p << " (" << getInductionVars();
  if (isNormalized()) {
    p << ") in ";
    printDynamicIndexList(p, op, getDynamicUpperBound(), getStaticUpperBound(),
                          /*valueTypes=*/{}, /*scalables=*/{},
                          OpAsmParser::Delimiter::Paren);
  } else {
    p << ") = ";
    printDynamicIndexList(p, op, getDynamicLowerBound(), getStaticLowerBound(),
                          /*valueTypes=*/{}, /*scalables=*/{},
                          OpAsmParser::Delimiter::Paren);
    p << " to ";
    printDynamicIndexList(p, op, getDynamicUpperBound(), getStaticUpperBound(),
                          /*valueTypes=*/{}, /*scalables=*/{},
                          OpAsmParser::Delimiter::Paren);
    p << " step ";
    printDynamicIndexList(p, op, getDynamicStep(), getStaticStep(),
                          /*valueTypes=*/{}, /*scalables=*/{},
                          OpAsmParser::Delimiter::Paren);
  }
  printInitializationList(p, getRegionOutArgs(), getOutputs(), " shared_outs");
  p << " ";
  if (!getRegionOutArgs().empty())
    p << "-> (" << getResultTypes() << ") ";
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/getNumResults() > 0);
  p.printOptionalAttrDict(op->getAttrs(), {getOperandSegmentSizesAttrName(),
                                           getStaticLowerBoundAttrName(),
                                           getStaticUpperBoundAttrName(),
                                           getStaticStepAttrName()});
}

ParseResult ForallOp::parse(OpAsmParser &parser, OperationState &result) {
  OpBuilder b(parser.getContext());
  auto indexType = b.getIndexType();

  // Parse an opening `(` followed by thread index variables followed by `)`
  // TODO: when we can refer to such "induction variable"-like handles from the
  // declarative assembly format, we can implement the parser as a custom hook.
  SmallVector<OpAsmParser::Argument, 4> ivs;
  if (parser.parseArgumentList(ivs, OpAsmParser::Delimiter::Paren))
    return failure();

  DenseI64ArrayAttr staticLbs, staticUbs, staticSteps;
  SmallVector<OpAsmParser::UnresolvedOperand> dynamicLbs, dynamicUbs,
      dynamicSteps;
  if (succeeded(parser.parseOptionalKeyword("in"))) {
    // Parse upper bounds.
    if (parseDynamicIndexList(parser, dynamicUbs, staticUbs,
                              /*valueTypes=*/nullptr,
                              OpAsmParser::Delimiter::Paren) ||
        parser.resolveOperands(dynamicUbs, indexType, result.operands))
      return failure();

    unsigned numLoops = ivs.size();
    staticLbs = b.getDenseI64ArrayAttr(SmallVector<int64_t>(numLoops, 0));
    staticSteps = b.getDenseI64ArrayAttr(SmallVector<int64_t>(numLoops, 1));
  } else {
    // Parse lower bounds.
    if (parser.parseEqual() ||
        parseDynamicIndexList(parser, dynamicLbs, staticLbs,
                              /*valueTypes=*/nullptr,
                              OpAsmParser::Delimiter::Paren) ||

        parser.resolveOperands(dynamicLbs, indexType, result.operands))
      return failure();

    // Parse upper bounds.
    if (parser.parseKeyword("to") ||
        parseDynamicIndexList(parser, dynamicUbs, staticUbs,
                              /*valueTypes=*/nullptr,
                              OpAsmParser::Delimiter::Paren) ||
        parser.resolveOperands(dynamicUbs, indexType, result.operands))
      return failure();

    // Parse step values.
    if (parser.parseKeyword("step") ||
        parseDynamicIndexList(parser, dynamicSteps, staticSteps,
                              /*valueTypes=*/nullptr,
                              OpAsmParser::Delimiter::Paren) ||
        parser.resolveOperands(dynamicSteps, indexType, result.operands))
      return failure();
  }

  // Parse out operands and results.
  SmallVector<OpAsmParser::Argument, 4> regionOutArgs;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> outOperands;
  SMLoc outOperandsLoc = parser.getCurrentLocation();
  if (succeeded(parser.parseOptionalKeyword("shared_outs"))) {
    if (outOperands.size() != result.types.size())
      return parser.emitError(outOperandsLoc,
                              "mismatch between out operands and types");
    if (parser.parseAssignmentList(regionOutArgs, outOperands) ||
        parser.parseOptionalArrowTypeList(result.types) ||
        parser.resolveOperands(outOperands, result.types, outOperandsLoc,
                               result.operands))
      return failure();
  }

  // Parse region.
  SmallVector<OpAsmParser::Argument, 4> regionArgs;
  std::unique_ptr<Region> region = std::make_unique<Region>();
  for (auto &iv : ivs) {
    iv.type = b.getIndexType();
    regionArgs.push_back(iv);
  }
  for (const auto &it : llvm::enumerate(regionOutArgs)) {
    auto &out = it.value();
    out.type = result.types[it.index()];
    regionArgs.push_back(out);
  }
  if (parser.parseRegion(*region, regionArgs))
    return failure();

  // Ensure terminator and move region.
  ForallOp::ensureTerminator(*region, b, result.location);
  result.addRegion(std::move(region));

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  result.addAttribute("staticLowerBound", staticLbs);
  result.addAttribute("staticUpperBound", staticUbs);
  result.addAttribute("staticStep", staticSteps);
  result.addAttribute("operandSegmentSizes",
                      parser.getBuilder().getDenseI32ArrayAttr(
                          {static_cast<int32_t>(dynamicLbs.size()),
                           static_cast<int32_t>(dynamicUbs.size()),
                           static_cast<int32_t>(dynamicSteps.size()),
                           static_cast<int32_t>(outOperands.size())}));
  return success();
}

// Builder that takes loop bounds.
void ForallOp::build(
    mlir::OpBuilder &b, mlir::OperationState &result,
    ArrayRef<OpFoldResult> lbs, ArrayRef<OpFoldResult> ubs,
    ArrayRef<OpFoldResult> steps, ValueRange outputs,
    std::optional<ArrayAttr> mapping,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  SmallVector<int64_t> staticLbs, staticUbs, staticSteps;
  SmallVector<Value> dynamicLbs, dynamicUbs, dynamicSteps;
  dispatchIndexOpFoldResults(lbs, dynamicLbs, staticLbs);
  dispatchIndexOpFoldResults(ubs, dynamicUbs, staticUbs);
  dispatchIndexOpFoldResults(steps, dynamicSteps, staticSteps);

  result.addOperands(dynamicLbs);
  result.addOperands(dynamicUbs);
  result.addOperands(dynamicSteps);
  result.addOperands(outputs);
  result.addTypes(TypeRange(outputs));

  result.addAttribute(getStaticLowerBoundAttrName(result.name),
                      b.getDenseI64ArrayAttr(staticLbs));
  result.addAttribute(getStaticUpperBoundAttrName(result.name),
                      b.getDenseI64ArrayAttr(staticUbs));
  result.addAttribute(getStaticStepAttrName(result.name),
                      b.getDenseI64ArrayAttr(staticSteps));
  result.addAttribute(
      "operandSegmentSizes",
      b.getDenseI32ArrayAttr({static_cast<int32_t>(dynamicLbs.size()),
                              static_cast<int32_t>(dynamicUbs.size()),
                              static_cast<int32_t>(dynamicSteps.size()),
                              static_cast<int32_t>(outputs.size())}));
  if (mapping.has_value()) {
    result.addAttribute(ForallOp::getMappingAttrName(result.name),
                        mapping.value());
  }

  Region *bodyRegion = result.addRegion();
  OpBuilder::InsertionGuard g(b);
  b.createBlock(bodyRegion);
  Block &bodyBlock = bodyRegion->front();

  // Add block arguments for indices and outputs.
  bodyBlock.addArguments(
      SmallVector<Type>(lbs.size(), b.getIndexType()),
      SmallVector<Location>(staticLbs.size(), result.location));
  bodyBlock.addArguments(
      TypeRange(outputs),
      SmallVector<Location>(outputs.size(), result.location));

  b.setInsertionPointToStart(&bodyBlock);
  if (!bodyBuilderFn) {
    ForallOp::ensureTerminator(*bodyRegion, b, result.location);
    return;
  }
  bodyBuilderFn(b, result.location, bodyBlock.getArguments());
}

// Builder that takes loop bounds.
void ForallOp::build(
    mlir::OpBuilder &b, mlir::OperationState &result,
    ArrayRef<OpFoldResult> ubs, ValueRange outputs,
    std::optional<ArrayAttr> mapping,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  unsigned numLoops = ubs.size();
  SmallVector<OpFoldResult> lbs(numLoops, b.getIndexAttr(0));
  SmallVector<OpFoldResult> steps(numLoops, b.getIndexAttr(1));
  build(b, result, lbs, ubs, steps, outputs, mapping, bodyBuilderFn);
}

// Checks if the lbs are zeros and steps are ones.
bool ForallOp::isNormalized() {
  auto allEqual = [](ArrayRef<OpFoldResult> results, int64_t val) {
    return llvm::all_of(results, [&](OpFoldResult ofr) {
      auto intValue = getConstantIntValue(ofr);
      return intValue.has_value() && intValue == val;
    });
  };
  return allEqual(getMixedLowerBound(), 0) && allEqual(getMixedStep(), 1);
}

InParallelOp ForallOp::getTerminator() {
  return cast<InParallelOp>(getBody()->getTerminator());
}

SmallVector<Operation *> ForallOp::getCombiningOps(BlockArgument bbArg) {
  SmallVector<Operation *> storeOps;
  InParallelOp inParallelOp = getTerminator();
  for (Operation &yieldOp : inParallelOp.getYieldingOps()) {
    if (auto parallelInsertSliceOp =
            dyn_cast<tensor::ParallelInsertSliceOp>(yieldOp);
        parallelInsertSliceOp && parallelInsertSliceOp.getDest() == bbArg) {
      storeOps.push_back(parallelInsertSliceOp);
    }
  }
  return storeOps;
}

SmallVector<DeviceMappingAttrInterface> ForallOp::getDeviceMappingAttrs() {
  SmallVector<DeviceMappingAttrInterface> res;
  if (!getMapping())
    return res;
  for (auto attr : getMapping()->getValue()) {
    auto m = dyn_cast<DeviceMappingAttrInterface>(attr);
    if (m)
      res.push_back(m);
  }
  return res;
}

FailureOr<DeviceMaskingAttrInterface> ForallOp::getDeviceMaskingAttr() {
  DeviceMaskingAttrInterface res;
  if (!getMapping())
    return res;
  for (auto attr : getMapping()->getValue()) {
    auto m = dyn_cast<DeviceMaskingAttrInterface>(attr);
    if (m && res)
      return failure();
    if (m)
      res = m;
  }
  return res;
}

bool ForallOp::usesLinearMapping() {
  SmallVector<DeviceMappingAttrInterface> ifaces = getDeviceMappingAttrs();
  if (ifaces.empty())
    return false;
  return ifaces.front().isLinearMapping();
}

std::optional<SmallVector<Value>> ForallOp::getLoopInductionVars() {
  return SmallVector<Value>{getBody()->getArguments().take_front(getRank())};
}

// Get lower bounds as OpFoldResult.
std::optional<SmallVector<OpFoldResult>> ForallOp::getLoopLowerBounds() {
  Builder b(getOperation()->getContext());
  return getMixedValues(getStaticLowerBound(), getDynamicLowerBound(), b);
}

// Get upper bounds as OpFoldResult.
std::optional<SmallVector<OpFoldResult>> ForallOp::getLoopUpperBounds() {
  Builder b(getOperation()->getContext());
  return getMixedValues(getStaticUpperBound(), getDynamicUpperBound(), b);
}

// Get steps as OpFoldResult.
std::optional<SmallVector<OpFoldResult>> ForallOp::getLoopSteps() {
  Builder b(getOperation()->getContext());
  return getMixedValues(getStaticStep(), getDynamicStep(), b);
}

ForallOp mlir::scf::getForallOpThreadIndexOwner(Value val) {
  auto tidxArg = llvm::dyn_cast<BlockArgument>(val);
  if (!tidxArg)
    return ForallOp();
  assert(tidxArg.getOwner() && "unlinked block argument");
  auto *containingOp = tidxArg.getOwner()->getParentOp();
  return dyn_cast<ForallOp>(containingOp);
}

namespace {
/// Fold tensor.dim(forall shared_outs(... = %t)) to tensor.dim(%t).
struct DimOfForallOp : public OpRewritePattern<tensor::DimOp> {
  using OpRewritePattern<tensor::DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::DimOp dimOp,
                                PatternRewriter &rewriter) const final {
    auto forallOp = dimOp.getSource().getDefiningOp<ForallOp>();
    if (!forallOp)
      return failure();
    Value sharedOut =
        forallOp.getTiedOpOperand(llvm::cast<OpResult>(dimOp.getSource()))
            ->get();
    rewriter.modifyOpInPlace(
        dimOp, [&]() { dimOp.getSourceMutable().assign(sharedOut); });
    return success();
  }
};

class ForallOpControlOperandsFolder : public OpRewritePattern<ForallOp> {
public:
  using OpRewritePattern<ForallOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ForallOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<OpFoldResult> mixedLowerBound(op.getMixedLowerBound());
    SmallVector<OpFoldResult> mixedUpperBound(op.getMixedUpperBound());
    SmallVector<OpFoldResult> mixedStep(op.getMixedStep());
    if (failed(foldDynamicIndexList(mixedLowerBound)) &&
        failed(foldDynamicIndexList(mixedUpperBound)) &&
        failed(foldDynamicIndexList(mixedStep)))
      return failure();

    rewriter.modifyOpInPlace(op, [&]() {
      SmallVector<Value> dynamicLowerBound, dynamicUpperBound, dynamicStep;
      SmallVector<int64_t> staticLowerBound, staticUpperBound, staticStep;
      dispatchIndexOpFoldResults(mixedLowerBound, dynamicLowerBound,
                                 staticLowerBound);
      op.getDynamicLowerBoundMutable().assign(dynamicLowerBound);
      op.setStaticLowerBound(staticLowerBound);

      dispatchIndexOpFoldResults(mixedUpperBound, dynamicUpperBound,
                                 staticUpperBound);
      op.getDynamicUpperBoundMutable().assign(dynamicUpperBound);
      op.setStaticUpperBound(staticUpperBound);

      dispatchIndexOpFoldResults(mixedStep, dynamicStep, staticStep);
      op.getDynamicStepMutable().assign(dynamicStep);
      op.setStaticStep(staticStep);

      op->setAttr(ForallOp::getOperandSegmentSizeAttr(),
                  rewriter.getDenseI32ArrayAttr(
                      {static_cast<int32_t>(dynamicLowerBound.size()),
                       static_cast<int32_t>(dynamicUpperBound.size()),
                       static_cast<int32_t>(dynamicStep.size()),
                       static_cast<int32_t>(op.getNumResults())}));
    });
    return success();
  }
};

/// The following canonicalization pattern folds the iter arguments of
/// scf.forall op if :-
/// 1. The corresponding result has zero uses.
/// 2. The iter argument is NOT being modified within the loop body.
/// uses.
///
/// Example of first case :-
///  INPUT:
///   %res:3 = scf.forall ... shared_outs(%arg0 = %a, %arg1 = %b, %arg2 = %c)
///            {
///                ...
///                <SOME USE OF %arg0>
///                <SOME USE OF %arg1>
///                <SOME USE OF %arg2>
///                ...
///                scf.forall.in_parallel {
///                    <STORE OP WITH DESTINATION %arg1>
///                    <STORE OP WITH DESTINATION %arg0>
///                    <STORE OP WITH DESTINATION %arg2>
///                }
///             }
///   return %res#1
///
///  OUTPUT:
///   %res:3 = scf.forall ... shared_outs(%new_arg0 = %b)
///            {
///                ...
///                <SOME USE OF %a>
///                <SOME USE OF %new_arg0>
///                <SOME USE OF %c>
///                ...
///                scf.forall.in_parallel {
///                    <STORE OP WITH DESTINATION %new_arg0>
///                }
///             }
///   return %res
///
/// NOTE: 1. All uses of the folded shared_outs (iter argument) within the
///          scf.forall is replaced by their corresponding operands.
///       2. Even if there are <STORE OP WITH DESTINATION *> ops within the body
///          of the scf.forall besides within scf.forall.in_parallel terminator,
///          this canonicalization remains valid. For more details, please refer
///          to :
///          https://github.com/llvm/llvm-project/pull/90189#discussion_r1589011124
///       3. TODO(avarma): Generalize it for other store ops. Currently it
///          handles tensor.parallel_insert_slice ops only.
///
/// Example of second case :-
///  INPUT:
///   %res:2 = scf.forall ... shared_outs(%arg0 = %a, %arg1 = %b)
///            {
///                ...
///                <SOME USE OF %arg0>
///                <SOME USE OF %arg1>
///                ...
///                scf.forall.in_parallel {
///                    <STORE OP WITH DESTINATION %arg1>
///                }
///             }
///   return %res#0, %res#1
///
///  OUTPUT:
///   %res = scf.forall ... shared_outs(%new_arg0 = %b)
///            {
///                ...
///                <SOME USE OF %a>
///                <SOME USE OF %new_arg0>
///                ...
///                scf.forall.in_parallel {
///                    <STORE OP WITH DESTINATION %new_arg0>
///                }
///             }
///   return %a, %res
struct ForallOpIterArgsFolder : public OpRewritePattern<ForallOp> {
  using OpRewritePattern<ForallOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ForallOp forallOp,
                                PatternRewriter &rewriter) const final {
    // Step 1: For a given i-th result of scf.forall, check the following :-
    //         a. If it has any use.
    //         b. If the corresponding iter argument is being modified within
    //            the loop, i.e. has at least one store op with the iter arg as
    //            its destination operand. For this we use
    //            ForallOp::getCombiningOps(iter_arg).
    //
    //         Based on the check we maintain the following :-
    //         a. `resultToDelete` - i-th result of scf.forall that'll be
    //            deleted.
    //         b. `resultToReplace` - i-th result of the old scf.forall
    //            whose uses will be replaced by the new scf.forall.
    //         c. `newOuts` - the shared_outs' operand of the new scf.forall
    //            corresponding to the i-th result with at least one use.
    SetVector<OpResult> resultToDelete;
    SmallVector<Value> resultToReplace;
    SmallVector<Value> newOuts;
    for (OpResult result : forallOp.getResults()) {
      OpOperand *opOperand = forallOp.getTiedOpOperand(result);
      BlockArgument blockArg = forallOp.getTiedBlockArgument(opOperand);
      if (result.use_empty() || forallOp.getCombiningOps(blockArg).empty()) {
        resultToDelete.insert(result);
      } else {
        resultToReplace.push_back(result);
        newOuts.push_back(opOperand->get());
      }
    }

    // Return early if all results of scf.forall have at least one use and being
    // modified within the loop.
    if (resultToDelete.empty())
      return failure();

    // Step 2: For the the i-th result, do the following :-
    //         a. Fetch the corresponding BlockArgument.
    //         b. Look for store ops (currently tensor.parallel_insert_slice)
    //            with the BlockArgument as its destination operand.
    //         c. Remove the operations fetched in b.
    for (OpResult result : resultToDelete) {
      OpOperand *opOperand = forallOp.getTiedOpOperand(result);
      BlockArgument blockArg = forallOp.getTiedBlockArgument(opOperand);
      SmallVector<Operation *> combiningOps =
          forallOp.getCombiningOps(blockArg);
      for (Operation *combiningOp : combiningOps)
        rewriter.eraseOp(combiningOp);
    }

    // Step 3. Create a new scf.forall op with the new shared_outs' operands
    //         fetched earlier
    auto newForallOp = rewriter.create<scf::ForallOp>(
        forallOp.getLoc(), forallOp.getMixedLowerBound(),
        forallOp.getMixedUpperBound(), forallOp.getMixedStep(), newOuts,
        forallOp.getMapping(),
        /*bodyBuilderFn =*/[](OpBuilder &, Location, ValueRange) {});

    // Step 4. Merge the block of the old scf.forall into the newly created
    //         scf.forall using the new set of arguments.
    Block *loopBody = forallOp.getBody();
    Block *newLoopBody = newForallOp.getBody();
    ArrayRef<BlockArgument> newBbArgs = newLoopBody->getArguments();
    // Form initial new bbArg list with just the control operands of the new
    // scf.forall op.
    SmallVector<Value> newBlockArgs =
        llvm::map_to_vector(newBbArgs.take_front(forallOp.getRank()),
                            [](BlockArgument b) -> Value { return b; });
    Block::BlockArgListType newSharedOutsArgs = newForallOp.getRegionOutArgs();
    unsigned index = 0;
    // Take the new corresponding bbArg if the old bbArg was used as a
    // destination in the in_parallel op. For all other bbArgs, use the
    // corresponding init_arg from the old scf.forall op.
    for (OpResult result : forallOp.getResults()) {
      if (resultToDelete.count(result)) {
        newBlockArgs.push_back(forallOp.getTiedOpOperand(result)->get());
      } else {
        newBlockArgs.push_back(newSharedOutsArgs[index++]);
      }
    }
    rewriter.mergeBlocks(loopBody, newLoopBody, newBlockArgs);

    // Step 5. Replace the uses of result of old scf.forall with that of the new
    //         scf.forall.
    for (auto &&[oldResult, newResult] :
         llvm::zip(resultToReplace, newForallOp->getResults()))
      rewriter.replaceAllUsesWith(oldResult, newResult);

    // Step 6. Replace the uses of those values that either has no use or are
    //         not being modified within the loop with the corresponding
    //         OpOperand.
    for (OpResult oldResult : resultToDelete)
      rewriter.replaceAllUsesWith(oldResult,
                                  forallOp.getTiedOpOperand(oldResult)->get());
    return success();
  }
};

struct ForallOpSingleOrZeroIterationDimsFolder
    : public OpRewritePattern<ForallOp> {
  using OpRewritePattern<ForallOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ForallOp op,
                                PatternRewriter &rewriter) const override {
    // Do not fold dimensions if they are mapped to processing units.
    if (op.getMapping().has_value() && !op.getMapping()->empty())
      return failure();
    Location loc = op.getLoc();

    // Compute new loop bounds that omit all single-iteration loop dimensions.
    SmallVector<OpFoldResult> newMixedLowerBounds, newMixedUpperBounds,
        newMixedSteps;
    IRMapping mapping;
    for (auto [lb, ub, step, iv] :
         llvm::zip(op.getMixedLowerBound(), op.getMixedUpperBound(),
                   op.getMixedStep(), op.getInductionVars())) {
      auto numIterations = constantTripCount(lb, ub, step);
      if (numIterations.has_value()) {
        // Remove the loop if it performs zero iterations.
        if (*numIterations == 0) {
          rewriter.replaceOp(op, op.getOutputs());
          return success();
        }
        // Replace the loop induction variable by the lower bound if the loop
        // performs a single iteration. Otherwise, copy the loop bounds.
        if (*numIterations == 1) {
          mapping.map(iv, getValueOrCreateConstantIndexOp(rewriter, loc, lb));
          continue;
        }
      }
      newMixedLowerBounds.push_back(lb);
      newMixedUpperBounds.push_back(ub);
      newMixedSteps.push_back(step);
    }

    // All of the loop dimensions perform a single iteration. Inline loop body.
    if (newMixedLowerBounds.empty()) {
      promote(rewriter, op);
      return success();
    }

    // Exit if none of the loop dimensions perform a single iteration.
    if (newMixedLowerBounds.size() == static_cast<unsigned>(op.getRank())) {
      return rewriter.notifyMatchFailure(
          op, "no dimensions have 0 or 1 iterations");
    }

    // Replace the loop by a lower-dimensional loop.
    ForallOp newOp;
    newOp = rewriter.create<ForallOp>(loc, newMixedLowerBounds,
                                      newMixedUpperBounds, newMixedSteps,
                                      op.getOutputs(), std::nullopt, nullptr);
    newOp.getBodyRegion().getBlocks().clear();
    // The new loop needs to keep all attributes from the old one, except for
    // "operandSegmentSizes" and static loop bound attributes which capture
    // the outdated information of the old iteration domain.
    SmallVector<StringAttr> elidedAttrs{newOp.getOperandSegmentSizesAttrName(),
                                        newOp.getStaticLowerBoundAttrName(),
                                        newOp.getStaticUpperBoundAttrName(),
                                        newOp.getStaticStepAttrName()};
    for (const auto &namedAttr : op->getAttrs()) {
      if (llvm::is_contained(elidedAttrs, namedAttr.getName()))
        continue;
      rewriter.modifyOpInPlace(newOp, [&]() {
        newOp->setAttr(namedAttr.getName(), namedAttr.getValue());
      });
    }
    rewriter.cloneRegionBefore(op.getRegion(), newOp.getRegion(),
                               newOp.getRegion().begin(), mapping);
    rewriter.replaceOp(op, newOp.getResults());
    return success();
  }
};

/// Replace all induction vars with a single trip count with their lower bound.
struct ForallOpReplaceConstantInductionVar : public OpRewritePattern<ForallOp> {
  using OpRewritePattern<ForallOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ForallOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    bool changed = false;
    for (auto [lb, ub, step, iv] :
         llvm::zip(op.getMixedLowerBound(), op.getMixedUpperBound(),
                   op.getMixedStep(), op.getInductionVars())) {
      if (iv.hasNUses(0))
        continue;
      auto numIterations = constantTripCount(lb, ub, step);
      if (!numIterations.has_value() || numIterations.value() != 1) {
        continue;
      }
      rewriter.replaceAllUsesWith(
          iv, getValueOrCreateConstantIndexOp(rewriter, loc, lb));
      changed = true;
    }
    return success(changed);
  }
};

struct FoldTensorCastOfOutputIntoForallOp
    : public OpRewritePattern<scf::ForallOp> {
  using OpRewritePattern<scf::ForallOp>::OpRewritePattern;

  struct TypeCast {
    Type srcType;
    Type dstType;
  };

  LogicalResult matchAndRewrite(scf::ForallOp forallOp,
                                PatternRewriter &rewriter) const final {
    llvm::SmallMapVector<unsigned, TypeCast, 2> tensorCastProducers;
    llvm::SmallVector<Value> newOutputTensors = forallOp.getOutputs();
    for (auto en : llvm::enumerate(newOutputTensors)) {
      auto castOp = en.value().getDefiningOp<tensor::CastOp>();
      if (!castOp)
        continue;

      // Only casts that that preserve static information, i.e. will make the
      // loop result type "more" static than before, will be folded.
      if (!tensor::preservesStaticInformation(castOp.getDest().getType(),
                                              castOp.getSource().getType())) {
        continue;
      }

      tensorCastProducers[en.index()] =
          TypeCast{castOp.getSource().getType(), castOp.getType()};
      newOutputTensors[en.index()] = castOp.getSource();
    }

    if (tensorCastProducers.empty())
      return failure();

    // Create new loop.
    Location loc = forallOp.getLoc();
    auto newForallOp = rewriter.create<ForallOp>(
        loc, forallOp.getMixedLowerBound(), forallOp.getMixedUpperBound(),
        forallOp.getMixedStep(), newOutputTensors, forallOp.getMapping(),
        [&](OpBuilder nestedBuilder, Location nestedLoc, ValueRange bbArgs) {
          auto castBlockArgs =
              llvm::to_vector(bbArgs.take_back(forallOp->getNumResults()));
          for (auto [index, cast] : tensorCastProducers) {
            Value &oldTypeBBArg = castBlockArgs[index];
            oldTypeBBArg = nestedBuilder.create<tensor::CastOp>(
                nestedLoc, cast.dstType, oldTypeBBArg);
          }

          // Move old body into new parallel loop.
          SmallVector<Value> ivsBlockArgs =
              llvm::to_vector(bbArgs.take_front(forallOp.getRank()));
          ivsBlockArgs.append(castBlockArgs);
          rewriter.mergeBlocks(forallOp.getBody(),
                               bbArgs.front().getParentBlock(), ivsBlockArgs);
        });

    // After `mergeBlocks` happened, the destinations in the terminator were
    // mapped to the tensor.cast old-typed results of the output bbArgs. The
    // destination have to be updated to point to the output bbArgs directly.
    auto terminator = newForallOp.getTerminator();
    for (auto [yieldingOp, outputBlockArg] : llvm::zip(
             terminator.getYieldingOps(), newForallOp.getRegionIterArgs())) {
      auto insertSliceOp = cast<tensor::ParallelInsertSliceOp>(yieldingOp);
      insertSliceOp.getDestMutable().assign(outputBlockArg);
    }

    // Cast results back to the original types.
    rewriter.setInsertionPointAfter(newForallOp);
    SmallVector<Value> castResults = newForallOp.getResults();
    for (auto &item : tensorCastProducers) {
      Value &oldTypeResult = castResults[item.first];
      oldTypeResult = rewriter.create<tensor::CastOp>(loc, item.second.dstType,
                                                      oldTypeResult);
    }
    rewriter.replaceOp(forallOp, castResults);
    return success();
  }
};

} // namespace

void ForallOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                           MLIRContext *context) {
  results.add<DimOfForallOp, FoldTensorCastOfOutputIntoForallOp,
              ForallOpControlOperandsFolder, ForallOpIterArgsFolder,
              ForallOpSingleOrZeroIterationDimsFolder,
              ForallOpReplaceConstantInductionVar>(context);
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes that
/// correspond to a constant value for each operand, or null if that operand is
/// not a constant.
void ForallOp::getSuccessorRegions(RegionBranchPoint point,
                                   SmallVectorImpl<RegionSuccessor> &regions) {
  // In accordance with the semantics of forall, its body is executed in
  // parallel by multiple threads. We should not expect to branch back into
  // the forall body after the region's execution is complete.
  if (point.isParent())
    regions.push_back(RegionSuccessor(&getRegion()));
  else
    regions.push_back(RegionSuccessor());
}

//===----------------------------------------------------------------------===//
// InParallelOp
//===----------------------------------------------------------------------===//

// Build a InParallelOp with mixed static and dynamic entries.
void InParallelOp::build(OpBuilder &b, OperationState &result) {
  OpBuilder::InsertionGuard g(b);
  Region *bodyRegion = result.addRegion();
  b.createBlock(bodyRegion);
}

LogicalResult InParallelOp::verify() {
  scf::ForallOp forallOp =
      dyn_cast<scf::ForallOp>(getOperation()->getParentOp());
  if (!forallOp)
    return this->emitOpError("expected forall op parent");

  // TODO: InParallelOpInterface.
  for (Operation &op : getRegion().front().getOperations()) {
    if (!isa<tensor::ParallelInsertSliceOp>(op)) {
      return this->emitOpError("expected only ")
             << tensor::ParallelInsertSliceOp::getOperationName() << " ops";
    }

    // Verify that inserts are into out block arguments.
    Value dest = cast<tensor::ParallelInsertSliceOp>(op).getDest();
    ArrayRef<BlockArgument> regionOutArgs = forallOp.getRegionOutArgs();
    if (!llvm::is_contained(regionOutArgs, dest))
      return op.emitOpError("may only insert into an output block argument");
  }
  return success();
}

void InParallelOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
  p.printOptionalAttrDict(getOperation()->getAttrs());
}

ParseResult InParallelOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();

  SmallVector<OpAsmParser::Argument, 8> regionOperands;
  std::unique_ptr<Region> region = std::make_unique<Region>();
  if (parser.parseRegion(*region, regionOperands))
    return failure();

  if (region->empty())
    OpBuilder(builder.getContext()).createBlock(region.get());
  result.addRegion(std::move(region));

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

OpResult InParallelOp::getParentResult(int64_t idx) {
  return getOperation()->getParentOp()->getResult(idx);
}

SmallVector<BlockArgument> InParallelOp::getDests() {
  return llvm::to_vector<4>(
      llvm::map_range(getYieldingOps(), [](Operation &op) {
        // Add new ops here as needed.
        auto insertSliceOp = cast<tensor::ParallelInsertSliceOp>(&op);
        return llvm::cast<BlockArgument>(insertSliceOp.getDest());
      }));
}

llvm::iterator_range<Block::iterator> InParallelOp::getYieldingOps() {
  return getRegion().front().getOperations();
}

//===----------------------------------------------------------------------===//
// IfOp
//===----------------------------------------------------------------------===//

bool mlir::scf::insideMutuallyExclusiveBranches(Operation *a, Operation *b) {
  assert(a && "expected non-empty operation");
  assert(b && "expected non-empty operation");

  IfOp ifOp = a->getParentOfType<IfOp>();
  while (ifOp) {
    // Check if b is inside ifOp. (We already know that a is.)
    if (ifOp->isProperAncestor(b))
      // b is contained in ifOp. a and b are in mutually exclusive branches if
      // they are in different blocks of ifOp.
      return static_cast<bool>(ifOp.thenBlock()->findAncestorOpInBlock(*a)) !=
             static_cast<bool>(ifOp.thenBlock()->findAncestorOpInBlock(*b));
    // Check next enclosing IfOp.
    ifOp = ifOp->getParentOfType<IfOp>();
  }

  // Could not find a common IfOp among a's and b's ancestors.
  return false;
}

LogicalResult
IfOp::inferReturnTypes(MLIRContext *ctx, std::optional<Location> loc,
                       IfOp::Adaptor adaptor,
                       SmallVectorImpl<Type> &inferredReturnTypes) {
  if (adaptor.getRegions().empty())
    return failure();
  Region *r = &adaptor.getThenRegion();
  if (r->empty())
    return failure();
  Block &b = r->front();
  if (b.empty())
    return failure();
  auto yieldOp = llvm::dyn_cast<YieldOp>(b.back());
  if (!yieldOp)
    return failure();
  TypeRange types = yieldOp.getOperandTypes();
  llvm::append_range(inferredReturnTypes, types);
  return success();
}

void IfOp::build(OpBuilder &builder, OperationState &result,
                 TypeRange resultTypes, Value cond) {
  return build(builder, result, resultTypes, cond, /*addThenBlock=*/false,
               /*addElseBlock=*/false);
}

void IfOp::build(OpBuilder &builder, OperationState &result,
                 TypeRange resultTypes, Value cond, bool addThenBlock,
                 bool addElseBlock) {
  assert((!addElseBlock || addThenBlock) &&
         "must not create else block w/o then block");
  result.addTypes(resultTypes);
  result.addOperands(cond);

  // Add regions and blocks.
  OpBuilder::InsertionGuard guard(builder);
  Region *thenRegion = result.addRegion();
  if (addThenBlock)
    builder.createBlock(thenRegion);
  Region *elseRegion = result.addRegion();
  if (addElseBlock)
    builder.createBlock(elseRegion);
}

void IfOp::build(OpBuilder &builder, OperationState &result, Value cond,
                 bool withElseRegion) {
  build(builder, result, TypeRange{}, cond, withElseRegion);
}

void IfOp::build(OpBuilder &builder, OperationState &result,
                 TypeRange resultTypes, Value cond, bool withElseRegion) {
  result.addTypes(resultTypes);
  result.addOperands(cond);

  // Build then region.
  OpBuilder::InsertionGuard guard(builder);
  Region *thenRegion = result.addRegion();
  builder.createBlock(thenRegion);
  if (resultTypes.empty())
    IfOp::ensureTerminator(*thenRegion, builder, result.location);

  // Build else region.
  Region *elseRegion = result.addRegion();
  if (withElseRegion) {
    builder.createBlock(elseRegion);
    if (resultTypes.empty())
      IfOp::ensureTerminator(*elseRegion, builder, result.location);
  }
}

void IfOp::build(OpBuilder &builder, OperationState &result, Value cond,
                 function_ref<void(OpBuilder &, Location)> thenBuilder,
                 function_ref<void(OpBuilder &, Location)> elseBuilder) {
  assert(thenBuilder && "the builder callback for 'then' must be present");
  result.addOperands(cond);

  // Build then region.
  OpBuilder::InsertionGuard guard(builder);
  Region *thenRegion = result.addRegion();
  builder.createBlock(thenRegion);
  thenBuilder(builder, result.location);

  // Build else region.
  Region *elseRegion = result.addRegion();
  if (elseBuilder) {
    builder.createBlock(elseRegion);
    elseBuilder(builder, result.location);
  }

  // Infer result types.
  SmallVector<Type> inferredReturnTypes;
  MLIRContext *ctx = builder.getContext();
  auto attrDict = DictionaryAttr::get(ctx, result.attributes);
  if (succeeded(inferReturnTypes(ctx, std::nullopt, result.operands, attrDict,
                                 /*properties=*/nullptr, result.regions,
                                 inferredReturnTypes))) {
    result.addTypes(inferredReturnTypes);
  }
}

LogicalResult IfOp::verify() {
  if (getNumResults() != 0 && getElseRegion().empty())
    return emitOpError("must have an else block if defining values");
  return success();
}

ParseResult IfOp::parse(OpAsmParser &parser, OperationState &result) {
  // Create the regions for 'then'.
  result.regions.reserve(2);
  Region *thenRegion = result.addRegion();
  Region *elseRegion = result.addRegion();

  auto &builder = parser.getBuilder();
  OpAsmParser::UnresolvedOperand cond;
  Type i1Type = builder.getIntegerType(1);
  if (parser.parseOperand(cond) ||
      parser.resolveOperand(cond, i1Type, result.operands))
    return failure();
  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();
  // Parse the 'then' region.
  if (parser.parseRegion(*thenRegion, /*arguments=*/{}, /*argTypes=*/{}))
    return failure();
  IfOp::ensureTerminator(*thenRegion, parser.getBuilder(), result.location);

  // If we find an 'else' keyword then parse the 'else' region.
  if (!parser.parseOptionalKeyword("else")) {
    if (parser.parseRegion(*elseRegion, /*arguments=*/{}, /*argTypes=*/{}))
      return failure();
    IfOp::ensureTerminator(*elseRegion, parser.getBuilder(), result.location);
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void IfOp::print(OpAsmPrinter &p) {
  bool printBlockTerminators = false;

  p << " " << getCondition();
  if (!getResults().empty()) {
    p << " -> (" << getResultTypes() << ")";
    // Print yield explicitly if the op defines values.
    printBlockTerminators = true;
  }
  p << ' ';
  p.printRegion(getThenRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/printBlockTerminators);

  // Print the 'else' regions if it exists and has a block.
  auto &elseRegion = getElseRegion();
  if (!elseRegion.empty()) {
    p << " else ";
    p.printRegion(elseRegion,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/printBlockTerminators);
  }

  p.printOptionalAttrDict((*this)->getAttrs());
}

void IfOp::getSuccessorRegions(RegionBranchPoint point,
                               SmallVectorImpl<RegionSuccessor> &regions) {
  // The `then` and the `else` region branch back to the parent operation.
  if (!point.isParent()) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  regions.push_back(RegionSuccessor(&getThenRegion()));

  // Don't consider the else region if it is empty.
  Region *elseRegion = &this->getElseRegion();
  if (elseRegion->empty())
    regions.push_back(RegionSuccessor());
  else
    regions.push_back(RegionSuccessor(elseRegion));
}

void IfOp::getEntrySuccessorRegions(ArrayRef<Attribute> operands,
                                    SmallVectorImpl<RegionSuccessor> &regions) {
  FoldAdaptor adaptor(operands, *this);
  auto boolAttr = dyn_cast_or_null<BoolAttr>(adaptor.getCondition());
  if (!boolAttr || boolAttr.getValue())
    regions.emplace_back(&getThenRegion());

  // If the else region is empty, execution continues after the parent op.
  if (!boolAttr || !boolAttr.getValue()) {
    if (!getElseRegion().empty())
      regions.emplace_back(&getElseRegion());
    else
      regions.emplace_back(getResults());
  }
}

LogicalResult IfOp::fold(FoldAdaptor adaptor,
                         SmallVectorImpl<OpFoldResult> &results) {
  // if (!c) then A() else B() -> if c then B() else A()
  if (getElseRegion().empty())
    return failure();

  arith::XOrIOp xorStmt = getCondition().getDefiningOp<arith::XOrIOp>();
  if (!xorStmt)
    return failure();

  if (!matchPattern(xorStmt.getRhs(), m_One()))
    return failure();

  getConditionMutable().assign(xorStmt.getLhs());
  Block *thenBlock = &getThenRegion().front();
  // It would be nicer to use iplist::swap, but that has no implemented
  // callbacks See: https://llvm.org/doxygen/ilist_8h_source.html#l00224
  getThenRegion().getBlocks().splice(getThenRegion().getBlocks().begin(),
                                     getElseRegion().getBlocks());
  getElseRegion().getBlocks().splice(getElseRegion().getBlocks().begin(),
                                     getThenRegion().getBlocks(), thenBlock);
  return success();
}

void IfOp::getRegionInvocationBounds(
    ArrayRef<Attribute> operands,
    SmallVectorImpl<InvocationBounds> &invocationBounds) {
  if (auto cond = llvm::dyn_cast_or_null<BoolAttr>(operands[0])) {
    // If the condition is known, then one region is known to be executed once
    // and the other zero times.
    invocationBounds.emplace_back(0, cond.getValue() ? 1 : 0);
    invocationBounds.emplace_back(0, cond.getValue() ? 0 : 1);
  } else {
    // Non-constant condition. Each region may be executed 0 or 1 times.
    invocationBounds.assign(2, {0, 1});
  }
}

namespace {
// Pattern to remove unused IfOp results.
struct RemoveUnusedResults : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  void transferBody(Block *source, Block *dest, ArrayRef<OpResult> usedResults,
                    PatternRewriter &rewriter) const {
    // Move all operations to the destination block.
    rewriter.mergeBlocks(source, dest);
    // Replace the yield op by one that returns only the used values.
    auto yieldOp = cast<scf::YieldOp>(dest->getTerminator());
    SmallVector<Value, 4> usedOperands;
    llvm::transform(usedResults, std::back_inserter(usedOperands),
                    [&](OpResult result) {
                      return yieldOp.getOperand(result.getResultNumber());
                    });
    rewriter.modifyOpInPlace(yieldOp,
                             [&]() { yieldOp->setOperands(usedOperands); });
  }

  LogicalResult matchAndRewrite(IfOp op,
                                PatternRewriter &rewriter) const override {
    // Compute the list of used results.
    SmallVector<OpResult, 4> usedResults;
    llvm::copy_if(op.getResults(), std::back_inserter(usedResults),
                  [](OpResult result) { return !result.use_empty(); });

    // Replace the operation if only a subset of its results have uses.
    if (usedResults.size() == op.getNumResults())
      return failure();

    // Compute the result types of the replacement operation.
    SmallVector<Type, 4> newTypes;
    llvm::transform(usedResults, std::back_inserter(newTypes),
                    [](OpResult result) { return result.getType(); });

    // Create a replacement operation with empty then and else regions.
    auto newOp =
        rewriter.create<IfOp>(op.getLoc(), newTypes, op.getCondition());
    rewriter.createBlock(&newOp.getThenRegion());
    rewriter.createBlock(&newOp.getElseRegion());

    // Move the bodies and replace the terminators (note there is a then and
    // an else region since the operation returns results).
    transferBody(op.getBody(0), newOp.getBody(0), usedResults, rewriter);
    transferBody(op.getBody(1), newOp.getBody(1), usedResults, rewriter);

    // Replace the operation by the new one.
    SmallVector<Value, 4> repResults(op.getNumResults());
    for (const auto &en : llvm::enumerate(usedResults))
      repResults[en.value().getResultNumber()] = newOp.getResult(en.index());
    rewriter.replaceOp(op, repResults);
    return success();
  }
};

struct RemoveStaticCondition : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IfOp op,
                                PatternRewriter &rewriter) const override {
    BoolAttr condition;
    if (!matchPattern(op.getCondition(), m_Constant(&condition)))
      return failure();

    if (condition.getValue())
      replaceOpWithRegion(rewriter, op, op.getThenRegion());
    else if (!op.getElseRegion().empty())
      replaceOpWithRegion(rewriter, op, op.getElseRegion());
    else
      rewriter.eraseOp(op);

    return success();
  }
};

/// Hoist any yielded results whose operands are defined outside
/// the if, to a select instruction.
struct ConvertTrivialIfToSelect : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IfOp op,
                                PatternRewriter &rewriter) const override {
    if (op->getNumResults() == 0)
      return failure();

    auto cond = op.getCondition();
    auto thenYieldArgs = op.thenYield().getOperands();
    auto elseYieldArgs = op.elseYield().getOperands();

    SmallVector<Type> nonHoistable;
    for (auto [trueVal, falseVal] : llvm::zip(thenYieldArgs, elseYieldArgs)) {
      if (&op.getThenRegion() == trueVal.getParentRegion() ||
          &op.getElseRegion() == falseVal.getParentRegion())
        nonHoistable.push_back(trueVal.getType());
    }
    // Early exit if there aren't any yielded values we can
    // hoist outside the if.
    if (nonHoistable.size() == op->getNumResults())
      return failure();

    IfOp replacement = rewriter.create<IfOp>(op.getLoc(), nonHoistable, cond,
                                             /*withElseRegion=*/false);
    if (replacement.thenBlock())
      rewriter.eraseBlock(replacement.thenBlock());
    replacement.getThenRegion().takeBody(op.getThenRegion());
    replacement.getElseRegion().takeBody(op.getElseRegion());

    SmallVector<Value> results(op->getNumResults());
    assert(thenYieldArgs.size() == results.size());
    assert(elseYieldArgs.size() == results.size());

    SmallVector<Value> trueYields;
    SmallVector<Value> falseYields;
    rewriter.setInsertionPoint(replacement);
    for (const auto &it :
         llvm::enumerate(llvm::zip(thenYieldArgs, elseYieldArgs))) {
      Value trueVal = std::get<0>(it.value());
      Value falseVal = std::get<1>(it.value());
      if (&replacement.getThenRegion() == trueVal.getParentRegion() ||
          &replacement.getElseRegion() == falseVal.getParentRegion()) {
        results[it.index()] = replacement.getResult(trueYields.size());
        trueYields.push_back(trueVal);
        falseYields.push_back(falseVal);
      } else if (trueVal == falseVal)
        results[it.index()] = trueVal;
      else
        results[it.index()] = rewriter.create<arith::SelectOp>(
            op.getLoc(), cond, trueVal, falseVal);
    }

    rewriter.setInsertionPointToEnd(replacement.thenBlock());
    rewriter.replaceOpWithNewOp<YieldOp>(replacement.thenYield(), trueYields);

    rewriter.setInsertionPointToEnd(replacement.elseBlock());
    rewriter.replaceOpWithNewOp<YieldOp>(replacement.elseYield(), falseYields);

    rewriter.replaceOp(op, results);
    return success();
  }
};

/// Remove any statements from an if that are equivalent to the condition
/// or its negation. For example:
///
///    %res:2 = scf.if %cmp {
///       yield something(), true
///    } else {
///       yield something2(), false
///    }
///    print(%res#1)
///
///  becomes
///    %res = scf.if %cmp {
///       yield something()
///    } else {
///       yield something2()
///    }
///    print(%cmp)
///
/// Additionally if both branches yield the same value, replace all uses
/// of the result with the yielded value.
///
///    %res:2 = scf.if %cmp {
///       yield something(), %arg1
///    } else {
///       yield something2(), %arg1
///    }
///    print(%res#1)
///
///  becomes
///    %res = scf.if %cmp {
///       yield something()
///    } else {
///       yield something2()
///    }
///    print(%arg1)
///
struct ReplaceIfYieldWithConditionOrValue : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IfOp op,
                                PatternRewriter &rewriter) const override {
    // Early exit if there are no results that could be replaced.
    if (op.getNumResults() == 0)
      return failure();

    auto trueYield =
        cast<scf::YieldOp>(op.getThenRegion().back().getTerminator());
    auto falseYield =
        cast<scf::YieldOp>(op.getElseRegion().back().getTerminator());

    rewriter.setInsertionPoint(op->getBlock(),
                               op.getOperation()->getIterator());
    bool changed = false;
    Type i1Ty = rewriter.getI1Type();
    for (auto [trueResult, falseResult, opResult] :
         llvm::zip(trueYield.getResults(), falseYield.getResults(),
                   op.getResults())) {
      if (trueResult == falseResult) {
        if (!opResult.use_empty()) {
          opResult.replaceAllUsesWith(trueResult);
          changed = true;
        }
        continue;
      }

      BoolAttr trueYield, falseYield;
      if (!matchPattern(trueResult, m_Constant(&trueYield)) ||
          !matchPattern(falseResult, m_Constant(&falseYield)))
        continue;

      bool trueVal = trueYield.getValue();
      bool falseVal = falseYield.getValue();
      if (!trueVal && falseVal) {
        if (!opResult.use_empty()) {
          Dialect *constDialect = trueResult.getDefiningOp()->getDialect();
          Value notCond = rewriter.create<arith::XOrIOp>(
              op.getLoc(), op.getCondition(),
              constDialect
                  ->materializeConstant(rewriter,
                                        rewriter.getIntegerAttr(i1Ty, 1), i1Ty,
                                        op.getLoc())
                  ->getResult(0));
          opResult.replaceAllUsesWith(notCond);
          changed = true;
        }
      }
      if (trueVal && !falseVal) {
        if (!opResult.use_empty()) {
          opResult.replaceAllUsesWith(op.getCondition());
          changed = true;
        }
      }
    }
    return success(changed);
  }
};

/// Merge any consecutive scf.if's with the same condition.
///
///    scf.if %cond {
///       firstCodeTrue();...
///    } else {
///       firstCodeFalse();...
///    }
///    %res = scf.if %cond {
///       secondCodeTrue();...
///    } else {
///       secondCodeFalse();...
///    }
///
///  becomes
///    %res = scf.if %cmp {
///       firstCodeTrue();...
///       secondCodeTrue();...
///    } else {
///       firstCodeFalse();...
///       secondCodeFalse();...
///    }
struct CombineIfs : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IfOp nextIf,
                                PatternRewriter &rewriter) const override {
    Block *parent = nextIf->getBlock();
    if (nextIf == &parent->front())
      return failure();

    auto prevIf = dyn_cast<IfOp>(nextIf->getPrevNode());
    if (!prevIf)
      return failure();

    // Determine the logical then/else blocks when prevIf's
    // condition is used. Null means the block does not exist
    // in that case (e.g. empty else). If neither of these
    // are set, the two conditions cannot be compared.
    Block *nextThen = nullptr;
    Block *nextElse = nullptr;
    if (nextIf.getCondition() == prevIf.getCondition()) {
      nextThen = nextIf.thenBlock();
      if (!nextIf.getElseRegion().empty())
        nextElse = nextIf.elseBlock();
    }
    if (arith::XOrIOp notv =
            nextIf.getCondition().getDefiningOp<arith::XOrIOp>()) {
      if (notv.getLhs() == prevIf.getCondition() &&
          matchPattern(notv.getRhs(), m_One())) {
        nextElse = nextIf.thenBlock();
        if (!nextIf.getElseRegion().empty())
          nextThen = nextIf.elseBlock();
      }
    }
    if (arith::XOrIOp notv =
            prevIf.getCondition().getDefiningOp<arith::XOrIOp>()) {
      if (notv.getLhs() == nextIf.getCondition() &&
          matchPattern(notv.getRhs(), m_One())) {
        nextElse = nextIf.thenBlock();
        if (!nextIf.getElseRegion().empty())
          nextThen = nextIf.elseBlock();
      }
    }

    if (!nextThen && !nextElse)
      return failure();

    SmallVector<Value> prevElseYielded;
    if (!prevIf.getElseRegion().empty())
      prevElseYielded = prevIf.elseYield().getOperands();
    // Replace all uses of return values of op within nextIf with the
    // corresponding yields
    for (auto it : llvm::zip(prevIf.getResults(),
                             prevIf.thenYield().getOperands(), prevElseYielded))
      for (OpOperand &use :
           llvm::make_early_inc_range(std::get<0>(it).getUses())) {
        if (nextThen && nextThen->getParent()->isAncestor(
                            use.getOwner()->getParentRegion())) {
          rewriter.startOpModification(use.getOwner());
          use.set(std::get<1>(it));
          rewriter.finalizeOpModification(use.getOwner());
        } else if (nextElse && nextElse->getParent()->isAncestor(
                                   use.getOwner()->getParentRegion())) {
          rewriter.startOpModification(use.getOwner());
          use.set(std::get<2>(it));
          rewriter.finalizeOpModification(use.getOwner());
        }
      }

    SmallVector<Type> mergedTypes(prevIf.getResultTypes());
    llvm::append_range(mergedTypes, nextIf.getResultTypes());

    IfOp combinedIf = rewriter.create<IfOp>(
        nextIf.getLoc(), mergedTypes, prevIf.getCondition(), /*hasElse=*/false);
    rewriter.eraseBlock(&combinedIf.getThenRegion().back());

    rewriter.inlineRegionBefore(prevIf.getThenRegion(),
                                combinedIf.getThenRegion(),
                                combinedIf.getThenRegion().begin());

    if (nextThen) {
      YieldOp thenYield = combinedIf.thenYield();
      YieldOp thenYield2 = cast<YieldOp>(nextThen->getTerminator());
      rewriter.mergeBlocks(nextThen, combinedIf.thenBlock());
      rewriter.setInsertionPointToEnd(combinedIf.thenBlock());

      SmallVector<Value> mergedYields(thenYield.getOperands());
      llvm::append_range(mergedYields, thenYield2.getOperands());
      rewriter.create<YieldOp>(thenYield2.getLoc(), mergedYields);
      rewriter.eraseOp(thenYield);
      rewriter.eraseOp(thenYield2);
    }

    rewriter.inlineRegionBefore(prevIf.getElseRegion(),
                                combinedIf.getElseRegion(),
                                combinedIf.getElseRegion().begin());

    if (nextElse) {
      if (combinedIf.getElseRegion().empty()) {
        rewriter.inlineRegionBefore(*nextElse->getParent(),
                                    combinedIf.getElseRegion(),
                                    combinedIf.getElseRegion().begin());
      } else {
        YieldOp elseYield = combinedIf.elseYield();
        YieldOp elseYield2 = cast<YieldOp>(nextElse->getTerminator());
        rewriter.mergeBlocks(nextElse, combinedIf.elseBlock());

        rewriter.setInsertionPointToEnd(combinedIf.elseBlock());

        SmallVector<Value> mergedElseYields(elseYield.getOperands());
        llvm::append_range(mergedElseYields, elseYield2.getOperands());

        rewriter.create<YieldOp>(elseYield2.getLoc(), mergedElseYields);
        rewriter.eraseOp(elseYield);
        rewriter.eraseOp(elseYield2);
      }
    }

    SmallVector<Value> prevValues;
    SmallVector<Value> nextValues;
    for (const auto &pair : llvm::enumerate(combinedIf.getResults())) {
      if (pair.index() < prevIf.getNumResults())
        prevValues.push_back(pair.value());
      else
        nextValues.push_back(pair.value());
    }
    rewriter.replaceOp(prevIf, prevValues);
    rewriter.replaceOp(nextIf, nextValues);
    return success();
  }
};

/// Pattern to remove an empty else branch.
struct RemoveEmptyElseBranch : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IfOp ifOp,
                                PatternRewriter &rewriter) const override {
    // Cannot remove else region when there are operation results.
    if (ifOp.getNumResults())
      return failure();
    Block *elseBlock = ifOp.elseBlock();
    if (!elseBlock || !llvm::hasSingleElement(*elseBlock))
      return failure();
    auto newIfOp = rewriter.cloneWithoutRegions(ifOp);
    rewriter.inlineRegionBefore(ifOp.getThenRegion(), newIfOp.getThenRegion(),
                                newIfOp.getThenRegion().begin());
    rewriter.eraseOp(ifOp);
    return success();
  }
};

/// Convert nested `if`s into `arith.andi` + single `if`.
///
///    scf.if %arg0 {
///      scf.if %arg1 {
///        ...
///        scf.yield
///      }
///      scf.yield
///    }
///  becomes
///
///    %0 = arith.andi %arg0, %arg1
///    scf.if %0 {
///      ...
///      scf.yield
///    }
struct CombineNestedIfs : public OpRewritePattern<IfOp> {
  using OpRewritePattern<IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IfOp op,
                                PatternRewriter &rewriter) const override {
    auto nestedOps = op.thenBlock()->without_terminator();
    // Nested `if` must be the only op in block.
    if (!llvm::hasSingleElement(nestedOps))
      return failure();

    // If there is an else block, it can only yield
    if (op.elseBlock() && !llvm::hasSingleElement(*op.elseBlock()))
      return failure();

    auto nestedIf = dyn_cast<IfOp>(*nestedOps.begin());
    if (!nestedIf)
      return failure();

    if (nestedIf.elseBlock() && !llvm::hasSingleElement(*nestedIf.elseBlock()))
      return failure();

    SmallVector<Value> thenYield(op.thenYield().getOperands());
    SmallVector<Value> elseYield;
    if (op.elseBlock())
      llvm::append_range(elseYield, op.elseYield().getOperands());

    // A list of indices for which we should upgrade the value yielded
    // in the else to a select.
    SmallVector<unsigned> elseYieldsToUpgradeToSelect;

    // If the outer scf.if yields a value produced by the inner scf.if,
    // only permit combining if the value yielded when the condition
    // is false in the outer scf.if is the same value yielded when the
    // inner scf.if condition is false.
    // Note that the array access to elseYield will not go out of bounds
    // since it must have the same length as thenYield, since they both
    // come from the same scf.if.
    for (const auto &tup : llvm::enumerate(thenYield)) {
      if (tup.value().getDefiningOp() == nestedIf) {
        auto nestedIdx = llvm::cast<OpResult>(tup.value()).getResultNumber();
        if (nestedIf.elseYield().getOperand(nestedIdx) !=
            elseYield[tup.index()]) {
          return failure();
        }
        // If the correctness test passes, we will yield
        // corresponding value from the inner scf.if
        thenYield[tup.index()] = nestedIf.thenYield().getOperand(nestedIdx);
        continue;
      }

      // Otherwise, we need to ensure the else block of the combined
      // condition still returns the same value when the outer condition is
      // true and the inner condition is false. This can be accomplished if
      // the then value is defined outside the outer scf.if and we replace the
      // value with a select that considers just the outer condition. Since
      // the else region contains just the yield, its yielded value is
      // defined outside the scf.if, by definition.

      // If the then value is defined within the scf.if, bail.
      if (tup.value().getParentRegion() == &op.getThenRegion()) {
        return failure();
      }
      elseYieldsToUpgradeToSelect.push_back(tup.index());
    }

    Location loc = op.getLoc();
    Value newCondition = rewriter.create<arith::AndIOp>(
        loc, op.getCondition(), nestedIf.getCondition());
    auto newIf = rewriter.create<IfOp>(loc, op.getResultTypes(), newCondition);
    Block *newIfBlock = rewriter.createBlock(&newIf.getThenRegion());

    SmallVector<Value> results;
    llvm::append_range(results, newIf.getResults());
    rewriter.setInsertionPoint(newIf);

    for (auto idx : elseYieldsToUpgradeToSelect)
      results[idx] = rewriter.create<arith::SelectOp>(
          op.getLoc(), op.getCondition(), thenYield[idx], elseYield[idx]);

    rewriter.mergeBlocks(nestedIf.thenBlock(), newIfBlock);
    rewriter.setInsertionPointToEnd(newIf.thenBlock());
    rewriter.replaceOpWithNewOp<YieldOp>(newIf.thenYield(), thenYield);
    if (!elseYield.empty()) {
      rewriter.createBlock(&newIf.getElseRegion());
      rewriter.setInsertionPointToEnd(newIf.elseBlock());
      rewriter.create<YieldOp>(loc, elseYield);
    }
    rewriter.replaceOp(op, results);
    return success();
  }
};

} // namespace

void IfOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                       MLIRContext *context) {
  results.add<CombineIfs, CombineNestedIfs, ConvertTrivialIfToSelect,
              RemoveEmptyElseBranch, RemoveStaticCondition, RemoveUnusedResults,
              ReplaceIfYieldWithConditionOrValue>(context);
}

Block *IfOp::thenBlock() { return &getThenRegion().back(); }
YieldOp IfOp::thenYield() { return cast<YieldOp>(&thenBlock()->back()); }
Block *IfOp::elseBlock() {
  Region &r = getElseRegion();
  if (r.empty())
    return nullptr;
  return &r.back();
}
YieldOp IfOp::elseYield() { return cast<YieldOp>(&elseBlock()->back()); }

//===----------------------------------------------------------------------===//
// ParallelOp
//===----------------------------------------------------------------------===//

void ParallelOp::build(
    OpBuilder &builder, OperationState &result, ValueRange lowerBounds,
    ValueRange upperBounds, ValueRange steps, ValueRange initVals,
    function_ref<void(OpBuilder &, Location, ValueRange, ValueRange)>
        bodyBuilderFn) {
  result.addOperands(lowerBounds);
  result.addOperands(upperBounds);
  result.addOperands(steps);
  result.addOperands(initVals);
  result.addAttribute(
      ParallelOp::getOperandSegmentSizeAttr(),
      builder.getDenseI32ArrayAttr({static_cast<int32_t>(lowerBounds.size()),
                                    static_cast<int32_t>(upperBounds.size()),
                                    static_cast<int32_t>(steps.size()),
                                    static_cast<int32_t>(initVals.size())}));
  result.addTypes(initVals.getTypes());

  OpBuilder::InsertionGuard guard(builder);
  unsigned numIVs = steps.size();
  SmallVector<Type, 8> argTypes(numIVs, builder.getIndexType());
  SmallVector<Location, 8> argLocs(numIVs, result.location);
  Region *bodyRegion = result.addRegion();
  Block *bodyBlock = builder.createBlock(bodyRegion, {}, argTypes, argLocs);

  if (bodyBuilderFn) {
    builder.setInsertionPointToStart(bodyBlock);
    bodyBuilderFn(builder, result.location,
                  bodyBlock->getArguments().take_front(numIVs),
                  bodyBlock->getArguments().drop_front(numIVs));
  }
  // Add terminator only if there are no reductions.
  if (initVals.empty())
    ParallelOp::ensureTerminator(*bodyRegion, builder, result.location);
}

void ParallelOp::build(
    OpBuilder &builder, OperationState &result, ValueRange lowerBounds,
    ValueRange upperBounds, ValueRange steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  // Only pass a non-null wrapper if bodyBuilderFn is non-null itself. Make sure
  // we don't capture a reference to a temporary by constructing the lambda at
  // function level.
  auto wrappedBuilderFn = [&bodyBuilderFn](OpBuilder &nestedBuilder,
                                           Location nestedLoc, ValueRange ivs,
                                           ValueRange) {
    bodyBuilderFn(nestedBuilder, nestedLoc, ivs);
  };
  function_ref<void(OpBuilder &, Location, ValueRange, ValueRange)> wrapper;
  if (bodyBuilderFn)
    wrapper = wrappedBuilderFn;

  build(builder, result, lowerBounds, upperBounds, steps, ValueRange(),
        wrapper);
}

LogicalResult ParallelOp::verify() {
  // Check that there is at least one value in lowerBound, upperBound and step.
  // It is sufficient to test only step, because it is ensured already that the
  // number of elements in lowerBound, upperBound and step are the same.
  Operation::operand_range stepValues = getStep();
  if (stepValues.empty())
    return emitOpError(
        "needs at least one tuple element for lowerBound, upperBound and step");

  // Check whether all constant step values are positive.
  for (Value stepValue : stepValues)
    if (auto cst = getConstantIntValue(stepValue))
      if (*cst <= 0)
        return emitOpError("constant step operand must be positive");

  // Check that the body defines the same number of block arguments as the
  // number of tuple elements in step.
  Block *body = getBody();
  if (body->getNumArguments() != stepValues.size())
    return emitOpError() << "expects the same number of induction variables: "
                         << body->getNumArguments()
                         << " as bound and step values: " << stepValues.size();
  for (auto arg : body->getArguments())
    if (!arg.getType().isIndex())
      return emitOpError(
          "expects arguments for the induction variable to be of index type");

  // Check that the terminator is an scf.reduce op.
  auto reduceOp = verifyAndGetTerminator<scf::ReduceOp>(
      *this, getRegion(), "expects body to terminate with 'scf.reduce'");
  if (!reduceOp)
    return failure();

  // Check that the number of results is the same as the number of reductions.
  auto resultsSize = getResults().size();
  auto reductionsSize = reduceOp.getReductions().size();
  auto initValsSize = getInitVals().size();
  if (resultsSize != reductionsSize)
    return emitOpError() << "expects number of results: " << resultsSize
                         << " to be the same as number of reductions: "
                         << reductionsSize;
  if (resultsSize != initValsSize)
    return emitOpError() << "expects number of results: " << resultsSize
                         << " to be the same as number of initial values: "
                         << initValsSize;

  // Check that the types of the results and reductions are the same.
  for (int64_t i = 0; i < static_cast<int64_t>(reductionsSize); ++i) {
    auto resultType = getOperation()->getResult(i).getType();
    auto reductionOperandType = reduceOp.getOperands()[i].getType();
    if (resultType != reductionOperandType)
      return reduceOp.emitOpError()
             << "expects type of " << i
             << "-th reduction operand: " << reductionOperandType
             << " to be the same as the " << i
             << "-th result type: " << resultType;
  }
  return success();
}

ParseResult ParallelOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  // Parse an opening `(` followed by induction variables followed by `)`
  SmallVector<OpAsmParser::Argument, 4> ivs;
  if (parser.parseArgumentList(ivs, OpAsmParser::Delimiter::Paren))
    return failure();

  // Parse loop bounds.
  SmallVector<OpAsmParser::UnresolvedOperand, 4> lower;
  if (parser.parseEqual() ||
      parser.parseOperandList(lower, ivs.size(),
                              OpAsmParser::Delimiter::Paren) ||
      parser.resolveOperands(lower, builder.getIndexType(), result.operands))
    return failure();

  SmallVector<OpAsmParser::UnresolvedOperand, 4> upper;
  if (parser.parseKeyword("to") ||
      parser.parseOperandList(upper, ivs.size(),
                              OpAsmParser::Delimiter::Paren) ||
      parser.resolveOperands(upper, builder.getIndexType(), result.operands))
    return failure();

  // Parse step values.
  SmallVector<OpAsmParser::UnresolvedOperand, 4> steps;
  if (parser.parseKeyword("step") ||
      parser.parseOperandList(steps, ivs.size(),
                              OpAsmParser::Delimiter::Paren) ||
      parser.resolveOperands(steps, builder.getIndexType(), result.operands))
    return failure();

  // Parse init values.
  SmallVector<OpAsmParser::UnresolvedOperand, 4> initVals;
  if (succeeded(parser.parseOptionalKeyword("init"))) {
    if (parser.parseOperandList(initVals, OpAsmParser::Delimiter::Paren))
      return failure();
  }

  // Parse optional results in case there is a reduce.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Now parse the body.
  Region *body = result.addRegion();
  for (auto &iv : ivs)
    iv.type = builder.getIndexType();
  if (parser.parseRegion(*body, ivs))
    return failure();

  // Set `operandSegmentSizes` attribute.
  result.addAttribute(
      ParallelOp::getOperandSegmentSizeAttr(),
      builder.getDenseI32ArrayAttr({static_cast<int32_t>(lower.size()),
                                    static_cast<int32_t>(upper.size()),
                                    static_cast<int32_t>(steps.size()),
                                    static_cast<int32_t>(initVals.size())}));

  // Parse attributes.
  if (parser.parseOptionalAttrDict(result.attributes) ||
      parser.resolveOperands(initVals, result.types, parser.getNameLoc(),
                             result.operands))
    return failure();

  // Add a terminator if none was parsed.
  ParallelOp::ensureTerminator(*body, builder, result.location);
  return success();
}

void ParallelOp::print(OpAsmPrinter &p) {
  p << " (" << getBody()->getArguments() << ") = (" << getLowerBound()
    << ") to (" << getUpperBound() << ") step (" << getStep() << ")";
  if (!getInitVals().empty())
    p << " init (" << getInitVals() << ")";
  p.printOptionalArrowTypeList(getResultTypes());
  p << ' ';
  p.printRegion(getRegion(), /*printEntryBlockArgs=*/false);
  p.printOptionalAttrDict(
      (*this)->getAttrs(),
      /*elidedAttrs=*/ParallelOp::getOperandSegmentSizeAttr());
}

SmallVector<Region *> ParallelOp::getLoopRegions() { return {&getRegion()}; }

std::optional<SmallVector<Value>> ParallelOp::getLoopInductionVars() {
  return SmallVector<Value>{getBody()->getArguments()};
}

std::optional<SmallVector<OpFoldResult>> ParallelOp::getLoopLowerBounds() {
  return getLowerBound();
}

std::optional<SmallVector<OpFoldResult>> ParallelOp::getLoopUpperBounds() {
  return getUpperBound();
}

std::optional<SmallVector<OpFoldResult>> ParallelOp::getLoopSteps() {
  return getStep();
}

ParallelOp mlir::scf::getParallelForInductionVarOwner(Value val) {
  auto ivArg = llvm::dyn_cast<BlockArgument>(val);
  if (!ivArg)
    return ParallelOp();
  assert(ivArg.getOwner() && "unlinked block argument");
  auto *containingOp = ivArg.getOwner()->getParentOp();
  return dyn_cast<ParallelOp>(containingOp);
}

namespace {
// Collapse loop dimensions that perform a single iteration.
struct ParallelOpSingleOrZeroIterationDimsFolder
    : public OpRewritePattern<ParallelOp> {
  using OpRewritePattern<ParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ParallelOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op.getLoc();

    // Compute new loop bounds that omit all single-iteration loop dimensions.
    SmallVector<Value> newLowerBounds, newUpperBounds, newSteps;
    IRMapping mapping;
    for (auto [lb, ub, step, iv] :
         llvm::zip(op.getLowerBound(), op.getUpperBound(), op.getStep(),
                   op.getInductionVars())) {
      auto numIterations = constantTripCount(lb, ub, step);
      if (numIterations.has_value()) {
        // Remove the loop if it performs zero iterations.
        if (*numIterations == 0) {
          rewriter.replaceOp(op, op.getInitVals());
          return success();
        }
        // Replace the loop induction variable by the lower bound if the loop
        // performs a single iteration. Otherwise, copy the loop bounds.
        if (*numIterations == 1) {
          mapping.map(iv, getValueOrCreateConstantIndexOp(rewriter, loc, lb));
          continue;
        }
      }
      newLowerBounds.push_back(lb);
      newUpperBounds.push_back(ub);
      newSteps.push_back(step);
    }
    // Exit if none of the loop dimensions perform a single iteration.
    if (newLowerBounds.size() == op.getLowerBound().size())
      return failure();

    if (newLowerBounds.empty()) {
      // All of the loop dimensions perform a single iteration. Inline
      // loop body and nested ReduceOp's
      SmallVector<Value> results;
      results.reserve(op.getInitVals().size());
      for (auto &bodyOp : op.getBody()->without_terminator())
        rewriter.clone(bodyOp, mapping);
      auto reduceOp = cast<ReduceOp>(op.getBody()->getTerminator());
      for (int64_t i = 0, e = reduceOp.getReductions().size(); i < e; ++i) {
        Block &reduceBlock = reduceOp.getReductions()[i].front();
        auto initValIndex = results.size();
        mapping.map(reduceBlock.getArgument(0), op.getInitVals()[initValIndex]);
        mapping.map(reduceBlock.getArgument(1),
                    mapping.lookupOrDefault(reduceOp.getOperands()[i]));
        for (auto &reduceBodyOp : reduceBlock.without_terminator())
          rewriter.clone(reduceBodyOp, mapping);

        auto result = mapping.lookupOrDefault(
            cast<ReduceReturnOp>(reduceBlock.getTerminator()).getResult());
        results.push_back(result);
      }

      rewriter.replaceOp(op, results);
      return success();
    }
    // Replace the parallel loop by lower-dimensional parallel loop.
    auto newOp =
        rewriter.create<ParallelOp>(op.getLoc(), newLowerBounds, newUpperBounds,
                                    newSteps, op.getInitVals(), nullptr);
    // Erase the empty block that was inserted by the builder.
    rewriter.eraseBlock(newOp.getBody());
    // Clone the loop body and remap the block arguments of the collapsed loops
    // (inlining does not support a cancellable block argument mapping).
    rewriter.cloneRegionBefore(op.getRegion(), newOp.getRegion(),
                               newOp.getRegion().begin(), mapping);
    rewriter.replaceOp(op, newOp.getResults());
    return success();
  }
};

struct MergeNestedParallelLoops : public OpRewritePattern<ParallelOp> {
  using OpRewritePattern<ParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ParallelOp op,
                                PatternRewriter &rewriter) const override {
    Block &outerBody = *op.getBody();
    if (!llvm::hasSingleElement(outerBody.without_terminator()))
      return failure();

    auto innerOp = dyn_cast<ParallelOp>(outerBody.front());
    if (!innerOp)
      return failure();

    for (auto val : outerBody.getArguments())
      if (llvm::is_contained(innerOp.getLowerBound(), val) ||
          llvm::is_contained(innerOp.getUpperBound(), val) ||
          llvm::is_contained(innerOp.getStep(), val))
        return failure();

    // Reductions are not supported yet.
    if (!op.getInitVals().empty() || !innerOp.getInitVals().empty())
      return failure();

    auto bodyBuilder = [&](OpBuilder &builder, Location /*loc*/,
                           ValueRange iterVals, ValueRange) {
      Block &innerBody = *innerOp.getBody();
      assert(iterVals.size() ==
             (outerBody.getNumArguments() + innerBody.getNumArguments()));
      IRMapping mapping;
      mapping.map(outerBody.getArguments(),
                  iterVals.take_front(outerBody.getNumArguments()));
      mapping.map(innerBody.getArguments(),
                  iterVals.take_back(innerBody.getNumArguments()));
      for (Operation &op : innerBody.without_terminator())
        builder.clone(op, mapping);
    };

    auto concatValues = [](const auto &first, const auto &second) {
      SmallVector<Value> ret;
      ret.reserve(first.size() + second.size());
      ret.assign(first.begin(), first.end());
      ret.append(second.begin(), second.end());
      return ret;
    };

    auto newLowerBounds =
        concatValues(op.getLowerBound(), innerOp.getLowerBound());
    auto newUpperBounds =
        concatValues(op.getUpperBound(), innerOp.getUpperBound());
    auto newSteps = concatValues(op.getStep(), innerOp.getStep());

    rewriter.replaceOpWithNewOp<ParallelOp>(op, newLowerBounds, newUpperBounds,
                                            newSteps, ValueRange(),
                                            bodyBuilder);
    return success();
  }
};

} // namespace

void ParallelOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                             MLIRContext *context) {
  results
      .add<ParallelOpSingleOrZeroIterationDimsFolder, MergeNestedParallelLoops>(
          context);
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes that
/// correspond to a constant value for each operand, or null if that operand is
/// not a constant.
void ParallelOp::getSuccessorRegions(
    RegionBranchPoint point, SmallVectorImpl<RegionSuccessor> &regions) {
  // Both the operation itself and the region may be branching into the body or
  // back into the operation itself. It is possible for loop not to enter the
  // body.
  regions.push_back(RegionSuccessor(&getRegion()));
  regions.push_back(RegionSuccessor());
}

//===----------------------------------------------------------------------===//
// ReduceOp
//===----------------------------------------------------------------------===//

void ReduceOp::build(OpBuilder &builder, OperationState &result) {}

void ReduceOp::build(OpBuilder &builder, OperationState &result,
                     ValueRange operands) {
  result.addOperands(operands);
  for (Value v : operands) {
    OpBuilder::InsertionGuard guard(builder);
    Region *bodyRegion = result.addRegion();
    builder.createBlock(bodyRegion, {},
                        ArrayRef<Type>{v.getType(), v.getType()},
                        {result.location, result.location});
  }
}

LogicalResult ReduceOp::verifyRegions() {
  // The region of a ReduceOp has two arguments of the same type as its
  // corresponding operand.
  for (int64_t i = 0, e = getReductions().size(); i < e; ++i) {
    auto type = getOperands()[i].getType();
    Block &block = getReductions()[i].front();
    if (block.empty())
      return emitOpError() << i << "-th reduction has an empty body";
    if (block.getNumArguments() != 2 ||
        llvm::any_of(block.getArguments(), [&](const BlockArgument &arg) {
          return arg.getType() != type;
        }))
      return emitOpError() << "expected two block arguments with type " << type
                           << " in the " << i << "-th reduction region";

    // Check that the block is terminated by a ReduceReturnOp.
    if (!isa<ReduceReturnOp>(block.getTerminator()))
      return emitOpError("reduction bodies must be terminated with an "
                         "'scf.reduce.return' op");
  }

  return success();
}

MutableOperandRange
ReduceOp::getMutableSuccessorOperands(RegionBranchPoint point) {
  // No operands are forwarded to the next iteration.
  return MutableOperandRange(getOperation(), /*start=*/0, /*length=*/0);
}

//===----------------------------------------------------------------------===//
// ReduceReturnOp
//===----------------------------------------------------------------------===//

LogicalResult ReduceReturnOp::verify() {
  // The type of the return value should be the same type as the types of the
  // block arguments of the reduction body.
  Block *reductionBody = getOperation()->getBlock();
  // Should already be verified by an op trait.
  assert(isa<ReduceOp>(reductionBody->getParentOp()) && "expected scf.reduce");
  Type expectedResultType = reductionBody->getArgument(0).getType();
  if (expectedResultType != getResult().getType())
    return emitOpError() << "must have type " << expectedResultType
                         << " (the type of the reduction inputs)";
  return success();
}

//===----------------------------------------------------------------------===//
// WhileOp
//===----------------------------------------------------------------------===//

void WhileOp::build(::mlir::OpBuilder &odsBuilder,
                    ::mlir::OperationState &odsState, TypeRange resultTypes,
                    ValueRange inits, BodyBuilderFn beforeBuilder,
                    BodyBuilderFn afterBuilder) {
  odsState.addOperands(inits);
  odsState.addTypes(resultTypes);

  OpBuilder::InsertionGuard guard(odsBuilder);

  // Build before region.
  SmallVector<Location, 4> beforeArgLocs;
  beforeArgLocs.reserve(inits.size());
  for (Value operand : inits) {
    beforeArgLocs.push_back(operand.getLoc());
  }

  Region *beforeRegion = odsState.addRegion();
  Block *beforeBlock = odsBuilder.createBlock(beforeRegion, /*insertPt=*/{},
                                              inits.getTypes(), beforeArgLocs);
  if (beforeBuilder)
    beforeBuilder(odsBuilder, odsState.location, beforeBlock->getArguments());

  // Build after region.
  SmallVector<Location, 4> afterArgLocs(resultTypes.size(), odsState.location);

  Region *afterRegion = odsState.addRegion();
  Block *afterBlock = odsBuilder.createBlock(afterRegion, /*insertPt=*/{},
                                             resultTypes, afterArgLocs);

  if (afterBuilder)
    afterBuilder(odsBuilder, odsState.location, afterBlock->getArguments());
}

ConditionOp WhileOp::getConditionOp() {
  return cast<ConditionOp>(getBeforeBody()->getTerminator());
}

YieldOp WhileOp::getYieldOp() {
  return cast<YieldOp>(getAfterBody()->getTerminator());
}

std::optional<MutableArrayRef<OpOperand>> WhileOp::getYieldedValuesMutable() {
  return getYieldOp().getResultsMutable();
}

Block::BlockArgListType WhileOp::getBeforeArguments() {
  return getBeforeBody()->getArguments();
}

Block::BlockArgListType WhileOp::getAfterArguments() {
  return getAfterBody()->getArguments();
}

Block::BlockArgListType WhileOp::getRegionIterArgs() {
  return getBeforeArguments();
}

OperandRange WhileOp::getEntrySuccessorOperands(RegionBranchPoint point) {
  assert(point == getBefore() &&
         "WhileOp is expected to branch only to the first region");
  return getInits();
}

void WhileOp::getSuccessorRegions(RegionBranchPoint point,
                                  SmallVectorImpl<RegionSuccessor> &regions) {
  // The parent op always branches to the condition region.
  if (point.isParent()) {
    regions.emplace_back(&getBefore(), getBefore().getArguments());
    return;
  }

  assert(llvm::is_contained({&getAfter(), &getBefore()}, point) &&
         "there are only two regions in a WhileOp");
  // The body region always branches back to the condition region.
  if (point == getAfter()) {
    regions.emplace_back(&getBefore(), getBefore().getArguments());
    return;
  }

  regions.emplace_back(getResults());
  regions.emplace_back(&getAfter(), getAfter().getArguments());
}

SmallVector<Region *> WhileOp::getLoopRegions() {
  return {&getBefore(), &getAfter()};
}

/// Parses a `while` op.
///
/// op ::= `scf.while` assignments `:` function-type region `do` region
///         `attributes` attribute-dict
/// initializer ::= /* empty */ | `(` assignment-list `)`
/// assignment-list ::= assignment | assignment `,` assignment-list
/// assignment ::= ssa-value `=` ssa-value
ParseResult scf::WhileOp::parse(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::Argument, 4> regionArgs;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> operands;
  Region *before = result.addRegion();
  Region *after = result.addRegion();

  OptionalParseResult listResult =
      parser.parseOptionalAssignmentList(regionArgs, operands);
  if (listResult.has_value() && failed(listResult.value()))
    return failure();

  FunctionType functionType;
  SMLoc typeLoc = parser.getCurrentLocation();
  if (failed(parser.parseColonType(functionType)))
    return failure();

  result.addTypes(functionType.getResults());

  if (functionType.getNumInputs() != operands.size()) {
    return parser.emitError(typeLoc)
           << "expected as many input types as operands "
           << "(expected " << operands.size() << " got "
           << functionType.getNumInputs() << ")";
  }

  // Resolve input operands.
  if (failed(parser.resolveOperands(operands, functionType.getInputs(),
                                    parser.getCurrentLocation(),
                                    result.operands)))
    return failure();

  // Propagate the types into the region arguments.
  for (size_t i = 0, e = regionArgs.size(); i != e; ++i)
    regionArgs[i].type = functionType.getInput(i);

  return failure(parser.parseRegion(*before, regionArgs) ||
                 parser.parseKeyword("do") || parser.parseRegion(*after) ||
                 parser.parseOptionalAttrDictWithKeyword(result.attributes));
}

/// Prints a `while` op.
void scf::WhileOp::print(OpAsmPrinter &p) {
  printInitializationList(p, getBeforeArguments(), getInits(), " ");
  p << " : ";
  p.printFunctionalType(getInits().getTypes(), getResults().getTypes());
  p << ' ';
  p.printRegion(getBefore(), /*printEntryBlockArgs=*/false);
  p << " do ";
  p.printRegion(getAfter());
  p.printOptionalAttrDictWithKeyword((*this)->getAttrs());
}

/// Verifies that two ranges of types match, i.e. have the same number of
/// entries and that types are pairwise equals. Reports errors on the given
/// operation in case of mismatch.
template <typename OpTy>
static LogicalResult verifyTypeRangesMatch(OpTy op, TypeRange left,
                                           TypeRange right, StringRef message) {
  if (left.size() != right.size())
    return op.emitOpError("expects the same number of ") << message;

  for (unsigned i = 0, e = left.size(); i < e; ++i) {
    if (left[i] != right[i]) {
      InFlightDiagnostic diag = op.emitOpError("expects the same types for ")
                                << message;
      diag.attachNote() << "for argument " << i << ", found " << left[i]
                        << " and " << right[i];
      return diag;
    }
  }

  return success();
}

LogicalResult scf::WhileOp::verify() {
  auto beforeTerminator = verifyAndGetTerminator<scf::ConditionOp>(
      *this, getBefore(),
      "expects the 'before' region to terminate with 'scf.condition'");
  if (!beforeTerminator)
    return failure();

  auto afterTerminator = verifyAndGetTerminator<scf::YieldOp>(
      *this, getAfter(),
      "expects the 'after' region to terminate with 'scf.yield'");
  return success(afterTerminator != nullptr);
}

namespace {
/// Replace uses of the condition within the do block with true, since otherwise
/// the block would not be evaluated.
///
/// scf.while (..) : (i1, ...) -> ... {
///  %condition = call @evaluate_condition() : () -> i1
///  scf.condition(%condition) %condition : i1, ...
/// } do {
/// ^bb0(%arg0: i1, ...):
///    use(%arg0)
///    ...
///
/// becomes
/// scf.while (..) : (i1, ...) -> ... {
///  %condition = call @evaluate_condition() : () -> i1
///  scf.condition(%condition) %condition : i1, ...
/// } do {
/// ^bb0(%arg0: i1, ...):
///    use(%true)
///    ...
struct WhileConditionTruth : public OpRewritePattern<WhileOp> {
  using OpRewritePattern<WhileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp op,
                                PatternRewriter &rewriter) const override {
    auto term = op.getConditionOp();

    // These variables serve to prevent creating duplicate constants
    // and hold constant true or false values.
    Value constantTrue = nullptr;

    bool replaced = false;
    for (auto yieldedAndBlockArgs :
         llvm::zip(term.getArgs(), op.getAfterArguments())) {
      if (std::get<0>(yieldedAndBlockArgs) == term.getCondition()) {
        if (!std::get<1>(yieldedAndBlockArgs).use_empty()) {
          if (!constantTrue)
            constantTrue = rewriter.create<arith::ConstantOp>(
                op.getLoc(), term.getCondition().getType(),
                rewriter.getBoolAttr(true));

          rewriter.replaceAllUsesWith(std::get<1>(yieldedAndBlockArgs),
                                      constantTrue);
          replaced = true;
        }
      }
    }
    return success(replaced);
  }
};

/// Remove loop invariant arguments from `before` block of scf.while.
/// A before block argument is considered loop invariant if :-
///   1. i-th yield operand is equal to the i-th while operand.
///   2. i-th yield operand is k-th after block argument which is (k+1)-th
///      condition operand AND this (k+1)-th condition operand is equal to i-th
///      iter argument/while operand.
/// For the arguments which are removed, their uses inside scf.while
/// are replaced with their corresponding initial value.
///
/// Eg:
///    INPUT :-
///    %res = scf.while <...> iter_args(%arg0_before = %a, %arg1_before = %b,
///                                     ..., %argN_before = %N)
///           {
///                ...
///                scf.condition(%cond) %arg1_before, %arg0_before,
///                                     %arg2_before, %arg0_before, ...
///           } do {
///             ^bb0(%arg1_after, %arg0_after_1, %arg2_after, %arg0_after_2,
///                  ..., %argK_after):
///                ...
///                scf.yield %arg0_after_2, %b, %arg1_after, ..., %argN
///           }
///
///    OUTPUT :-
///    %res = scf.while <...> iter_args(%arg2_before = %c, ..., %argN_before =
///                                     %N)
///           {
///                ...
///                scf.condition(%cond) %b, %a, %arg2_before, %a, ...
///           } do {
///             ^bb0(%arg1_after, %arg0_after_1, %arg2_after, %arg0_after_2,
///                  ..., %argK_after):
///                ...
///                scf.yield %arg1_after, ..., %argN
///           }
///
///    EXPLANATION:
///      We iterate over each yield operand.
///        1. 0-th yield operand %arg0_after_2 is 4-th condition operand
///           %arg0_before, which in turn is the 0-th iter argument. So we
///           remove 0-th before block argument and yield operand, and replace
///           all uses of the 0-th before block argument with its initial value
///           %a.
///        2. 1-th yield operand %b is equal to the 1-th iter arg's initial
///           value. So we remove this operand and the corresponding before
///           block argument and replace all uses of 1-th before block argument
///           with %b.
struct RemoveLoopInvariantArgsFromBeforeBlock
    : public OpRewritePattern<WhileOp> {
  using OpRewritePattern<WhileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp op,
                                PatternRewriter &rewriter) const override {
    Block &afterBlock = *op.getAfterBody();
    Block::BlockArgListType beforeBlockArgs = op.getBeforeArguments();
    ConditionOp condOp = op.getConditionOp();
    OperandRange condOpArgs = condOp.getArgs();
    Operation *yieldOp = afterBlock.getTerminator();
    ValueRange yieldOpArgs = yieldOp->getOperands();

    bool canSimplify = false;
    for (const auto &it :
         llvm::enumerate(llvm::zip(op.getOperands(), yieldOpArgs))) {
      auto index = static_cast<unsigned>(it.index());
      auto [initVal, yieldOpArg] = it.value();
      // If i-th yield operand is equal to the i-th operand of the scf.while,
      // the i-th before block argument is a loop invariant.
      if (yieldOpArg == initVal) {
        canSimplify = true;
        break;
      }
      // If the i-th yield operand is k-th after block argument, then we check
      // if the (k+1)-th condition op operand is equal to either the i-th before
      // block argument or the initial value of i-th before block argument. If
      // the comparison results `true`, i-th before block argument is a loop
      // invariant.
      auto yieldOpBlockArg = llvm::dyn_cast<BlockArgument>(yieldOpArg);
      if (yieldOpBlockArg && yieldOpBlockArg.getOwner() == &afterBlock) {
        Value condOpArg = condOpArgs[yieldOpBlockArg.getArgNumber()];
        if (condOpArg == beforeBlockArgs[index] || condOpArg == initVal) {
          canSimplify = true;
          break;
        }
      }
    }

    if (!canSimplify)
      return failure();

    SmallVector<Value> newInitArgs, newYieldOpArgs;
    DenseMap<unsigned, Value> beforeBlockInitValMap;
    SmallVector<Location> newBeforeBlockArgLocs;
    for (const auto &it :
         llvm::enumerate(llvm::zip(op.getOperands(), yieldOpArgs))) {
      auto index = static_cast<unsigned>(it.index());
      auto [initVal, yieldOpArg] = it.value();

      // If i-th yield operand is equal to the i-th operand of the scf.while,
      // the i-th before block argument is a loop invariant.
      if (yieldOpArg == initVal) {
        beforeBlockInitValMap.insert({index, initVal});
        continue;
      } else {
        // If the i-th yield operand is k-th after block argument, then we check
        // if the (k+1)-th condition op operand is equal to either the i-th
        // before block argument or the initial value of i-th before block
        // argument. If the comparison results `true`, i-th before block
        // argument is a loop invariant.
        auto yieldOpBlockArg = llvm::dyn_cast<BlockArgument>(yieldOpArg);
        if (yieldOpBlockArg && yieldOpBlockArg.getOwner() == &afterBlock) {
          Value condOpArg = condOpArgs[yieldOpBlockArg.getArgNumber()];
          if (condOpArg == beforeBlockArgs[index] || condOpArg == initVal) {
            beforeBlockInitValMap.insert({index, initVal});
            continue;
          }
        }
      }
      newInitArgs.emplace_back(initVal);
      newYieldOpArgs.emplace_back(yieldOpArg);
      newBeforeBlockArgLocs.emplace_back(beforeBlockArgs[index].getLoc());
    }

    {
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPoint(yieldOp);
      rewriter.replaceOpWithNewOp<YieldOp>(yieldOp, newYieldOpArgs);
    }

    auto newWhile =
        rewriter.create<WhileOp>(op.getLoc(), op.getResultTypes(), newInitArgs);

    Block &newBeforeBlock = *rewriter.createBlock(
        &newWhile.getBefore(), /*insertPt*/ {},
        ValueRange(newYieldOpArgs).getTypes(), newBeforeBlockArgLocs);

    Block &beforeBlock = *op.getBeforeBody();
    SmallVector<Value> newBeforeBlockArgs(beforeBlock.getNumArguments());
    // For each i-th before block argument we find it's replacement value as :-
    //   1. If i-th before block argument is a loop invariant, we fetch it's
    //      initial value from `beforeBlockInitValMap` by querying for key `i`.
    //   2. Else we fetch j-th new before block argument as the replacement
    //      value of i-th before block argument.
    for (unsigned i = 0, j = 0, n = beforeBlock.getNumArguments(); i < n; i++) {
      // If the index 'i' argument was a loop invariant we fetch it's initial
      // value from `beforeBlockInitValMap`.
      if (beforeBlockInitValMap.count(i) != 0)
        newBeforeBlockArgs[i] = beforeBlockInitValMap[i];
      else
        newBeforeBlockArgs[i] = newBeforeBlock.getArgument(j++);
    }

    rewriter.mergeBlocks(&beforeBlock, &newBeforeBlock, newBeforeBlockArgs);
    rewriter.inlineRegionBefore(op.getAfter(), newWhile.getAfter(),
                                newWhile.getAfter().begin());

    rewriter.replaceOp(op, newWhile.getResults());
    return success();
  }
};

/// Remove loop invariant value from result (condition op) of scf.while.
/// A value is considered loop invariant if the final value yielded by
/// scf.condition is defined outside of the `before` block. We remove the
/// corresponding argument in `after` block and replace the use with the value.
/// We also replace the use of the corresponding result of scf.while with the
/// value.
///
/// Eg:
///    INPUT :-
///    %res_input:K = scf.while <...> iter_args(%arg0_before = , ...,
///                                             %argN_before = %N) {
///                ...
///                scf.condition(%cond) %arg0_before, %a, %b, %arg1_before, ...
///           } do {
///             ^bb0(%arg0_after, %arg1_after, %arg2_after, ..., %argK_after):
///                ...
///                some_func(%arg1_after)
///                ...
///                scf.yield %arg0_after, %arg2_after, ..., %argN_after
///           }
///
///    OUTPUT :-
///    %res_output:M = scf.while <...> iter_args(%arg0 = , ..., %argN = %N) {
///                ...
///                scf.condition(%cond) %arg0, %arg1, ..., %argM
///           } do {
///             ^bb0(%arg0, %arg3, ..., %argM):
///                ...
///                some_func(%a)
///                ...
///                scf.yield %arg0, %b, ..., %argN
///           }
///
///     EXPLANATION:
///       1. The 1-th and 2-th operand of scf.condition are defined outside the
///          before block of scf.while, so they get removed.
///       2. %res_input#1's uses are replaced by %a and %res_input#2's uses are
///          replaced by %b.
///       3. The corresponding after block argument %arg1_after's uses are
///          replaced by %a and %arg2_after's uses are replaced by %b.
struct RemoveLoopInvariantValueYielded : public OpRewritePattern<WhileOp> {
  using OpRewritePattern<WhileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp op,
                                PatternRewriter &rewriter) const override {
    Block &beforeBlock = *op.getBeforeBody();
    ConditionOp condOp = op.getConditionOp();
    OperandRange condOpArgs = condOp.getArgs();

    bool canSimplify = false;
    for (Value condOpArg : condOpArgs) {
      // Those values not defined within `before` block will be considered as
      // loop invariant values. We map the corresponding `index` with their
      // value.
      if (condOpArg.getParentBlock() != &beforeBlock) {
        canSimplify = true;
        break;
      }
    }

    if (!canSimplify)
      return failure();

    Block::BlockArgListType afterBlockArgs = op.getAfterArguments();

    SmallVector<Value> newCondOpArgs;
    SmallVector<Type> newAfterBlockType;
    DenseMap<unsigned, Value> condOpInitValMap;
    SmallVector<Location> newAfterBlockArgLocs;
    for (const auto &it : llvm::enumerate(condOpArgs)) {
      auto index = static_cast<unsigned>(it.index());
      Value condOpArg = it.value();
      // Those values not defined within `before` block will be considered as
      // loop invariant values. We map the corresponding `index` with their
      // value.
      if (condOpArg.getParentBlock() != &beforeBlock) {
        condOpInitValMap.insert({index, condOpArg});
      } else {
        newCondOpArgs.emplace_back(condOpArg);
        newAfterBlockType.emplace_back(condOpArg.getType());
        newAfterBlockArgLocs.emplace_back(afterBlockArgs[index].getLoc());
      }
    }

    {
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPoint(condOp);
      rewriter.replaceOpWithNewOp<ConditionOp>(condOp, condOp.getCondition(),
                                               newCondOpArgs);
    }

    auto newWhile = rewriter.create<WhileOp>(op.getLoc(), newAfterBlockType,
                                             op.getOperands());

    Block &newAfterBlock =
        *rewriter.createBlock(&newWhile.getAfter(), /*insertPt*/ {},
                              newAfterBlockType, newAfterBlockArgLocs);

    Block &afterBlock = *op.getAfterBody();
    // Since a new scf.condition op was created, we need to fetch the new
    // `after` block arguments which will be used while replacing operations of
    // previous scf.while's `after` blocks. We'd also be fetching new result
    // values too.
    SmallVector<Value> newAfterBlockArgs(afterBlock.getNumArguments());
    SmallVector<Value> newWhileResults(afterBlock.getNumArguments());
    for (unsigned i = 0, j = 0, n = afterBlock.getNumArguments(); i < n; i++) {
      Value afterBlockArg, result;
      // If index 'i' argument was loop invariant we fetch it's value from the
      // `condOpInitMap` map.
      if (condOpInitValMap.count(i) != 0) {
        afterBlockArg = condOpInitValMap[i];
        result = afterBlockArg;
      } else {
        afterBlockArg = newAfterBlock.getArgument(j);
        result = newWhile.getResult(j);
        j++;
      }
      newAfterBlockArgs[i] = afterBlockArg;
      newWhileResults[i] = result;
    }

    rewriter.mergeBlocks(&afterBlock, &newAfterBlock, newAfterBlockArgs);
    rewriter.inlineRegionBefore(op.getBefore(), newWhile.getBefore(),
                                newWhile.getBefore().begin());

    rewriter.replaceOp(op, newWhileResults);
    return success();
  }
};

/// Remove WhileOp results that are also unused in 'after' block.
///
///  %0:2 = scf.while () : () -> (i32, i64) {
///    %condition = "test.condition"() : () -> i1
///    %v1 = "test.get_some_value"() : () -> i32
///    %v2 = "test.get_some_value"() : () -> i64
///    scf.condition(%condition) %v1, %v2 : i32, i64
///  } do {
///  ^bb0(%arg0: i32, %arg1: i64):
///    "test.use"(%arg0) : (i32) -> ()
///    scf.yield
///  }
///  return %0#0 : i32
///
/// becomes
///  %0 = scf.while () : () -> (i32) {
///    %condition = "test.condition"() : () -> i1
///    %v1 = "test.get_some_value"() : () -> i32
///    %v2 = "test.get_some_value"() : () -> i64
///    scf.condition(%condition) %v1 : i32
///  } do {
///  ^bb0(%arg0: i32):
///    "test.use"(%arg0) : (i32) -> ()
///    scf.yield
///  }
///  return %0 : i32
struct WhileUnusedResult : public OpRewritePattern<WhileOp> {
  using OpRewritePattern<WhileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp op,
                                PatternRewriter &rewriter) const override {
    auto term = op.getConditionOp();
    auto afterArgs = op.getAfterArguments();
    auto termArgs = term.getArgs();

    // Collect results mapping, new terminator args and new result types.
    SmallVector<unsigned> newResultsIndices;
    SmallVector<Type> newResultTypes;
    SmallVector<Value> newTermArgs;
    SmallVector<Location> newArgLocs;
    bool needUpdate = false;
    for (const auto &it :
         llvm::enumerate(llvm::zip(op.getResults(), afterArgs, termArgs))) {
      auto i = static_cast<unsigned>(it.index());
      Value result = std::get<0>(it.value());
      Value afterArg = std::get<1>(it.value());
      Value termArg = std::get<2>(it.value());
      if (result.use_empty() && afterArg.use_empty()) {
        needUpdate = true;
      } else {
        newResultsIndices.emplace_back(i);
        newTermArgs.emplace_back(termArg);
        newResultTypes.emplace_back(result.getType());
        newArgLocs.emplace_back(result.getLoc());
      }
    }

    if (!needUpdate)
      return failure();

    {
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPoint(term);
      rewriter.replaceOpWithNewOp<ConditionOp>(term, term.getCondition(),
                                               newTermArgs);
    }

    auto newWhile =
        rewriter.create<WhileOp>(op.getLoc(), newResultTypes, op.getInits());

    Block &newAfterBlock = *rewriter.createBlock(
        &newWhile.getAfter(), /*insertPt*/ {}, newResultTypes, newArgLocs);

    // Build new results list and new after block args (unused entries will be
    // null).
    SmallVector<Value> newResults(op.getNumResults());
    SmallVector<Value> newAfterBlockArgs(op.getNumResults());
    for (const auto &it : llvm::enumerate(newResultsIndices)) {
      newResults[it.value()] = newWhile.getResult(it.index());
      newAfterBlockArgs[it.value()] = newAfterBlock.getArgument(it.index());
    }

    rewriter.inlineRegionBefore(op.getBefore(), newWhile.getBefore(),
                                newWhile.getBefore().begin());

    Block &afterBlock = *op.getAfterBody();
    rewriter.mergeBlocks(&afterBlock, &newAfterBlock, newAfterBlockArgs);

    rewriter.replaceOp(op, newResults);
    return success();
  }
};

/// Replace operations equivalent to the condition in the do block with true,
/// since otherwise the block would not be evaluated.
///
/// scf.while (..) : (i32, ...) -> ... {
///  %z = ... : i32
///  %condition = cmpi pred %z, %a
///  scf.condition(%condition) %z : i32, ...
/// } do {
/// ^bb0(%arg0: i32, ...):
///    %condition2 = cmpi pred %arg0, %a
///    use(%condition2)
///    ...
///
/// becomes
/// scf.while (..) : (i32, ...) -> ... {
///  %z = ... : i32
///  %condition = cmpi pred %z, %a
///  scf.condition(%condition) %z : i32, ...
/// } do {
/// ^bb0(%arg0: i32, ...):
///    use(%true)
///    ...
struct WhileCmpCond : public OpRewritePattern<scf::WhileOp> {
  using OpRewritePattern<scf::WhileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::WhileOp op,
                                PatternRewriter &rewriter) const override {
    using namespace scf;
    auto cond = op.getConditionOp();
    auto cmp = cond.getCondition().getDefiningOp<arith::CmpIOp>();
    if (!cmp)
      return failure();
    bool changed = false;
    for (auto tup : llvm::zip(cond.getArgs(), op.getAfterArguments())) {
      for (size_t opIdx = 0; opIdx < 2; opIdx++) {
        if (std::get<0>(tup) != cmp.getOperand(opIdx))
          continue;
        for (OpOperand &u :
             llvm::make_early_inc_range(std::get<1>(tup).getUses())) {
          auto cmp2 = dyn_cast<arith::CmpIOp>(u.getOwner());
          if (!cmp2)
            continue;
          // For a binary operator 1-opIdx gets the other side.
          if (cmp2.getOperand(1 - opIdx) != cmp.getOperand(1 - opIdx))
            continue;
          bool samePredicate;
          if (cmp2.getPredicate() == cmp.getPredicate())
            samePredicate = true;
          else if (cmp2.getPredicate() ==
                   arith::invertPredicate(cmp.getPredicate()))
            samePredicate = false;
          else
            continue;

          rewriter.replaceOpWithNewOp<arith::ConstantIntOp>(cmp2, samePredicate,
                                                            1);
          changed = true;
        }
      }
    }
    return success(changed);
  }
};

/// Remove unused init/yield args.
struct WhileRemoveUnusedArgs : public OpRewritePattern<WhileOp> {
  using OpRewritePattern<WhileOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp op,
                                PatternRewriter &rewriter) const override {

    if (!llvm::any_of(op.getBeforeArguments(),
                      [](Value arg) { return arg.use_empty(); }))
      return rewriter.notifyMatchFailure(op, "No args to remove");

    YieldOp yield = op.getYieldOp();

    // Collect results mapping, new terminator args and new result types.
    SmallVector<Value> newYields;
    SmallVector<Value> newInits;
    llvm::BitVector argsToErase;

    size_t argsCount = op.getBeforeArguments().size();
    newYields.reserve(argsCount);
    newInits.reserve(argsCount);
    argsToErase.reserve(argsCount);
    for (auto &&[beforeArg, yieldValue, initValue] : llvm::zip(
             op.getBeforeArguments(), yield.getOperands(), op.getInits())) {
      if (beforeArg.use_empty()) {
        argsToErase.push_back(true);
      } else {
        argsToErase.push_back(false);
        newYields.emplace_back(yieldValue);
        newInits.emplace_back(initValue);
      }
    }

    Block &beforeBlock = *op.getBeforeBody();
    Block &afterBlock = *op.getAfterBody();

    beforeBlock.eraseArguments(argsToErase);

    Location loc = op.getLoc();
    auto newWhileOp =
        rewriter.create<WhileOp>(loc, op.getResultTypes(), newInits,
                                 /*beforeBody*/ nullptr, /*afterBody*/ nullptr);
    Block &newBeforeBlock = *newWhileOp.getBeforeBody();
    Block &newAfterBlock = *newWhileOp.getAfterBody();

    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(yield);
    rewriter.replaceOpWithNewOp<YieldOp>(yield, newYields);

    rewriter.mergeBlocks(&beforeBlock, &newBeforeBlock,
                         newBeforeBlock.getArguments());
    rewriter.mergeBlocks(&afterBlock, &newAfterBlock,
                         newAfterBlock.getArguments());

    rewriter.replaceOp(op, newWhileOp.getResults());
    return success();
  }
};

/// Remove duplicated ConditionOp args.
struct WhileRemoveDuplicatedResults : public OpRewritePattern<WhileOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp op,
                                PatternRewriter &rewriter) const override {
    ConditionOp condOp = op.getConditionOp();
    ValueRange condOpArgs = condOp.getArgs();

    llvm::SmallPtrSet<Value, 8> argsSet(llvm::from_range, condOpArgs);

    if (argsSet.size() == condOpArgs.size())
      return rewriter.notifyMatchFailure(op, "No results to remove");

    llvm::SmallDenseMap<Value, unsigned> argsMap;
    SmallVector<Value> newArgs;
    argsMap.reserve(condOpArgs.size());
    newArgs.reserve(condOpArgs.size());
    for (Value arg : condOpArgs) {
      if (!argsMap.count(arg)) {
        auto pos = static_cast<unsigned>(argsMap.size());
        argsMap.insert({arg, pos});
        newArgs.emplace_back(arg);
      }
    }

    ValueRange argsRange(newArgs);

    Location loc = op.getLoc();
    auto newWhileOp = rewriter.create<scf::WhileOp>(
        loc, argsRange.getTypes(), op.getInits(), /*beforeBody*/ nullptr,
        /*afterBody*/ nullptr);
    Block &newBeforeBlock = *newWhileOp.getBeforeBody();
    Block &newAfterBlock = *newWhileOp.getAfterBody();

    SmallVector<Value> afterArgsMapping;
    SmallVector<Value> resultsMapping;
    for (auto &&[i, arg] : llvm::enumerate(condOpArgs)) {
      auto it = argsMap.find(arg);
      assert(it != argsMap.end());
      auto pos = it->second;
      afterArgsMapping.emplace_back(newAfterBlock.getArgument(pos));
      resultsMapping.emplace_back(newWhileOp->getResult(pos));
    }

    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(condOp);
    rewriter.replaceOpWithNewOp<ConditionOp>(condOp, condOp.getCondition(),
                                             argsRange);

    Block &beforeBlock = *op.getBeforeBody();
    Block &afterBlock = *op.getAfterBody();

    rewriter.mergeBlocks(&beforeBlock, &newBeforeBlock,
                         newBeforeBlock.getArguments());
    rewriter.mergeBlocks(&afterBlock, &newAfterBlock, afterArgsMapping);
    rewriter.replaceOp(op, resultsMapping);
    return success();
  }
};

/// If both ranges contain same values return mappping indices from args2 to
/// args1. Otherwise return std::nullopt.
static std::optional<SmallVector<unsigned>> getArgsMapping(ValueRange args1,
                                                           ValueRange args2) {
  if (args1.size() != args2.size())
    return std::nullopt;

  SmallVector<unsigned> ret(args1.size());
  for (auto &&[i, arg1] : llvm::enumerate(args1)) {
    auto it = llvm::find(args2, arg1);
    if (it == args2.end())
      return std::nullopt;

    ret[std::distance(args2.begin(), it)] = static_cast<unsigned>(i);
  }

  return ret;
}

static bool hasDuplicates(ValueRange args) {
  llvm::SmallDenseSet<Value> set;
  for (Value arg : args) {
    if (!set.insert(arg).second)
      return true;
  }
  return false;
}

/// If `before` block args are directly forwarded to `scf.condition`, rearrange
/// `scf.condition` args into same order as block args. Update `after` block
/// args and op result values accordingly.
/// Needed to simplify `scf.while` -> `scf.for` uplifting.
struct WhileOpAlignBeforeArgs : public OpRewritePattern<WhileOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileOp loop,
                                PatternRewriter &rewriter) const override {
    auto oldBefore = loop.getBeforeBody();
    ConditionOp oldTerm = loop.getConditionOp();
    ValueRange beforeArgs = oldBefore->getArguments();
    ValueRange termArgs = oldTerm.getArgs();
    if (beforeArgs == termArgs)
      return failure();

    if (hasDuplicates(termArgs))
      return failure();

    auto mapping = getArgsMapping(beforeArgs, termArgs);
    if (!mapping)
      return failure();

    {
      OpBuilder::InsertionGuard g(rewriter);
      rewriter.setInsertionPoint(oldTerm);
      rewriter.replaceOpWithNewOp<ConditionOp>(oldTerm, oldTerm.getCondition(),
                                               beforeArgs);
    }

    auto oldAfter = loop.getAfterBody();

    SmallVector<Type> newResultTypes(beforeArgs.size());
    for (auto &&[i, j] : llvm::enumerate(*mapping))
      newResultTypes[j] = loop.getResult(i).getType();

    auto newLoop = rewriter.create<WhileOp>(
        loop.getLoc(), newResultTypes, loop.getInits(),
        /*beforeBuilder=*/nullptr, /*afterBuilder=*/nullptr);
    auto newBefore = newLoop.getBeforeBody();
    auto newAfter = newLoop.getAfterBody();

    SmallVector<Value> newResults(beforeArgs.size());
    SmallVector<Value> newAfterArgs(beforeArgs.size());
    for (auto &&[i, j] : llvm::enumerate(*mapping)) {
      newResults[i] = newLoop.getResult(j);
      newAfterArgs[i] = newAfter->getArgument(j);
    }

    rewriter.inlineBlockBefore(oldBefore, newBefore, newBefore->begin(),
                               newBefore->getArguments());
    rewriter.inlineBlockBefore(oldAfter, newAfter, newAfter->begin(),
                               newAfterArgs);

    rewriter.replaceOp(loop, newResults);
    return success();
  }
};
} // namespace

void WhileOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                          MLIRContext *context) {
  results.add<RemoveLoopInvariantArgsFromBeforeBlock,
              RemoveLoopInvariantValueYielded, WhileConditionTruth,
              WhileCmpCond, WhileUnusedResult, WhileRemoveDuplicatedResults,
              WhileRemoveUnusedArgs, WhileOpAlignBeforeArgs>(context);
}

//===----------------------------------------------------------------------===//
// IndexSwitchOp
//===----------------------------------------------------------------------===//

/// Parse the case regions and values.
static ParseResult
parseSwitchCases(OpAsmParser &p, DenseI64ArrayAttr &cases,
                 SmallVectorImpl<std::unique_ptr<Region>> &caseRegions) {
  SmallVector<int64_t> caseValues;
  while (succeeded(p.parseOptionalKeyword("case"))) {
    int64_t value;
    Region &region = *caseRegions.emplace_back(std::make_unique<Region>());
    if (p.parseInteger(value) || p.parseRegion(region, /*arguments=*/{}))
      return failure();
    caseValues.push_back(value);
  }
  cases = p.getBuilder().getDenseI64ArrayAttr(caseValues);
  return success();
}

/// Print the case regions and values.
static void printSwitchCases(OpAsmPrinter &p, Operation *op,
                             DenseI64ArrayAttr cases, RegionRange caseRegions) {
  for (auto [value, region] : llvm::zip(cases.asArrayRef(), caseRegions)) {
    p.printNewline();
    p << "case " << value << ' ';
    p.printRegion(*region, /*printEntryBlockArgs=*/false);
  }
}

LogicalResult scf::IndexSwitchOp::verify() {
  if (getCases().size() != getCaseRegions().size()) {
    return emitOpError("has ")
           << getCaseRegions().size() << " case regions but "
           << getCases().size() << " case values";
  }

  DenseSet<int64_t> valueSet;
  for (int64_t value : getCases())
    if (!valueSet.insert(value).second)
      return emitOpError("has duplicate case value: ") << value;
  auto verifyRegion = [&](Region &region, const Twine &name) -> LogicalResult {
    auto yield = dyn_cast<YieldOp>(region.front().back());
    if (!yield)
      return emitOpError("expected region to end with scf.yield, but got ")
             << region.front().back().getName();

    if (yield.getNumOperands() != getNumResults()) {
      return (emitOpError("expected each region to return ")
              << getNumResults() << " values, but " << name << " returns "
              << yield.getNumOperands())
                 .attachNote(yield.getLoc())
             << "see yield operation here";
    }
    for (auto [idx, result, operand] :
         llvm::zip(llvm::seq<unsigned>(0, getNumResults()), getResultTypes(),
                   yield.getOperandTypes())) {
      if (result == operand)
        continue;
      return (emitOpError("expected result #")
              << idx << " of each region to be " << result)
                 .attachNote(yield.getLoc())
             << name << " returns " << operand << " here";
    }
    return success();
  };

  if (failed(verifyRegion(getDefaultRegion(), "default region")))
    return failure();
  for (auto [idx, caseRegion] : llvm::enumerate(getCaseRegions()))
    if (failed(verifyRegion(caseRegion, "case region #" + Twine(idx))))
      return failure();

  return success();
}

unsigned scf::IndexSwitchOp::getNumCases() { return getCases().size(); }

Block &scf::IndexSwitchOp::getDefaultBlock() {
  return getDefaultRegion().front();
}

Block &scf::IndexSwitchOp::getCaseBlock(unsigned idx) {
  assert(idx < getNumCases() && "case index out-of-bounds");
  return getCaseRegions()[idx].front();
}

void IndexSwitchOp::getSuccessorRegions(
    RegionBranchPoint point, SmallVectorImpl<RegionSuccessor> &successors) {
  // All regions branch back to the parent op.
  if (!point.isParent()) {
    successors.emplace_back(getResults());
    return;
  }

  llvm::append_range(successors, getRegions());
}

void IndexSwitchOp::getEntrySuccessorRegions(
    ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &successors) {
  FoldAdaptor adaptor(operands, *this);

  // If a constant was not provided, all regions are possible successors.
  auto arg = dyn_cast_or_null<IntegerAttr>(adaptor.getArg());
  if (!arg) {
    llvm::append_range(successors, getRegions());
    return;
  }

  // Otherwise, try to find a case with a matching value. If not, the
  // default region is the only successor.
  for (auto [caseValue, caseRegion] : llvm::zip(getCases(), getCaseRegions())) {
    if (caseValue == arg.getInt()) {
      successors.emplace_back(&caseRegion);
      return;
    }
  }
  successors.emplace_back(&getDefaultRegion());
}

void IndexSwitchOp::getRegionInvocationBounds(
    ArrayRef<Attribute> operands, SmallVectorImpl<InvocationBounds> &bounds) {
  auto operandValue = llvm::dyn_cast_or_null<IntegerAttr>(operands.front());
  if (!operandValue) {
    // All regions are invoked at most once.
    bounds.append(getNumRegions(), InvocationBounds(/*lb=*/0, /*ub=*/1));
    return;
  }

  unsigned liveIndex = getNumRegions() - 1;
  const auto *it = llvm::find(getCases(), operandValue.getInt());
  if (it != getCases().end())
    liveIndex = std::distance(getCases().begin(), it);
  for (unsigned i = 0, e = getNumRegions(); i < e; ++i)
    bounds.emplace_back(/*lb=*/0, /*ub=*/i == liveIndex);
}

struct FoldConstantCase : OpRewritePattern<scf::IndexSwitchOp> {
  using OpRewritePattern<scf::IndexSwitchOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::IndexSwitchOp op,
                                PatternRewriter &rewriter) const override {
    // If `op.getArg()` is a constant, select the region that matches with
    // the constant value. Use the default region if no matche is found.
    std::optional<int64_t> maybeCst = getConstantIntValue(op.getArg());
    if (!maybeCst.has_value())
      return failure();
    int64_t cst = *maybeCst;
    int64_t caseIdx, e = op.getNumCases();
    for (caseIdx = 0; caseIdx < e; ++caseIdx) {
      if (cst == op.getCases()[caseIdx])
        break;
    }

    Region &r = (caseIdx < op.getNumCases()) ? op.getCaseRegions()[caseIdx]
                                             : op.getDefaultRegion();
    Block &source = r.front();
    Operation *terminator = source.getTerminator();
    SmallVector<Value> results = terminator->getOperands();

    rewriter.inlineBlockBefore(&source, op);
    rewriter.eraseOp(terminator);
    // Replace the operation with a potentially empty list of results.
    // Fold mechanism doesn't support the case where the result list is empty.
    rewriter.replaceOp(op, results);

    return success();
  }
};

void IndexSwitchOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<FoldConstantCase>(context);
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/SCF/IR/SCFOps.cpp.inc"
