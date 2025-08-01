//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Interfaces/Utils/InferIntRangeCommon.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"

using namespace mlir;
using namespace mlir::memref;

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *MemRefDialect::materializeConstant(OpBuilder &builder,
                                              Attribute value, Type type,
                                              Location loc) {
  return arith::ConstantOp::materialize(builder, value, type, loc);
}

//===----------------------------------------------------------------------===//
// Common canonicalization pattern support logic
//===----------------------------------------------------------------------===//

/// This is a common class used for patterns of the form
/// "someop(memrefcast) -> someop".  It folds the source of any memref.cast
/// into the root operation directly.
LogicalResult mlir::memref::foldMemRefCast(Operation *op, Value inner) {
  bool folded = false;
  for (OpOperand &operand : op->getOpOperands()) {
    auto cast = operand.get().getDefiningOp<CastOp>();
    if (cast && operand.get() != inner &&
        !llvm::isa<UnrankedMemRefType>(cast.getOperand().getType())) {
      operand.set(cast.getOperand());
      folded = true;
    }
  }
  return success(folded);
}

/// Return an unranked/ranked tensor type for the given unranked/ranked memref
/// type.
Type mlir::memref::getTensorTypeFromMemRefType(Type type) {
  if (auto memref = llvm::dyn_cast<MemRefType>(type))
    return RankedTensorType::get(memref.getShape(), memref.getElementType());
  if (auto memref = llvm::dyn_cast<UnrankedMemRefType>(type))
    return UnrankedTensorType::get(memref.getElementType());
  return NoneType::get(type.getContext());
}

OpFoldResult memref::getMixedSize(OpBuilder &builder, Location loc, Value value,
                                  int64_t dim) {
  auto memrefType = llvm::cast<MemRefType>(value.getType());
  if (memrefType.isDynamicDim(dim))
    return builder.createOrFold<memref::DimOp>(loc, value, dim);

  return builder.getIndexAttr(memrefType.getDimSize(dim));
}

SmallVector<OpFoldResult> memref::getMixedSizes(OpBuilder &builder,
                                                Location loc, Value value) {
  auto memrefType = llvm::cast<MemRefType>(value.getType());
  SmallVector<OpFoldResult> result;
  for (int64_t i = 0; i < memrefType.getRank(); ++i)
    result.push_back(getMixedSize(builder, loc, value, i));
  return result;
}

//===----------------------------------------------------------------------===//
// Utility functions for propagating static information
//===----------------------------------------------------------------------===//

/// Helper function that sets values[i] to constValues[i] if the latter is a
/// static value, as indicated by ShapedType::kDynamic.
///
/// If constValues[i] is dynamic, tries to extract a constant value from
/// value[i] to allow for additional folding opportunities. Also convertes all
/// existing attributes to index attributes. (They may be i64 attributes.)
static void constifyIndexValues(SmallVectorImpl<OpFoldResult> &values,
                                ArrayRef<int64_t> constValues) {
  assert(constValues.size() == values.size() &&
         "incorrect number of const values");
  for (auto [i, cstVal] : llvm::enumerate(constValues)) {
    Builder builder(values[i].getContext());
    if (ShapedType::isStatic(cstVal)) {
      // Constant value is known, use it directly.
      values[i] = builder.getIndexAttr(cstVal);
      continue;
    }
    if (std::optional<int64_t> cst = getConstantIntValue(values[i])) {
      // Try to extract a constant or convert an existing to index.
      values[i] = builder.getIndexAttr(*cst);
    }
  }
}

//===----------------------------------------------------------------------===//
// AllocOp / AllocaOp
//===----------------------------------------------------------------------===//

void AllocOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "alloc");
}

void AllocaOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "alloca");
}

template <typename AllocLikeOp>
static LogicalResult verifyAllocLikeOp(AllocLikeOp op) {
  static_assert(llvm::is_one_of<AllocLikeOp, AllocOp, AllocaOp>::value,
                "applies to only alloc or alloca");
  auto memRefType = llvm::dyn_cast<MemRefType>(op.getResult().getType());
  if (!memRefType)
    return op.emitOpError("result must be a memref");

  if (op.getDynamicSizes().size() != memRefType.getNumDynamicDims())
    return op.emitOpError("dimension operand count does not equal memref "
                          "dynamic dimension count");

  unsigned numSymbols = 0;
  if (!memRefType.getLayout().isIdentity())
    numSymbols = memRefType.getLayout().getAffineMap().getNumSymbols();
  if (op.getSymbolOperands().size() != numSymbols)
    return op.emitOpError("symbol operand count does not equal memref symbol "
                          "count: expected ")
           << numSymbols << ", got " << op.getSymbolOperands().size();

  return success();
}

LogicalResult AllocOp::verify() { return verifyAllocLikeOp(*this); }

LogicalResult AllocaOp::verify() {
  // An alloca op needs to have an ancestor with an allocation scope trait.
  if (!(*this)->getParentWithTrait<OpTrait::AutomaticAllocationScope>())
    return emitOpError(
        "requires an ancestor op with AutomaticAllocationScope trait");

  return verifyAllocLikeOp(*this);
}

namespace {
/// Fold constant dimensions into an alloc like operation.
template <typename AllocLikeOp>
struct SimplifyAllocConst : public OpRewritePattern<AllocLikeOp> {
  using OpRewritePattern<AllocLikeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AllocLikeOp alloc,
                                PatternRewriter &rewriter) const override {
    // Check to see if any dimensions operands are constants.  If so, we can
    // substitute and drop them.
    if (llvm::none_of(alloc.getDynamicSizes(), [](Value operand) {
          APInt constSizeArg;
          if (!matchPattern(operand, m_ConstantInt(&constSizeArg)))
            return false;
          return constSizeArg.isNonNegative();
        }))
      return failure();

    auto memrefType = alloc.getType();

    // Ok, we have one or more constant operands.  Collect the non-constant ones
    // and keep track of the resultant memref type to build.
    SmallVector<int64_t, 4> newShapeConstants;
    newShapeConstants.reserve(memrefType.getRank());
    SmallVector<Value, 4> dynamicSizes;

    unsigned dynamicDimPos = 0;
    for (unsigned dim = 0, e = memrefType.getRank(); dim < e; ++dim) {
      int64_t dimSize = memrefType.getDimSize(dim);
      // If this is already static dimension, keep it.
      if (ShapedType::isStatic(dimSize)) {
        newShapeConstants.push_back(dimSize);
        continue;
      }
      auto dynamicSize = alloc.getDynamicSizes()[dynamicDimPos];
      APInt constSizeArg;
      if (matchPattern(dynamicSize, m_ConstantInt(&constSizeArg)) &&
          constSizeArg.isNonNegative()) {
        // Dynamic shape dimension will be folded.
        newShapeConstants.push_back(constSizeArg.getZExtValue());
      } else {
        // Dynamic shape dimension not folded; copy dynamicSize from old memref.
        newShapeConstants.push_back(ShapedType::kDynamic);
        dynamicSizes.push_back(dynamicSize);
      }
      dynamicDimPos++;
    }

    // Create new memref type (which will have fewer dynamic dimensions).
    MemRefType newMemRefType =
        MemRefType::Builder(memrefType).setShape(newShapeConstants);
    assert(dynamicSizes.size() == newMemRefType.getNumDynamicDims());

    // Create and insert the alloc op for the new memref.
    auto newAlloc = rewriter.create<AllocLikeOp>(
        alloc.getLoc(), newMemRefType, dynamicSizes, alloc.getSymbolOperands(),
        alloc.getAlignmentAttr());
    // Insert a cast so we have the same type as the old alloc.
    rewriter.replaceOpWithNewOp<CastOp>(alloc, alloc.getType(), newAlloc);
    return success();
  }
};

/// Fold alloc operations with no users or only store and dealloc uses.
template <typename T>
struct SimplifyDeadAlloc : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T alloc,
                                PatternRewriter &rewriter) const override {
    if (llvm::any_of(alloc->getUsers(), [&](Operation *op) {
          if (auto storeOp = dyn_cast<StoreOp>(op))
            return storeOp.getValue() == alloc;
          return !isa<DeallocOp>(op);
        }))
      return failure();

    for (Operation *user : llvm::make_early_inc_range(alloc->getUsers()))
      rewriter.eraseOp(user);

    rewriter.eraseOp(alloc);
    return success();
  }
};
} // namespace

void AllocOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                          MLIRContext *context) {
  results.add<SimplifyAllocConst<AllocOp>, SimplifyDeadAlloc<AllocOp>>(context);
}

void AllocaOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                           MLIRContext *context) {
  results.add<SimplifyAllocConst<AllocaOp>, SimplifyDeadAlloc<AllocaOp>>(
      context);
}

//===----------------------------------------------------------------------===//
// ReallocOp
//===----------------------------------------------------------------------===//

LogicalResult ReallocOp::verify() {
  auto sourceType = llvm::cast<MemRefType>(getOperand(0).getType());
  MemRefType resultType = getType();

  // The source memref should have identity layout (or none).
  if (!sourceType.getLayout().isIdentity())
    return emitError("unsupported layout for source memref type ")
           << sourceType;

  // The result memref should have identity layout (or none).
  if (!resultType.getLayout().isIdentity())
    return emitError("unsupported layout for result memref type ")
           << resultType;

  // The source memref and the result memref should be in the same memory space.
  if (sourceType.getMemorySpace() != resultType.getMemorySpace())
    return emitError("different memory spaces specified for source memref "
                     "type ")
           << sourceType << " and result memref type " << resultType;

  // The source memref and the result memref should have the same element type.
  if (sourceType.getElementType() != resultType.getElementType())
    return emitError("different element types specified for source memref "
                     "type ")
           << sourceType << " and result memref type " << resultType;

  // Verify that we have the dynamic dimension operand when it is needed.
  if (resultType.getNumDynamicDims() && !getDynamicResultSize())
    return emitError("missing dimension operand for result type ")
           << resultType;
  if (!resultType.getNumDynamicDims() && getDynamicResultSize())
    return emitError("unnecessary dimension operand for result type ")
           << resultType;

  return success();
}

void ReallocOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<SimplifyDeadAlloc<ReallocOp>>(context);
}

//===----------------------------------------------------------------------===//
// AllocaScopeOp
//===----------------------------------------------------------------------===//

void AllocaScopeOp::print(OpAsmPrinter &p) {
  bool printBlockTerminators = false;

  p << ' ';
  if (!getResults().empty()) {
    p << " -> (" << getResultTypes() << ")";
    printBlockTerminators = true;
  }
  p << ' ';
  p.printRegion(getBodyRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/printBlockTerminators);
  p.printOptionalAttrDict((*this)->getAttrs());
}

ParseResult AllocaScopeOp::parse(OpAsmParser &parser, OperationState &result) {
  // Create a region for the body.
  result.regions.reserve(1);
  Region *bodyRegion = result.addRegion();

  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Parse the body region.
  if (parser.parseRegion(*bodyRegion, /*arguments=*/{}))
    return failure();
  AllocaScopeOp::ensureTerminator(*bodyRegion, parser.getBuilder(),
                                  result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

void AllocaScopeOp::getSuccessorRegions(
    RegionBranchPoint point, SmallVectorImpl<RegionSuccessor> &regions) {
  if (!point.isParent()) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  regions.push_back(RegionSuccessor(&getBodyRegion()));
}

/// Given an operation, return whether this op is guaranteed to
/// allocate an AutomaticAllocationScopeResource
static bool isGuaranteedAutomaticAllocation(Operation *op) {
  MemoryEffectOpInterface interface = dyn_cast<MemoryEffectOpInterface>(op);
  if (!interface)
    return false;
  for (auto res : op->getResults()) {
    if (auto effect =
            interface.getEffectOnValue<MemoryEffects::Allocate>(res)) {
      if (isa<SideEffects::AutomaticAllocationScopeResource>(
              effect->getResource()))
        return true;
    }
  }
  return false;
}

/// Given an operation, return whether this op itself could
/// allocate an AutomaticAllocationScopeResource. Note that
/// this will not check whether an operation contained within
/// the op can allocate.
static bool isOpItselfPotentialAutomaticAllocation(Operation *op) {
  // This op itself doesn't create a stack allocation,
  // the inner allocation should be handled separately.
  if (op->hasTrait<OpTrait::HasRecursiveMemoryEffects>())
    return false;
  MemoryEffectOpInterface interface = dyn_cast<MemoryEffectOpInterface>(op);
  if (!interface)
    return true;
  for (auto res : op->getResults()) {
    if (auto effect =
            interface.getEffectOnValue<MemoryEffects::Allocate>(res)) {
      if (isa<SideEffects::AutomaticAllocationScopeResource>(
              effect->getResource()))
        return true;
    }
  }
  return false;
}

/// Return whether this op is the last non terminating op
/// in a region. That is to say, it is in a one-block region
/// and is only followed by a terminator. This prevents
/// extending the lifetime of allocations.
static bool lastNonTerminatorInRegion(Operation *op) {
  return op->getBlock()->mightHaveTerminator() &&
         op->getNextNode() == op->getBlock()->getTerminator() &&
         op->getParentRegion()->hasOneBlock();
}

/// Inline an AllocaScopeOp if either the direct parent is an allocation scope
/// or it contains no allocation.
struct AllocaScopeInliner : public OpRewritePattern<AllocaScopeOp> {
  using OpRewritePattern<AllocaScopeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AllocaScopeOp op,
                                PatternRewriter &rewriter) const override {
    bool hasPotentialAlloca =
        op->walk<WalkOrder::PreOrder>([&](Operation *alloc) {
            if (alloc == op)
              return WalkResult::advance();
            if (isOpItselfPotentialAutomaticAllocation(alloc))
              return WalkResult::interrupt();
            if (alloc->hasTrait<OpTrait::AutomaticAllocationScope>())
              return WalkResult::skip();
            return WalkResult::advance();
          }).wasInterrupted();

    // If this contains no potential allocation, it is always legal to
    // inline. Otherwise, consider two conditions:
    if (hasPotentialAlloca) {
      // If the parent isn't an allocation scope, or we are not the last
      // non-terminator op in the parent, we will extend the lifetime.
      if (!op->getParentOp()->hasTrait<OpTrait::AutomaticAllocationScope>())
        return failure();
      if (!lastNonTerminatorInRegion(op))
        return failure();
    }

    Block *block = &op.getRegion().front();
    Operation *terminator = block->getTerminator();
    ValueRange results = terminator->getOperands();
    rewriter.inlineBlockBefore(block, op);
    rewriter.replaceOp(op, results);
    rewriter.eraseOp(terminator);
    return success();
  }
};

/// Move allocations into an allocation scope, if it is legal to
/// move them (e.g. their operands are available at the location
/// the op would be moved to).
struct AllocaScopeHoister : public OpRewritePattern<AllocaScopeOp> {
  using OpRewritePattern<AllocaScopeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AllocaScopeOp op,
                                PatternRewriter &rewriter) const override {

    if (!op->getParentWithTrait<OpTrait::AutomaticAllocationScope>())
      return failure();

    Operation *lastParentWithoutScope = op->getParentOp();

    if (!lastParentWithoutScope ||
        lastParentWithoutScope->hasTrait<OpTrait::AutomaticAllocationScope>())
      return failure();

    // Only apply to if this is this last non-terminator
    // op in the block (lest lifetime be extended) of a one
    // block region
    if (!lastNonTerminatorInRegion(op) ||
        !lastNonTerminatorInRegion(lastParentWithoutScope))
      return failure();

    while (!lastParentWithoutScope->getParentOp()
                ->hasTrait<OpTrait::AutomaticAllocationScope>()) {
      lastParentWithoutScope = lastParentWithoutScope->getParentOp();
      if (!lastParentWithoutScope ||
          !lastNonTerminatorInRegion(lastParentWithoutScope))
        return failure();
    }
    assert(lastParentWithoutScope->getParentOp()
               ->hasTrait<OpTrait::AutomaticAllocationScope>());

    Region *containingRegion = nullptr;
    for (auto &r : lastParentWithoutScope->getRegions()) {
      if (r.isAncestor(op->getParentRegion())) {
        assert(containingRegion == nullptr &&
               "only one region can contain the op");
        containingRegion = &r;
      }
    }
    assert(containingRegion && "op must be contained in a region");

    SmallVector<Operation *> toHoist;
    op->walk([&](Operation *alloc) {
      if (!isGuaranteedAutomaticAllocation(alloc))
        return WalkResult::skip();

      // If any operand is not defined before the location of
      // lastParentWithoutScope (i.e. where we would hoist to), skip.
      if (llvm::any_of(alloc->getOperands(), [&](Value v) {
            return containingRegion->isAncestor(v.getParentRegion());
          }))
        return WalkResult::skip();
      toHoist.push_back(alloc);
      return WalkResult::advance();
    });

    if (toHoist.empty())
      return failure();
    rewriter.setInsertionPoint(lastParentWithoutScope);
    for (auto *op : toHoist) {
      auto *cloned = rewriter.clone(*op);
      rewriter.replaceOp(op, cloned->getResults());
    }
    return success();
  }
};

void AllocaScopeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<AllocaScopeInliner, AllocaScopeHoister>(context);
}

//===----------------------------------------------------------------------===//
// AssumeAlignmentOp
//===----------------------------------------------------------------------===//

LogicalResult AssumeAlignmentOp::verify() {
  if (!llvm::isPowerOf2_32(getAlignment()))
    return emitOpError("alignment must be power of 2");
  return success();
}

void AssumeAlignmentOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "assume_align");
}

OpFoldResult AssumeAlignmentOp::fold(FoldAdaptor adaptor) {
  auto source = getMemref().getDefiningOp<AssumeAlignmentOp>();
  if (!source)
    return {};
  if (source.getAlignment() != getAlignment())
    return {};
  return getMemref();
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

void CastOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "cast");
}

/// Determines whether MemRef_CastOp casts to a more dynamic version of the
/// source memref. This is useful to fold a memref.cast into a consuming op
/// and implement canonicalization patterns for ops in different dialects that
/// may consume the results of memref.cast operations. Such foldable memref.cast
/// operations are typically inserted as `view` and `subview` ops are
/// canonicalized, to preserve the type compatibility of their uses.
///
/// Returns true when all conditions are met:
/// 1. source and result are ranked memrefs with strided semantics and same
/// element type and rank.
/// 2. each of the source's size, offset or stride has more static information
/// than the corresponding result's size, offset or stride.
///
/// Example 1:
/// ```mlir
///   %1 = memref.cast %0 : memref<8x16xf32> to memref<?x?xf32>
///   %2 = consumer %1 ... : memref<?x?xf32> ...
/// ```
///
/// may fold into:
///
/// ```mlir
///   %2 = consumer %0 ... : memref<8x16xf32> ...
/// ```
///
/// Example 2:
/// ```
///   %1 = memref.cast %0 : memref<?x16xf32, affine_map<(i, j)->(16 * i + j)>>
///          to memref<?x?xf32>
///   consumer %1 : memref<?x?xf32> ...
/// ```
///
/// may fold into:
///
/// ```
///   consumer %0 ... : memref<?x16xf32, affine_map<(i, j)->(16 * i + j)>>
/// ```
bool CastOp::canFoldIntoConsumerOp(CastOp castOp) {
  MemRefType sourceType =
      llvm::dyn_cast<MemRefType>(castOp.getSource().getType());
  MemRefType resultType = llvm::dyn_cast<MemRefType>(castOp.getType());

  // Requires ranked MemRefType.
  if (!sourceType || !resultType)
    return false;

  // Requires same elemental type.
  if (sourceType.getElementType() != resultType.getElementType())
    return false;

  // Requires same rank.
  if (sourceType.getRank() != resultType.getRank())
    return false;

  // Only fold casts between strided memref forms.
  int64_t sourceOffset, resultOffset;
  SmallVector<int64_t, 4> sourceStrides, resultStrides;
  if (failed(sourceType.getStridesAndOffset(sourceStrides, sourceOffset)) ||
      failed(resultType.getStridesAndOffset(resultStrides, resultOffset)))
    return false;

  // If cast is towards more static sizes along any dimension, don't fold.
  for (auto it : llvm::zip(sourceType.getShape(), resultType.getShape())) {
    auto ss = std::get<0>(it), st = std::get<1>(it);
    if (ss != st)
      if (ShapedType::isDynamic(ss) && ShapedType::isStatic(st))
        return false;
  }

  // If cast is towards more static offset along any dimension, don't fold.
  if (sourceOffset != resultOffset)
    if (ShapedType::isDynamic(sourceOffset) &&
        ShapedType::isStatic(resultOffset))
      return false;

  // If cast is towards more static strides along any dimension, don't fold.
  for (auto it : llvm::zip(sourceStrides, resultStrides)) {
    auto ss = std::get<0>(it), st = std::get<1>(it);
    if (ss != st)
      if (ShapedType::isDynamic(ss) && ShapedType::isStatic(st))
        return false;
  }

  return true;
}

bool CastOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  Type a = inputs.front(), b = outputs.front();
  auto aT = llvm::dyn_cast<MemRefType>(a);
  auto bT = llvm::dyn_cast<MemRefType>(b);

  auto uaT = llvm::dyn_cast<UnrankedMemRefType>(a);
  auto ubT = llvm::dyn_cast<UnrankedMemRefType>(b);

  if (aT && bT) {
    if (aT.getElementType() != bT.getElementType())
      return false;
    if (aT.getLayout() != bT.getLayout()) {
      int64_t aOffset, bOffset;
      SmallVector<int64_t, 4> aStrides, bStrides;
      if (failed(aT.getStridesAndOffset(aStrides, aOffset)) ||
          failed(bT.getStridesAndOffset(bStrides, bOffset)) ||
          aStrides.size() != bStrides.size())
        return false;

      // Strides along a dimension/offset are compatible if the value in the
      // source memref is static and the value in the target memref is the
      // same. They are also compatible if either one is dynamic (see
      // description of MemRefCastOp for details).
      auto checkCompatible = [](int64_t a, int64_t b) {
        return (ShapedType::isDynamic(a) || ShapedType::isDynamic(b) || a == b);
      };
      if (!checkCompatible(aOffset, bOffset))
        return false;
      for (const auto &aStride : enumerate(aStrides))
        if (!checkCompatible(aStride.value(), bStrides[aStride.index()]))
          return false;
    }
    if (aT.getMemorySpace() != bT.getMemorySpace())
      return false;

    // They must have the same rank, and any specified dimensions must match.
    if (aT.getRank() != bT.getRank())
      return false;

    for (unsigned i = 0, e = aT.getRank(); i != e; ++i) {
      int64_t aDim = aT.getDimSize(i), bDim = bT.getDimSize(i);
      if (ShapedType::isStatic(aDim) && ShapedType::isStatic(bDim) &&
          aDim != bDim)
        return false;
    }
    return true;
  } else {
    if (!aT && !uaT)
      return false;
    if (!bT && !ubT)
      return false;
    // Unranked to unranked casting is unsupported
    if (uaT && ubT)
      return false;

    auto aEltType = (aT) ? aT.getElementType() : uaT.getElementType();
    auto bEltType = (bT) ? bT.getElementType() : ubT.getElementType();
    if (aEltType != bEltType)
      return false;

    auto aMemSpace = (aT) ? aT.getMemorySpace() : uaT.getMemorySpace();
    auto bMemSpace = (bT) ? bT.getMemorySpace() : ubT.getMemorySpace();
    return aMemSpace == bMemSpace;
  }

  return false;
}

OpFoldResult CastOp::fold(FoldAdaptor adaptor) {
  return succeeded(foldMemRefCast(*this)) ? getResult() : Value();
}

//===----------------------------------------------------------------------===//
// CopyOp
//===----------------------------------------------------------------------===//

namespace {

/// Fold memref.copy(%x, %x).
struct FoldSelfCopy : public OpRewritePattern<CopyOp> {
  using OpRewritePattern<CopyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CopyOp copyOp,
                                PatternRewriter &rewriter) const override {
    if (copyOp.getSource() != copyOp.getTarget())
      return failure();

    rewriter.eraseOp(copyOp);
    return success();
  }
};

struct FoldEmptyCopy final : public OpRewritePattern<CopyOp> {
  using OpRewritePattern<CopyOp>::OpRewritePattern;

  static bool isEmptyMemRef(BaseMemRefType type) {
    return type.hasRank() && llvm::is_contained(type.getShape(), 0);
  }

  LogicalResult matchAndRewrite(CopyOp copyOp,
                                PatternRewriter &rewriter) const override {
    if (isEmptyMemRef(copyOp.getSource().getType()) ||
        isEmptyMemRef(copyOp.getTarget().getType())) {
      rewriter.eraseOp(copyOp);
      return success();
    }

    return failure();
  }
};
} // namespace

void CopyOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                         MLIRContext *context) {
  results.add<FoldEmptyCopy, FoldSelfCopy>(context);
}

/// If the source/target of a CopyOp is a CastOp that does not modify the shape
/// and element type, the cast can be skipped. Such CastOps only cast the layout
/// of the type.
static LogicalResult FoldCopyOfCast(CopyOp op) {
  for (OpOperand &operand : op->getOpOperands()) {
    auto castOp = operand.get().getDefiningOp<memref::CastOp>();
    if (castOp && memref::CastOp::canFoldIntoConsumerOp(castOp)) {
      operand.set(castOp.getOperand());
      return success();
    }
  }
  return failure();
}

LogicalResult CopyOp::fold(FoldAdaptor adaptor,
                           SmallVectorImpl<OpFoldResult> &results) {

  /// copy(memrefcast) -> copy
  return FoldCopyOfCast(*this);
}

//===----------------------------------------------------------------------===//
// DeallocOp
//===----------------------------------------------------------------------===//

LogicalResult DeallocOp::fold(FoldAdaptor adaptor,
                              SmallVectorImpl<OpFoldResult> &results) {
  /// dealloc(memrefcast) -> dealloc
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// DimOp
//===----------------------------------------------------------------------===//

void DimOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "dim");
}

void DimOp::build(OpBuilder &builder, OperationState &result, Value source,
                  int64_t index) {
  auto loc = result.location;
  Value indexValue = builder.create<arith::ConstantIndexOp>(loc, index);
  build(builder, result, source, indexValue);
}

std::optional<int64_t> DimOp::getConstantIndex() {
  return getConstantIntValue(getIndex());
}

Speculation::Speculatability DimOp::getSpeculatability() {
  auto constantIndex = getConstantIndex();
  if (!constantIndex)
    return Speculation::NotSpeculatable;

  auto rankedSourceType = dyn_cast<MemRefType>(getSource().getType());
  if (!rankedSourceType)
    return Speculation::NotSpeculatable;

  if (rankedSourceType.getRank() <= constantIndex)
    return Speculation::NotSpeculatable;

  return Speculation::Speculatable;
}

void DimOp::inferResultRangesFromOptional(ArrayRef<IntegerValueRange> argRanges,
                                          SetIntLatticeFn setResultRange) {
  setResultRange(getResult(),
                 intrange::inferShapedDimOpInterface(*this, argRanges[1]));
}

/// Return a map with key being elements in `vals` and data being number of
/// occurences of it. Use std::map, since the `vals` here are strides and the
/// dynamic stride value is the same as the tombstone value for
/// `DenseMap<int64_t>`.
static std::map<int64_t, unsigned> getNumOccurences(ArrayRef<int64_t> vals) {
  std::map<int64_t, unsigned> numOccurences;
  for (auto val : vals)
    numOccurences[val]++;
  return numOccurences;
}

/// Given the `originalType` and a `candidateReducedType` whose shape is assumed
/// to be a subset of `originalType` with some `1` entries erased, return the
/// set of indices that specifies which of the entries of `originalShape` are
/// dropped to obtain `reducedShape`.
/// This accounts for cases where there are multiple unit-dims, but only a
/// subset of those are dropped. For MemRefTypes these can be disambiguated
/// using the strides. If a dimension is dropped the stride must be dropped too.
static FailureOr<llvm::SmallBitVector>
computeMemRefRankReductionMask(MemRefType originalType, MemRefType reducedType,
                               ArrayRef<OpFoldResult> sizes) {
  llvm::SmallBitVector unusedDims(originalType.getRank());
  if (originalType.getRank() == reducedType.getRank())
    return unusedDims;

  for (const auto &dim : llvm::enumerate(sizes))
    if (auto attr = llvm::dyn_cast_if_present<Attribute>(dim.value()))
      if (llvm::cast<IntegerAttr>(attr).getInt() == 1)
        unusedDims.set(dim.index());

  // Early exit for the case where the number of unused dims matches the number
  // of ranks reduced.
  if (static_cast<int64_t>(unusedDims.count()) + reducedType.getRank() ==
      originalType.getRank())
    return unusedDims;

  SmallVector<int64_t> originalStrides, candidateStrides;
  int64_t originalOffset, candidateOffset;
  if (failed(
          originalType.getStridesAndOffset(originalStrides, originalOffset)) ||
      failed(
          reducedType.getStridesAndOffset(candidateStrides, candidateOffset)))
    return failure();

  // For memrefs, a dimension is truly dropped if its corresponding stride is
  // also dropped. This is particularly important when more than one of the dims
  // is 1. Track the number of occurences of the strides in the original type
  // and the candidate type. For each unused dim that stride should not be
  // present in the candidate type. Note that there could be multiple dimensions
  // that have the same size. We dont need to exactly figure out which dim
  // corresponds to which stride, we just need to verify that the number of
  // reptitions of a stride in the original + number of unused dims with that
  // stride == number of repititions of a stride in the candidate.
  std::map<int64_t, unsigned> currUnaccountedStrides =
      getNumOccurences(originalStrides);
  std::map<int64_t, unsigned> candidateStridesNumOccurences =
      getNumOccurences(candidateStrides);
  for (size_t dim = 0, e = unusedDims.size(); dim != e; ++dim) {
    if (!unusedDims.test(dim))
      continue;
    int64_t originalStride = originalStrides[dim];
    if (currUnaccountedStrides[originalStride] >
        candidateStridesNumOccurences[originalStride]) {
      // This dim can be treated as dropped.
      currUnaccountedStrides[originalStride]--;
      continue;
    }
    if (currUnaccountedStrides[originalStride] ==
        candidateStridesNumOccurences[originalStride]) {
      // The stride for this is not dropped. Keep as is.
      unusedDims.reset(dim);
      continue;
    }
    if (currUnaccountedStrides[originalStride] <
        candidateStridesNumOccurences[originalStride]) {
      // This should never happen. Cant have a stride in the reduced rank type
      // that wasnt in the original one.
      return failure();
    }
  }

  if ((int64_t)unusedDims.count() + reducedType.getRank() !=
      originalType.getRank())
    return failure();
  return unusedDims;
}

llvm::SmallBitVector SubViewOp::getDroppedDims() {
  MemRefType sourceType = getSourceType();
  MemRefType resultType = getType();
  FailureOr<llvm::SmallBitVector> unusedDims =
      computeMemRefRankReductionMask(sourceType, resultType, getMixedSizes());
  assert(succeeded(unusedDims) && "unable to find unused dims of subview");
  return *unusedDims;
}

OpFoldResult DimOp::fold(FoldAdaptor adaptor) {
  // All forms of folding require a known index.
  auto index = llvm::dyn_cast_if_present<IntegerAttr>(adaptor.getIndex());
  if (!index)
    return {};

  // Folding for unranked types (UnrankedMemRefType) is not supported.
  auto memrefType = llvm::dyn_cast<MemRefType>(getSource().getType());
  if (!memrefType)
    return {};

  // Out of bound indices produce undefined behavior but are still valid IR.
  // Don't choke on them.
  int64_t indexVal = index.getInt();
  if (indexVal < 0 || indexVal >= memrefType.getRank())
    return {};

  // Fold if the shape extent along the given index is known.
  if (!memrefType.isDynamicDim(index.getInt())) {
    Builder builder(getContext());
    return builder.getIndexAttr(memrefType.getShape()[index.getInt()]);
  }

  // The size at the given index is now known to be a dynamic size.
  unsigned unsignedIndex = index.getValue().getZExtValue();

  // Fold dim to the size argument for an `AllocOp`, `ViewOp`, or `SubViewOp`.
  Operation *definingOp = getSource().getDefiningOp();

  if (auto alloc = dyn_cast_or_null<AllocOp>(definingOp))
    return *(alloc.getDynamicSizes().begin() +
             memrefType.getDynamicDimIndex(unsignedIndex));

  if (auto alloca = dyn_cast_or_null<AllocaOp>(definingOp))
    return *(alloca.getDynamicSizes().begin() +
             memrefType.getDynamicDimIndex(unsignedIndex));

  if (auto view = dyn_cast_or_null<ViewOp>(definingOp))
    return *(view.getDynamicSizes().begin() +
             memrefType.getDynamicDimIndex(unsignedIndex));

  if (auto subview = dyn_cast_or_null<SubViewOp>(definingOp)) {
    llvm::SmallBitVector unusedDims = subview.getDroppedDims();
    unsigned resultIndex = 0;
    unsigned sourceRank = subview.getSourceType().getRank();
    unsigned sourceIndex = 0;
    for (auto i : llvm::seq<unsigned>(0, sourceRank)) {
      if (unusedDims.test(i))
        continue;
      if (resultIndex == unsignedIndex) {
        sourceIndex = i;
        break;
      }
      resultIndex++;
    }
    assert(subview.isDynamicSize(sourceIndex) &&
           "expected dynamic subview size");
    return subview.getDynamicSize(sourceIndex);
  }

  if (auto sizeInterface =
          dyn_cast_or_null<OffsetSizeAndStrideOpInterface>(definingOp)) {
    assert(sizeInterface.isDynamicSize(unsignedIndex) &&
           "Expected dynamic subview size");
    return sizeInterface.getDynamicSize(unsignedIndex);
  }

  // dim(memrefcast) -> dim
  if (succeeded(foldMemRefCast(*this)))
    return getResult();

  return {};
}

namespace {
/// Fold dim of a memref reshape operation to a load into the reshape's shape
/// operand.
struct DimOfMemRefReshape : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DimOp dim,
                                PatternRewriter &rewriter) const override {
    auto reshape = dim.getSource().getDefiningOp<ReshapeOp>();

    if (!reshape)
      return rewriter.notifyMatchFailure(
          dim, "Dim op is not defined by a reshape op.");

    // dim of a memref reshape can be folded if dim.getIndex() dominates the
    // reshape. Instead of using `DominanceInfo` (which is usually costly) we
    // cheaply check that either of the following conditions hold:
    //      1. dim.getIndex() is defined in the same block as reshape but before
    //      reshape.
    //      2. dim.getIndex() is defined in a parent block of
    //      reshape.

    // Check condition 1
    if (dim.getIndex().getParentBlock() == reshape->getBlock()) {
      if (auto *definingOp = dim.getIndex().getDefiningOp()) {
        if (reshape->isBeforeInBlock(definingOp)) {
          return rewriter.notifyMatchFailure(
              dim,
              "dim.getIndex is not defined before reshape in the same block.");
        }
      } // else dim.getIndex is a block argument to reshape->getBlock and
        // dominates reshape
    } // Check condition 2
    else if (dim->getBlock() != reshape->getBlock() &&
             !dim.getIndex().getParentRegion()->isProperAncestor(
                 reshape->getParentRegion())) {
      // If dim and reshape are in the same block but dim.getIndex() isn't, we
      // already know dim.getIndex() dominates reshape without calling
      // `isProperAncestor`
      return rewriter.notifyMatchFailure(
          dim, "dim.getIndex does not dominate reshape.");
    }

    // Place the load directly after the reshape to ensure that the shape memref
    // was not mutated.
    rewriter.setInsertionPointAfter(reshape);
    Location loc = dim.getLoc();
    Value load =
        rewriter.create<LoadOp>(loc, reshape.getShape(), dim.getIndex());
    if (load.getType() != dim.getType())
      load = rewriter.create<arith::IndexCastOp>(loc, dim.getType(), load);
    rewriter.replaceOp(dim, load);
    return success();
  }
};

} // namespace

void DimOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                        MLIRContext *context) {
  results.add<DimOfMemRefReshape>(context);
}

// ---------------------------------------------------------------------------
// DmaStartOp
// ---------------------------------------------------------------------------

void DmaStartOp::build(OpBuilder &builder, OperationState &result,
                       Value srcMemRef, ValueRange srcIndices, Value destMemRef,
                       ValueRange destIndices, Value numElements,
                       Value tagMemRef, ValueRange tagIndices, Value stride,
                       Value elementsPerStride) {
  result.addOperands(srcMemRef);
  result.addOperands(srcIndices);
  result.addOperands(destMemRef);
  result.addOperands(destIndices);
  result.addOperands({numElements, tagMemRef});
  result.addOperands(tagIndices);
  if (stride)
    result.addOperands({stride, elementsPerStride});
}

void DmaStartOp::print(OpAsmPrinter &p) {
  p << " " << getSrcMemRef() << '[' << getSrcIndices() << "], "
    << getDstMemRef() << '[' << getDstIndices() << "], " << getNumElements()
    << ", " << getTagMemRef() << '[' << getTagIndices() << ']';
  if (isStrided())
    p << ", " << getStride() << ", " << getNumElementsPerStride();

  p.printOptionalAttrDict((*this)->getAttrs());
  p << " : " << getSrcMemRef().getType() << ", " << getDstMemRef().getType()
    << ", " << getTagMemRef().getType();
}

// Parse DmaStartOp.
// Ex:
//   %dma_id = dma_start %src[%i, %j], %dst[%k, %l], %size,
//                       %tag[%index], %stride, %num_elt_per_stride :
//                     : memref<3076 x f32, 0>,
//                       memref<1024 x f32, 2>,
//                       memref<1 x i32>
//
ParseResult DmaStartOp::parse(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::UnresolvedOperand srcMemRefInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> srcIndexInfos;
  OpAsmParser::UnresolvedOperand dstMemRefInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> dstIndexInfos;
  OpAsmParser::UnresolvedOperand numElementsInfo;
  OpAsmParser::UnresolvedOperand tagMemrefInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> tagIndexInfos;
  SmallVector<OpAsmParser::UnresolvedOperand, 2> strideInfo;

  SmallVector<Type, 3> types;
  auto indexType = parser.getBuilder().getIndexType();

  // Parse and resolve the following list of operands:
  // *) source memref followed by its indices (in square brackets).
  // *) destination memref followed by its indices (in square brackets).
  // *) dma size in KiB.
  if (parser.parseOperand(srcMemRefInfo) ||
      parser.parseOperandList(srcIndexInfos, OpAsmParser::Delimiter::Square) ||
      parser.parseComma() || parser.parseOperand(dstMemRefInfo) ||
      parser.parseOperandList(dstIndexInfos, OpAsmParser::Delimiter::Square) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo) ||
      parser.parseComma() || parser.parseOperand(tagMemrefInfo) ||
      parser.parseOperandList(tagIndexInfos, OpAsmParser::Delimiter::Square))
    return failure();

  // Parse optional stride and elements per stride.
  if (parser.parseTrailingOperandList(strideInfo))
    return failure();

  bool isStrided = strideInfo.size() == 2;
  if (!strideInfo.empty() && !isStrided) {
    return parser.emitError(parser.getNameLoc(),
                            "expected two stride related operands");
  }

  if (parser.parseColonTypeList(types))
    return failure();
  if (types.size() != 3)
    return parser.emitError(parser.getNameLoc(), "fewer/more types expected");

  if (parser.resolveOperand(srcMemRefInfo, types[0], result.operands) ||
      parser.resolveOperands(srcIndexInfos, indexType, result.operands) ||
      parser.resolveOperand(dstMemRefInfo, types[1], result.operands) ||
      parser.resolveOperands(dstIndexInfos, indexType, result.operands) ||
      // size should be an index.
      parser.resolveOperand(numElementsInfo, indexType, result.operands) ||
      parser.resolveOperand(tagMemrefInfo, types[2], result.operands) ||
      // tag indices should be index.
      parser.resolveOperands(tagIndexInfos, indexType, result.operands))
    return failure();

  if (isStrided) {
    if (parser.resolveOperands(strideInfo, indexType, result.operands))
      return failure();
  }

  return success();
}

LogicalResult DmaStartOp::verify() {
  unsigned numOperands = getNumOperands();

  // Mandatory non-variadic operands are: src memref, dst memref, tag memref and
  // the number of elements.
  if (numOperands < 4)
    return emitOpError("expected at least 4 operands");

  // Check types of operands. The order of these calls is important: the later
  // calls rely on some type properties to compute the operand position.
  // 1. Source memref.
  if (!llvm::isa<MemRefType>(getSrcMemRef().getType()))
    return emitOpError("expected source to be of memref type");
  if (numOperands < getSrcMemRefRank() + 4)
    return emitOpError() << "expected at least " << getSrcMemRefRank() + 4
                         << " operands";
  if (!getSrcIndices().empty() &&
      !llvm::all_of(getSrcIndices().getTypes(),
                    [](Type t) { return t.isIndex(); }))
    return emitOpError("expected source indices to be of index type");

  // 2. Destination memref.
  if (!llvm::isa<MemRefType>(getDstMemRef().getType()))
    return emitOpError("expected destination to be of memref type");
  unsigned numExpectedOperands = getSrcMemRefRank() + getDstMemRefRank() + 4;
  if (numOperands < numExpectedOperands)
    return emitOpError() << "expected at least " << numExpectedOperands
                         << " operands";
  if (!getDstIndices().empty() &&
      !llvm::all_of(getDstIndices().getTypes(),
                    [](Type t) { return t.isIndex(); }))
    return emitOpError("expected destination indices to be of index type");

  // 3. Number of elements.
  if (!getNumElements().getType().isIndex())
    return emitOpError("expected num elements to be of index type");

  // 4. Tag memref.
  if (!llvm::isa<MemRefType>(getTagMemRef().getType()))
    return emitOpError("expected tag to be of memref type");
  numExpectedOperands += getTagMemRefRank();
  if (numOperands < numExpectedOperands)
    return emitOpError() << "expected at least " << numExpectedOperands
                         << " operands";
  if (!getTagIndices().empty() &&
      !llvm::all_of(getTagIndices().getTypes(),
                    [](Type t) { return t.isIndex(); }))
    return emitOpError("expected tag indices to be of index type");

  // Optional stride-related operands must be either both present or both
  // absent.
  if (numOperands != numExpectedOperands &&
      numOperands != numExpectedOperands + 2)
    return emitOpError("incorrect number of operands");

  // 5. Strides.
  if (isStrided()) {
    if (!getStride().getType().isIndex() ||
        !getNumElementsPerStride().getType().isIndex())
      return emitOpError(
          "expected stride and num elements per stride to be of type index");
  }

  return success();
}

LogicalResult DmaStartOp::fold(FoldAdaptor adaptor,
                               SmallVectorImpl<OpFoldResult> &results) {
  /// dma_start(memrefcast) -> dma_start
  return foldMemRefCast(*this);
}

// ---------------------------------------------------------------------------
// DmaWaitOp
// ---------------------------------------------------------------------------

LogicalResult DmaWaitOp::fold(FoldAdaptor adaptor,
                              SmallVectorImpl<OpFoldResult> &results) {
  /// dma_wait(memrefcast) -> dma_wait
  return foldMemRefCast(*this);
}

LogicalResult DmaWaitOp::verify() {
  // Check that the number of tag indices matches the tagMemRef rank.
  unsigned numTagIndices = getTagIndices().size();
  unsigned tagMemRefRank = getTagMemRefRank();
  if (numTagIndices != tagMemRefRank)
    return emitOpError() << "expected tagIndices to have the same number of "
                            "elements as the tagMemRef rank, expected "
                         << tagMemRefRank << ", but got " << numTagIndices;
  return success();
}

//===----------------------------------------------------------------------===//
// ExtractAlignedPointerAsIndexOp
//===----------------------------------------------------------------------===//

void ExtractAlignedPointerAsIndexOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "intptr");
}

//===----------------------------------------------------------------------===//
// ExtractStridedMetadataOp
//===----------------------------------------------------------------------===//

/// The number and type of the results are inferred from the
/// shape of the source.
LogicalResult ExtractStridedMetadataOp::inferReturnTypes(
    MLIRContext *context, std::optional<Location> location,
    ExtractStridedMetadataOp::Adaptor adaptor,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  auto sourceType = llvm::dyn_cast<MemRefType>(adaptor.getSource().getType());
  if (!sourceType)
    return failure();

  unsigned sourceRank = sourceType.getRank();
  IndexType indexType = IndexType::get(context);
  auto memrefType =
      MemRefType::get({}, sourceType.getElementType(),
                      MemRefLayoutAttrInterface{}, sourceType.getMemorySpace());
  // Base.
  inferredReturnTypes.push_back(memrefType);
  // Offset.
  inferredReturnTypes.push_back(indexType);
  // Sizes and strides.
  for (unsigned i = 0; i < sourceRank * 2; ++i)
    inferredReturnTypes.push_back(indexType);
  return success();
}

void ExtractStridedMetadataOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getBaseBuffer(), "base_buffer");
  setNameFn(getOffset(), "offset");
  // For multi-result to work properly with pretty names and packed syntax `x:3`
  // we can only give a pretty name to the first value in the pack.
  if (!getSizes().empty()) {
    setNameFn(getSizes().front(), "sizes");
    setNameFn(getStrides().front(), "strides");
  }
}

/// Helper function to perform the replacement of all constant uses of `values`
/// by a materialized constant extracted from `maybeConstants`.
/// `values` and `maybeConstants` are expected to have the same size.
template <typename Container>
static bool replaceConstantUsesOf(OpBuilder &rewriter, Location loc,
                                  Container values,
                                  ArrayRef<OpFoldResult> maybeConstants) {
  assert(values.size() == maybeConstants.size() &&
         " expected values and maybeConstants of the same size");
  bool atLeastOneReplacement = false;
  for (auto [maybeConstant, result] : llvm::zip(maybeConstants, values)) {
    // Don't materialize a constant if there are no uses: this would indice
    // infinite loops in the driver.
    if (result.use_empty() || maybeConstant == getAsOpFoldResult(result))
      continue;
    assert(isa<Attribute>(maybeConstant) &&
           "The constified value should be either unchanged (i.e., == result) "
           "or a constant");
    Value constantVal = rewriter.create<arith::ConstantIndexOp>(
        loc, llvm::cast<IntegerAttr>(cast<Attribute>(maybeConstant)).getInt());
    for (Operation *op : llvm::make_early_inc_range(result.getUsers())) {
      // modifyOpInPlace: lambda cannot capture structured bindings in C++17
      // yet.
      op->replaceUsesOfWith(result, constantVal);
      atLeastOneReplacement = true;
    }
  }
  return atLeastOneReplacement;
}

LogicalResult
ExtractStridedMetadataOp::fold(FoldAdaptor adaptor,
                               SmallVectorImpl<OpFoldResult> &results) {
  OpBuilder builder(*this);

  bool atLeastOneReplacement = replaceConstantUsesOf(
      builder, getLoc(), ArrayRef<TypedValue<IndexType>>(getOffset()),
      getConstifiedMixedOffset());
  atLeastOneReplacement |= replaceConstantUsesOf(builder, getLoc(), getSizes(),
                                                 getConstifiedMixedSizes());
  atLeastOneReplacement |= replaceConstantUsesOf(
      builder, getLoc(), getStrides(), getConstifiedMixedStrides());

  return success(atLeastOneReplacement);
}

SmallVector<OpFoldResult> ExtractStridedMetadataOp::getConstifiedMixedSizes() {
  SmallVector<OpFoldResult> values = getAsOpFoldResult(getSizes());
  constifyIndexValues(values, getSource().getType().getShape());
  return values;
}

SmallVector<OpFoldResult>
ExtractStridedMetadataOp::getConstifiedMixedStrides() {
  SmallVector<OpFoldResult> values = getAsOpFoldResult(getStrides());
  SmallVector<int64_t> staticValues;
  int64_t unused;
  LogicalResult status =
      getSource().getType().getStridesAndOffset(staticValues, unused);
  (void)status;
  assert(succeeded(status) && "could not get strides from type");
  constifyIndexValues(values, staticValues);
  return values;
}

OpFoldResult ExtractStridedMetadataOp::getConstifiedMixedOffset() {
  OpFoldResult offsetOfr = getAsOpFoldResult(getOffset());
  SmallVector<OpFoldResult> values(1, offsetOfr);
  SmallVector<int64_t> staticValues, unused;
  int64_t offset;
  LogicalResult status =
      getSource().getType().getStridesAndOffset(unused, offset);
  (void)status;
  assert(succeeded(status) && "could not get offset from type");
  staticValues.push_back(offset);
  constifyIndexValues(values, staticValues);
  return values[0];
}

//===----------------------------------------------------------------------===//
// GenericAtomicRMWOp
//===----------------------------------------------------------------------===//

void GenericAtomicRMWOp::build(OpBuilder &builder, OperationState &result,
                               Value memref, ValueRange ivs) {
  OpBuilder::InsertionGuard g(builder);
  result.addOperands(memref);
  result.addOperands(ivs);

  if (auto memrefType = llvm::dyn_cast<MemRefType>(memref.getType())) {
    Type elementType = memrefType.getElementType();
    result.addTypes(elementType);

    Region *bodyRegion = result.addRegion();
    builder.createBlock(bodyRegion);
    bodyRegion->addArgument(elementType, memref.getLoc());
  }
}

LogicalResult GenericAtomicRMWOp::verify() {
  auto &body = getRegion();
  if (body.getNumArguments() != 1)
    return emitOpError("expected single number of entry block arguments");

  if (getResult().getType() != body.getArgument(0).getType())
    return emitOpError("expected block argument of the same type result type");

  bool hasSideEffects =
      body.walk([&](Operation *nestedOp) {
            if (isMemoryEffectFree(nestedOp))
              return WalkResult::advance();
            nestedOp->emitError(
                "body of 'memref.generic_atomic_rmw' should contain "
                "only operations with no side effects");
            return WalkResult::interrupt();
          })
          .wasInterrupted();
  return hasSideEffects ? failure() : success();
}

ParseResult GenericAtomicRMWOp::parse(OpAsmParser &parser,
                                      OperationState &result) {
  OpAsmParser::UnresolvedOperand memref;
  Type memrefType;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> ivs;

  Type indexType = parser.getBuilder().getIndexType();
  if (parser.parseOperand(memref) ||
      parser.parseOperandList(ivs, OpAsmParser::Delimiter::Square) ||
      parser.parseColonType(memrefType) ||
      parser.resolveOperand(memref, memrefType, result.operands) ||
      parser.resolveOperands(ivs, indexType, result.operands))
    return failure();

  Region *body = result.addRegion();
  if (parser.parseRegion(*body, {}) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  result.types.push_back(llvm::cast<MemRefType>(memrefType).getElementType());
  return success();
}

void GenericAtomicRMWOp::print(OpAsmPrinter &p) {
  p << ' ' << getMemref() << "[" << getIndices()
    << "] : " << getMemref().getType() << ' ';
  p.printRegion(getRegion());
  p.printOptionalAttrDict((*this)->getAttrs());
}

//===----------------------------------------------------------------------===//
// AtomicYieldOp
//===----------------------------------------------------------------------===//

LogicalResult AtomicYieldOp::verify() {
  Type parentType = (*this)->getParentOp()->getResultTypes().front();
  Type resultType = getResult().getType();
  if (parentType != resultType)
    return emitOpError() << "types mismatch between yield op: " << resultType
                         << " and its parent: " << parentType;
  return success();
}

//===----------------------------------------------------------------------===//
// GlobalOp
//===----------------------------------------------------------------------===//

static void printGlobalMemrefOpTypeAndInitialValue(OpAsmPrinter &p, GlobalOp op,
                                                   TypeAttr type,
                                                   Attribute initialValue) {
  p << type;
  if (!op.isExternal()) {
    p << " = ";
    if (op.isUninitialized())
      p << "uninitialized";
    else
      p.printAttributeWithoutType(initialValue);
  }
}

static ParseResult
parseGlobalMemrefOpTypeAndInitialValue(OpAsmParser &parser, TypeAttr &typeAttr,
                                       Attribute &initialValue) {
  Type type;
  if (parser.parseType(type))
    return failure();

  auto memrefType = llvm::dyn_cast<MemRefType>(type);
  if (!memrefType || !memrefType.hasStaticShape())
    return parser.emitError(parser.getNameLoc())
           << "type should be static shaped memref, but got " << type;
  typeAttr = TypeAttr::get(type);

  if (parser.parseOptionalEqual())
    return success();

  if (succeeded(parser.parseOptionalKeyword("uninitialized"))) {
    initialValue = UnitAttr::get(parser.getContext());
    return success();
  }

  Type tensorType = getTensorTypeFromMemRefType(memrefType);
  if (parser.parseAttribute(initialValue, tensorType))
    return failure();
  if (!llvm::isa<ElementsAttr>(initialValue))
    return parser.emitError(parser.getNameLoc())
           << "initial value should be a unit or elements attribute";
  return success();
}

LogicalResult GlobalOp::verify() {
  auto memrefType = llvm::dyn_cast<MemRefType>(getType());
  if (!memrefType || !memrefType.hasStaticShape())
    return emitOpError("type should be static shaped memref, but got ")
           << getType();

  // Verify that the initial value, if present, is either a unit attribute or
  // an elements attribute.
  if (getInitialValue().has_value()) {
    Attribute initValue = getInitialValue().value();
    if (!llvm::isa<UnitAttr>(initValue) && !llvm::isa<ElementsAttr>(initValue))
      return emitOpError("initial value should be a unit or elements "
                         "attribute, but got ")
             << initValue;

    // Check that the type of the initial value is compatible with the type of
    // the global variable.
    if (auto elementsAttr = llvm::dyn_cast<ElementsAttr>(initValue)) {
      // Check the element types match.
      auto initElementType =
          cast<TensorType>(elementsAttr.getType()).getElementType();
      auto memrefElementType = memrefType.getElementType();

      if (initElementType != memrefElementType)
        return emitOpError("initial value element expected to be of type ")
               << memrefElementType << ", but was of type " << initElementType;

      // Check the shapes match, given that memref globals can only produce
      // statically shaped memrefs and elements literal type must have a static
      // shape we can assume both types are shaped.
      auto initShape = elementsAttr.getShapedType().getShape();
      auto memrefShape = memrefType.getShape();
      if (initShape != memrefShape)
        return emitOpError("initial value shape expected to be ")
               << memrefShape << " but was " << initShape;
    }
  }

  if (std::optional<uint64_t> alignAttr = getAlignment()) {
    uint64_t alignment = *alignAttr;

    if (!llvm::isPowerOf2_64(alignment))
      return emitError() << "alignment attribute value " << alignment
                         << " is not a power of 2";
  }

  // TODO: verify visibility for declarations.
  return success();
}

ElementsAttr GlobalOp::getConstantInitValue() {
  auto initVal = getInitialValue();
  if (getConstant() && initVal.has_value())
    return llvm::cast<ElementsAttr>(initVal.value());
  return {};
}

//===----------------------------------------------------------------------===//
// GetGlobalOp
//===----------------------------------------------------------------------===//

LogicalResult
GetGlobalOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  // Verify that the result type is same as the type of the referenced
  // memref.global op.
  auto global =
      symbolTable.lookupNearestSymbolFrom<GlobalOp>(*this, getNameAttr());
  if (!global)
    return emitOpError("'")
           << getName() << "' does not reference a valid global memref";

  Type resultType = getResult().getType();
  if (global.getType() != resultType)
    return emitOpError("result type ")
           << resultType << " does not match type " << global.getType()
           << " of the global memref @" << getName();
  return success();
}

//===----------------------------------------------------------------------===//
// LoadOp
//===----------------------------------------------------------------------===//

LogicalResult LoadOp::verify() {
  if (static_cast<int64_t>(getIndices().size()) != getMemRefType().getRank()) {
    return emitOpError("incorrect number of indices for load, expected ")
           << getMemRefType().getRank() << " but got " << getIndices().size();
  }
  return success();
}

OpFoldResult LoadOp::fold(FoldAdaptor adaptor) {
  /// load(memrefcast) -> load
  if (succeeded(foldMemRefCast(*this)))
    return getResult();
  return OpFoldResult();
}

//===----------------------------------------------------------------------===//
// MemorySpaceCastOp
//===----------------------------------------------------------------------===//

void MemorySpaceCastOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "memspacecast");
}

bool MemorySpaceCastOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  Type a = inputs.front(), b = outputs.front();
  auto aT = llvm::dyn_cast<MemRefType>(a);
  auto bT = llvm::dyn_cast<MemRefType>(b);

  auto uaT = llvm::dyn_cast<UnrankedMemRefType>(a);
  auto ubT = llvm::dyn_cast<UnrankedMemRefType>(b);

  if (aT && bT) {
    if (aT.getElementType() != bT.getElementType())
      return false;
    if (aT.getLayout() != bT.getLayout())
      return false;
    if (aT.getShape() != bT.getShape())
      return false;
    return true;
  }
  if (uaT && ubT) {
    return uaT.getElementType() == ubT.getElementType();
  }
  return false;
}

OpFoldResult MemorySpaceCastOp::fold(FoldAdaptor adaptor) {
  // memory_space_cast(memory_space_cast(v, t1), t2) -> memory_space_cast(v,
  // t2)
  if (auto parentCast = getSource().getDefiningOp<MemorySpaceCastOp>()) {
    getSourceMutable().assign(parentCast.getSource());
    return getResult();
  }
  return Value{};
}

//===----------------------------------------------------------------------===//
// PrefetchOp
//===----------------------------------------------------------------------===//

void PrefetchOp::print(OpAsmPrinter &p) {
  p << " " << getMemref() << '[';
  p.printOperands(getIndices());
  p << ']' << ", " << (getIsWrite() ? "write" : "read");
  p << ", locality<" << getLocalityHint();
  p << ">, " << (getIsDataCache() ? "data" : "instr");
  p.printOptionalAttrDict(
      (*this)->getAttrs(),
      /*elidedAttrs=*/{"localityHint", "isWrite", "isDataCache"});
  p << " : " << getMemRefType();
}

ParseResult PrefetchOp::parse(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::UnresolvedOperand memrefInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> indexInfo;
  IntegerAttr localityHint;
  MemRefType type;
  StringRef readOrWrite, cacheType;

  auto indexTy = parser.getBuilder().getIndexType();
  auto i32Type = parser.getBuilder().getIntegerType(32);
  if (parser.parseOperand(memrefInfo) ||
      parser.parseOperandList(indexInfo, OpAsmParser::Delimiter::Square) ||
      parser.parseComma() || parser.parseKeyword(&readOrWrite) ||
      parser.parseComma() || parser.parseKeyword("locality") ||
      parser.parseLess() ||
      parser.parseAttribute(localityHint, i32Type, "localityHint",
                            result.attributes) ||
      parser.parseGreater() || parser.parseComma() ||
      parser.parseKeyword(&cacheType) || parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(indexInfo, indexTy, result.operands))
    return failure();

  if (readOrWrite != "read" && readOrWrite != "write")
    return parser.emitError(parser.getNameLoc(),
                            "rw specifier has to be 'read' or 'write'");
  result.addAttribute(PrefetchOp::getIsWriteAttrStrName(),
                      parser.getBuilder().getBoolAttr(readOrWrite == "write"));

  if (cacheType != "data" && cacheType != "instr")
    return parser.emitError(parser.getNameLoc(),
                            "cache type has to be 'data' or 'instr'");

  result.addAttribute(PrefetchOp::getIsDataCacheAttrStrName(),
                      parser.getBuilder().getBoolAttr(cacheType == "data"));

  return success();
}

LogicalResult PrefetchOp::verify() {
  if (getNumOperands() != 1 + getMemRefType().getRank())
    return emitOpError("too few indices");

  return success();
}

LogicalResult PrefetchOp::fold(FoldAdaptor adaptor,
                               SmallVectorImpl<OpFoldResult> &results) {
  // prefetch(memrefcast) -> prefetch
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

OpFoldResult RankOp::fold(FoldAdaptor adaptor) {
  // Constant fold rank when the rank of the operand is known.
  auto type = getOperand().getType();
  auto shapedType = llvm::dyn_cast<ShapedType>(type);
  if (shapedType && shapedType.hasRank())
    return IntegerAttr::get(IndexType::get(getContext()), shapedType.getRank());
  return IntegerAttr();
}

//===----------------------------------------------------------------------===//
// ReinterpretCastOp
//===----------------------------------------------------------------------===//

void ReinterpretCastOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "reinterpret_cast");
}

/// Build a ReinterpretCastOp with all dynamic entries: `staticOffsets`,
/// `staticSizes` and `staticStrides` are automatically filled with
/// source-memref-rank sentinel values that encode dynamic entries.
void ReinterpretCastOp::build(OpBuilder &b, OperationState &result,
                              MemRefType resultType, Value source,
                              OpFoldResult offset, ArrayRef<OpFoldResult> sizes,
                              ArrayRef<OpFoldResult> strides,
                              ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offset, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  result.addAttributes(attrs);
  build(b, result, resultType, source, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getDenseI64ArrayAttr(staticOffsets),
        b.getDenseI64ArrayAttr(staticSizes),
        b.getDenseI64ArrayAttr(staticStrides));
}

void ReinterpretCastOp::build(OpBuilder &b, OperationState &result,
                              Value source, OpFoldResult offset,
                              ArrayRef<OpFoldResult> sizes,
                              ArrayRef<OpFoldResult> strides,
                              ArrayRef<NamedAttribute> attrs) {
  auto sourceType = cast<BaseMemRefType>(source.getType());
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offset, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  auto stridedLayout = StridedLayoutAttr::get(
      b.getContext(), staticOffsets.front(), staticStrides);
  auto resultType = MemRefType::get(staticSizes, sourceType.getElementType(),
                                    stridedLayout, sourceType.getMemorySpace());
  build(b, result, resultType, source, offset, sizes, strides, attrs);
}

void ReinterpretCastOp::build(OpBuilder &b, OperationState &result,
                              MemRefType resultType, Value source,
                              int64_t offset, ArrayRef<int64_t> sizes,
                              ArrayRef<int64_t> strides,
                              ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> sizeValues =
      llvm::to_vector<4>(llvm::map_range(sizes, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  build(b, result, resultType, source, b.getI64IntegerAttr(offset), sizeValues,
        strideValues, attrs);
}

void ReinterpretCastOp::build(OpBuilder &b, OperationState &result,
                              MemRefType resultType, Value source, Value offset,
                              ValueRange sizes, ValueRange strides,
                              ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, resultType, source, offset, sizeValues, strideValues, attrs);
}

// TODO: ponder whether we want to allow missing trailing sizes/strides that are
// completed automatically, like we have for subview and extract_slice.
LogicalResult ReinterpretCastOp::verify() {
  // The source and result memrefs should be in the same memory space.
  auto srcType = llvm::cast<BaseMemRefType>(getSource().getType());
  auto resultType = llvm::cast<MemRefType>(getType());
  if (srcType.getMemorySpace() != resultType.getMemorySpace())
    return emitError("different memory spaces specified for source type ")
           << srcType << " and result memref type " << resultType;
  if (srcType.getElementType() != resultType.getElementType())
    return emitError("different element types specified for source type ")
           << srcType << " and result memref type " << resultType;

  // Match sizes in result memref type and in static_sizes attribute.
  for (auto [idx, resultSize, expectedSize] :
       llvm::enumerate(resultType.getShape(), getStaticSizes())) {
    if (ShapedType::isStatic(resultSize) && resultSize != expectedSize)
      return emitError("expected result type with size = ")
             << (ShapedType::isDynamic(expectedSize)
                     ? std::string("dynamic")
                     : std::to_string(expectedSize))
             << " instead of " << resultSize << " in dim = " << idx;
  }

  // Match offset and strides in static_offset and static_strides attributes. If
  // result memref type has no affine map specified, this will assume an
  // identity layout.
  int64_t resultOffset;
  SmallVector<int64_t, 4> resultStrides;
  if (failed(resultType.getStridesAndOffset(resultStrides, resultOffset)))
    return emitError("expected result type to have strided layout but found ")
           << resultType;

  // Match offset in result memref type and in static_offsets attribute.
  int64_t expectedOffset = getStaticOffsets().front();
  if (ShapedType::isStatic(resultOffset) && resultOffset != expectedOffset)
    return emitError("expected result type with offset = ")
           << (ShapedType::isDynamic(expectedOffset)
                   ? std::string("dynamic")
                   : std::to_string(expectedOffset))
           << " instead of " << resultOffset;

  // Match strides in result memref type and in static_strides attribute.
  for (auto [idx, resultStride, expectedStride] :
       llvm::enumerate(resultStrides, getStaticStrides())) {
    if (ShapedType::isStatic(resultStride) && resultStride != expectedStride)
      return emitError("expected result type with stride = ")
             << (ShapedType::isDynamic(expectedStride)
                     ? std::string("dynamic")
                     : std::to_string(expectedStride))
             << " instead of " << resultStride << " in dim = " << idx;
  }

  return success();
}

OpFoldResult ReinterpretCastOp::fold(FoldAdaptor /*operands*/) {
  Value src = getSource();
  auto getPrevSrc = [&]() -> Value {
    // reinterpret_cast(reinterpret_cast(x)) -> reinterpret_cast(x).
    if (auto prev = src.getDefiningOp<ReinterpretCastOp>())
      return prev.getSource();

    // reinterpret_cast(cast(x)) -> reinterpret_cast(x).
    if (auto prev = src.getDefiningOp<CastOp>())
      return prev.getSource();

    // reinterpret_cast(subview(x)) -> reinterpret_cast(x) if subview offsets
    // are 0.
    if (auto prev = src.getDefiningOp<SubViewOp>())
      if (llvm::all_of(prev.getMixedOffsets(), isZeroInteger))
        return prev.getSource();

    return nullptr;
  };

  if (auto prevSrc = getPrevSrc()) {
    getSourceMutable().assign(prevSrc);
    return getResult();
  }

  // reinterpret_cast(x) w/o offset/shape/stride changes -> x
  if (ShapedType::isStaticShape(getType().getShape()) &&
      src.getType() == getType() && getStaticOffsets().front() == 0) {
    return src;
  }

  return nullptr;
}

SmallVector<OpFoldResult> ReinterpretCastOp::getConstifiedMixedSizes() {
  SmallVector<OpFoldResult> values = getMixedSizes();
  constifyIndexValues(values, getType().getShape());
  return values;
}

SmallVector<OpFoldResult> ReinterpretCastOp::getConstifiedMixedStrides() {
  SmallVector<OpFoldResult> values = getMixedStrides();
  SmallVector<int64_t> staticValues;
  int64_t unused;
  LogicalResult status = getType().getStridesAndOffset(staticValues, unused);
  (void)status;
  assert(succeeded(status) && "could not get strides from type");
  constifyIndexValues(values, staticValues);
  return values;
}

OpFoldResult ReinterpretCastOp::getConstifiedMixedOffset() {
  SmallVector<OpFoldResult> values = getMixedOffsets();
  assert(values.size() == 1 &&
         "reinterpret_cast must have one and only one offset");
  SmallVector<int64_t> staticValues, unused;
  int64_t offset;
  LogicalResult status = getType().getStridesAndOffset(unused, offset);
  (void)status;
  assert(succeeded(status) && "could not get offset from type");
  staticValues.push_back(offset);
  constifyIndexValues(values, staticValues);
  return values[0];
}

namespace {
/// Replace the sequence:
/// ```
/// base, offset, sizes, strides = extract_strided_metadata src
/// dst = reinterpret_cast base to offset, sizes, strides
/// ```
/// With
///
/// ```
/// dst = memref.cast src
/// ```
///
/// Note: The cast operation is only inserted when the type of dst and src
/// are not the same. E.g., when going from <4xf32> to <?xf32>.
///
/// This pattern also matches when the offset, sizes, and strides don't come
/// directly from the `extract_strided_metadata`'s results but it can be
/// statically proven that they would hold the same values.
///
/// For instance, the following sequence would be replaced:
/// ```
/// base, offset, sizes, strides =
///   extract_strided_metadata memref : memref<3x4xty>
/// dst = reinterpret_cast base to 0, [3, 4], strides
/// ```
/// Because we know (thanks to the type of the input memref) that variable
/// `offset` and `sizes` will respectively hold 0 and [3, 4].
///
/// Similarly, the following sequence would be replaced:
/// ```
/// c0 = arith.constant 0
/// c4 = arith.constant 4
/// base, offset, sizes, strides =
///   extract_strided_metadata memref : memref<3x4xty>
/// dst = reinterpret_cast base to c0, [3, c4], strides
/// ```
/// Because we know that `offset`and `c0` will hold 0
/// and `c4` will hold 4.
///
/// If the pattern above does not match, the input of the
/// extract_strided_metadata is always folded into the input of the
/// reinterpret_cast operator. This allows for dead code elimination to get rid
/// of the extract_strided_metadata in some cases.
struct ReinterpretCastOpExtractStridedMetadataFolder
    : public OpRewritePattern<ReinterpretCastOp> {
public:
  using OpRewritePattern<ReinterpretCastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReinterpretCastOp op,
                                PatternRewriter &rewriter) const override {
    auto extractStridedMetadata =
        op.getSource().getDefiningOp<ExtractStridedMetadataOp>();
    if (!extractStridedMetadata)
      return failure();

    // Check if the reinterpret cast reconstructs a memref with the exact same
    // properties as the extract strided metadata.
    auto isReinterpretCastNoop = [&]() -> bool {
      // First, check that the strides are the same.
      if (!llvm::equal(extractStridedMetadata.getConstifiedMixedStrides(),
                       op.getConstifiedMixedStrides()))
        return false;

      // Second, check the sizes.
      if (!llvm::equal(extractStridedMetadata.getConstifiedMixedSizes(),
                       op.getConstifiedMixedSizes()))
        return false;

      // Finally, check the offset.
      assert(op.getMixedOffsets().size() == 1 &&
             "reinterpret_cast with more than one offset should have been "
             "rejected by the verifier");
      return extractStridedMetadata.getConstifiedMixedOffset() ==
             op.getConstifiedMixedOffset();
    };

    if (!isReinterpretCastNoop()) {
      // If the extract_strided_metadata / reinterpret_cast pair can't be
      // completely folded, then we could fold the input of the
      // extract_strided_metadata into the input of the reinterpret_cast
      // input. For some cases (e.g., static dimensions) the
      // the extract_strided_metadata is eliminated by dead code elimination.
      //
      // reinterpret_cast(extract_strided_metadata(x)) -> reinterpret_cast(x).
      //
      // We can always fold the input of a extract_strided_metadata operator
      // to the input of a reinterpret_cast operator, because they point to
      // the same memory. Note that the reinterpret_cast does not use the
      // layout of its input memref, only its base memory pointer which is
      // the same as the base pointer returned by the extract_strided_metadata
      // operator and the base pointer of the extract_strided_metadata memref
      // input.
      rewriter.modifyOpInPlace(op, [&]() {
        op.getSourceMutable().assign(extractStridedMetadata.getSource());
      });
      return success();
    }

    // At this point, we know that the back and forth between extract strided
    // metadata and reinterpret cast is a noop. However, the final type of the
    // reinterpret cast may not be exactly the same as the original memref.
    // E.g., it could be changing a dimension from static to dynamic. Check that
    // here and add a cast if necessary.
    Type srcTy = extractStridedMetadata.getSource().getType();
    if (srcTy == op.getResult().getType())
      rewriter.replaceOp(op, extractStridedMetadata.getSource());
    else
      rewriter.replaceOpWithNewOp<CastOp>(op, op.getType(),
                                          extractStridedMetadata.getSource());

    return success();
  }
};
} // namespace

void ReinterpretCastOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                    MLIRContext *context) {
  results.add<ReinterpretCastOpExtractStridedMetadataFolder>(context);
}

//===----------------------------------------------------------------------===//
// Reassociative reshape ops
//===----------------------------------------------------------------------===//

void CollapseShapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "collapse_shape");
}

void ExpandShapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "expand_shape");
}

LogicalResult ExpandShapeOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedResultShapes) {
  reifiedResultShapes = {
      getMixedValues(getStaticOutputShape(), getOutputShape(), builder)};
  return success();
}

/// Helper function for verifying the shape of ExpandShapeOp and ResultShapeOp
/// result and operand. Layout maps are verified separately.
///
/// If `allowMultipleDynamicDimsPerGroup`, multiple dynamic dimensions are
/// allowed in a reassocation group.
static LogicalResult
verifyCollapsedShape(Operation *op, ArrayRef<int64_t> collapsedShape,
                     ArrayRef<int64_t> expandedShape,
                     ArrayRef<ReassociationIndices> reassociation,
                     bool allowMultipleDynamicDimsPerGroup) {
  // There must be one reassociation group per collapsed dimension.
  if (collapsedShape.size() != reassociation.size())
    return op->emitOpError("invalid number of reassociation groups: found ")
           << reassociation.size() << ", expected " << collapsedShape.size();

  // The next expected expanded dimension index (while iterating over
  // reassociation indices).
  int64_t nextDim = 0;
  for (const auto &it : llvm::enumerate(reassociation)) {
    ReassociationIndices group = it.value();
    int64_t collapsedDim = it.index();

    bool foundDynamic = false;
    for (int64_t expandedDim : group) {
      if (expandedDim != nextDim++)
        return op->emitOpError("reassociation indices must be contiguous");

      if (expandedDim >= static_cast<int64_t>(expandedShape.size()))
        return op->emitOpError("reassociation index ")
               << expandedDim << " is out of bounds";

      // Check if there are multiple dynamic dims in a reassociation group.
      if (ShapedType::isDynamic(expandedShape[expandedDim])) {
        if (foundDynamic && !allowMultipleDynamicDimsPerGroup)
          return op->emitOpError(
              "at most one dimension in a reassociation group may be dynamic");
        foundDynamic = true;
      }
    }

    // ExpandShapeOp/CollapseShapeOp may not be used to cast dynamicity.
    if (ShapedType::isDynamic(collapsedShape[collapsedDim]) != foundDynamic)
      return op->emitOpError("collapsed dim (")
             << collapsedDim
             << ") must be dynamic if and only if reassociation group is "
                "dynamic";

    // If all dims in the reassociation group are static, the size of the
    // collapsed dim can be verified.
    if (!foundDynamic) {
      int64_t groupSize = 1;
      for (int64_t expandedDim : group)
        groupSize *= expandedShape[expandedDim];
      if (groupSize != collapsedShape[collapsedDim])
        return op->emitOpError("collapsed dim size (")
               << collapsedShape[collapsedDim]
               << ") must equal reassociation group size (" << groupSize << ")";
    }
  }

  if (collapsedShape.empty()) {
    // Rank 0: All expanded dimensions must be 1.
    for (int64_t d : expandedShape)
      if (d != 1)
        return op->emitOpError(
            "rank 0 memrefs can only be extended/collapsed with/from ones");
  } else if (nextDim != static_cast<int64_t>(expandedShape.size())) {
    // Rank >= 1: Number of dimensions among all reassociation groups must match
    // the result memref rank.
    return op->emitOpError("expanded rank (")
           << expandedShape.size()
           << ") inconsistent with number of reassociation indices (" << nextDim
           << ")";
  }

  return success();
}

SmallVector<AffineMap, 4> CollapseShapeOp::getReassociationMaps() {
  return getSymbolLessAffineMaps(getReassociationExprs());
}

SmallVector<ReassociationExprs, 4> CollapseShapeOp::getReassociationExprs() {
  return convertReassociationIndicesToExprs(getContext(),
                                            getReassociationIndices());
}

SmallVector<AffineMap, 4> ExpandShapeOp::getReassociationMaps() {
  return getSymbolLessAffineMaps(getReassociationExprs());
}

SmallVector<ReassociationExprs, 4> ExpandShapeOp::getReassociationExprs() {
  return convertReassociationIndicesToExprs(getContext(),
                                            getReassociationIndices());
}

/// Compute the layout map after expanding a given source MemRef type with the
/// specified reassociation indices.
static FailureOr<StridedLayoutAttr>
computeExpandedLayoutMap(MemRefType srcType, ArrayRef<int64_t> resultShape,
                         ArrayRef<ReassociationIndices> reassociation) {
  int64_t srcOffset;
  SmallVector<int64_t> srcStrides;
  if (failed(srcType.getStridesAndOffset(srcStrides, srcOffset)))
    return failure();
  assert(srcStrides.size() == reassociation.size() && "invalid reassociation");

  // 1-1 mapping between srcStrides and reassociation packs.
  // Each srcStride starts with the given value and gets expanded according to
  // the proper entries in resultShape.
  // Example:
  //   srcStrides     =                   [10000,  1 ,    100   ],
  //   reassociations =                   [  [0], [1], [2, 3, 4]],
  //   resultSizes    = [2, 5, 4, 3, 2] = [  [2], [5], [4, 3, 2]]
  //     -> For the purpose of stride calculation, the useful sizes are:
  //                    [x, x, x, 3, 2] = [  [x], [x], [x, 3, 2]].
  //   resultStrides = [10000, 1, 600, 200, 100]
  // Note that a stride does not get expanded along the first entry of each
  // shape pack.
  SmallVector<int64_t> reverseResultStrides;
  reverseResultStrides.reserve(resultShape.size());
  unsigned shapeIndex = resultShape.size() - 1;
  for (auto it : llvm::reverse(llvm::zip(reassociation, srcStrides))) {
    ReassociationIndices reassoc = std::get<0>(it);
    int64_t currentStrideToExpand = std::get<1>(it);
    for (unsigned idx = 0, e = reassoc.size(); idx < e; ++idx) {
      reverseResultStrides.push_back(currentStrideToExpand);
      currentStrideToExpand =
          (SaturatedInteger::wrap(currentStrideToExpand) *
           SaturatedInteger::wrap(resultShape[shapeIndex--]))
              .asInteger();
    }
  }
  auto resultStrides = llvm::to_vector<8>(llvm::reverse(reverseResultStrides));
  resultStrides.resize(resultShape.size(), 1);
  return StridedLayoutAttr::get(srcType.getContext(), srcOffset, resultStrides);
}

FailureOr<MemRefType> ExpandShapeOp::computeExpandedType(
    MemRefType srcType, ArrayRef<int64_t> resultShape,
    ArrayRef<ReassociationIndices> reassociation) {
  if (srcType.getLayout().isIdentity()) {
    // If the source is contiguous (i.e., no layout map specified), so is the
    // result.
    MemRefLayoutAttrInterface layout;
    return MemRefType::get(resultShape, srcType.getElementType(), layout,
                           srcType.getMemorySpace());
  }

  // Source may not be contiguous. Compute the layout map.
  FailureOr<StridedLayoutAttr> computedLayout =
      computeExpandedLayoutMap(srcType, resultShape, reassociation);
  if (failed(computedLayout))
    return failure();
  return MemRefType::get(resultShape, srcType.getElementType(), *computedLayout,
                         srcType.getMemorySpace());
}

FailureOr<SmallVector<OpFoldResult>>
ExpandShapeOp::inferOutputShape(OpBuilder &b, Location loc,
                                MemRefType expandedType,
                                ArrayRef<ReassociationIndices> reassociation,
                                ArrayRef<OpFoldResult> inputShape) {
  std::optional<SmallVector<OpFoldResult>> outputShape =
      inferExpandShapeOutputShape(b, loc, expandedType, reassociation,
                                  inputShape);
  if (!outputShape)
    return failure();
  return *outputShape;
}

void ExpandShapeOp::build(OpBuilder &builder, OperationState &result,
                          Type resultType, Value src,
                          ArrayRef<ReassociationIndices> reassociation,
                          ArrayRef<OpFoldResult> outputShape) {
  auto [staticOutputShape, dynamicOutputShape] =
      decomposeMixedValues(SmallVector<OpFoldResult>(outputShape));
  build(builder, result, llvm::cast<MemRefType>(resultType), src,
        getReassociationIndicesAttribute(builder, reassociation),
        dynamicOutputShape, staticOutputShape);
}

void ExpandShapeOp::build(OpBuilder &builder, OperationState &result,
                          Type resultType, Value src,
                          ArrayRef<ReassociationIndices> reassociation) {
  SmallVector<OpFoldResult> inputShape =
      getMixedSizes(builder, result.location, src);
  MemRefType memrefResultTy = llvm::cast<MemRefType>(resultType);
  FailureOr<SmallVector<OpFoldResult>> outputShape = inferOutputShape(
      builder, result.location, memrefResultTy, reassociation, inputShape);
  // Failure of this assertion usually indicates presence of multiple
  // dynamic dimensions in the same reassociation group.
  assert(succeeded(outputShape) && "unable to infer output shape");
  build(builder, result, memrefResultTy, src, reassociation, *outputShape);
}

void ExpandShapeOp::build(OpBuilder &builder, OperationState &result,
                          ArrayRef<int64_t> resultShape, Value src,
                          ArrayRef<ReassociationIndices> reassociation) {
  // Only ranked memref source values are supported.
  auto srcType = llvm::cast<MemRefType>(src.getType());
  FailureOr<MemRefType> resultType =
      ExpandShapeOp::computeExpandedType(srcType, resultShape, reassociation);
  // Failure of this assertion usually indicates a problem with the source
  // type, e.g., could not get strides/offset.
  assert(succeeded(resultType) && "could not compute layout");
  build(builder, result, *resultType, src, reassociation);
}

void ExpandShapeOp::build(OpBuilder &builder, OperationState &result,
                          ArrayRef<int64_t> resultShape, Value src,
                          ArrayRef<ReassociationIndices> reassociation,
                          ArrayRef<OpFoldResult> outputShape) {
  // Only ranked memref source values are supported.
  auto srcType = llvm::cast<MemRefType>(src.getType());
  FailureOr<MemRefType> resultType =
      ExpandShapeOp::computeExpandedType(srcType, resultShape, reassociation);
  // Failure of this assertion usually indicates a problem with the source
  // type, e.g., could not get strides/offset.
  assert(succeeded(resultType) && "could not compute layout");
  build(builder, result, *resultType, src, reassociation, outputShape);
}

LogicalResult ExpandShapeOp::verify() {
  MemRefType srcType = getSrcType();
  MemRefType resultType = getResultType();

  if (srcType.getRank() > resultType.getRank()) {
    auto r0 = srcType.getRank();
    auto r1 = resultType.getRank();
    return emitOpError("has source rank ")
           << r0 << " and result rank " << r1 << ". This is not an expansion ("
           << r0 << " > " << r1 << ").";
  }

  // Verify result shape.
  if (failed(verifyCollapsedShape(getOperation(), srcType.getShape(),
                                  resultType.getShape(),
                                  getReassociationIndices(),
                                  /*allowMultipleDynamicDimsPerGroup=*/true)))
    return failure();

  // Compute expected result type (including layout map).
  FailureOr<MemRefType> expectedResultType = ExpandShapeOp::computeExpandedType(
      srcType, resultType.getShape(), getReassociationIndices());
  if (failed(expectedResultType))
    return emitOpError("invalid source layout map");

  // Check actual result type.
  if (*expectedResultType != resultType)
    return emitOpError("expected expanded type to be ")
           << *expectedResultType << " but found " << resultType;

  if ((int64_t)getStaticOutputShape().size() != resultType.getRank())
    return emitOpError("expected number of static shape bounds to be equal to "
                       "the output rank (")
           << resultType.getRank() << ") but found "
           << getStaticOutputShape().size() << " inputs instead";

  if ((int64_t)getOutputShape().size() !=
      llvm::count(getStaticOutputShape(), ShapedType::kDynamic))
    return emitOpError("mismatch in dynamic dims in output_shape and "
                       "static_output_shape: static_output_shape has ")
           << llvm::count(getStaticOutputShape(), ShapedType::kDynamic)
           << " dynamic dims while output_shape has " << getOutputShape().size()
           << " values";

  // Verify if provided output shapes are in agreement with output type.
  DenseI64ArrayAttr staticOutputShapes = getStaticOutputShapeAttr();
  ArrayRef<int64_t> resShape = getResult().getType().getShape();
  for (auto [pos, shape] : llvm::enumerate(resShape)) {
    if (ShapedType::isStatic(shape) && shape != staticOutputShapes[pos]) {
      return emitOpError("invalid output shape provided at pos ") << pos;
    }
  }

  return success();
}

void ExpandShapeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<
      ComposeReassociativeReshapeOps<ExpandShapeOp, ReshapeOpKind::kExpand>,
      ComposeExpandOfCollapseOp<ExpandShapeOp, CollapseShapeOp>>(context);
}

/// Compute the layout map after collapsing a given source MemRef type with the
/// specified reassociation indices.
///
/// Note: All collapsed dims in a reassociation group must be contiguous. It is
/// not possible to check this by inspecting a MemRefType in the general case.
/// If non-contiguity cannot be checked statically, the collapse is assumed to
/// be valid (and thus accepted by this function) unless `strict = true`.
static FailureOr<StridedLayoutAttr>
computeCollapsedLayoutMap(MemRefType srcType,
                          ArrayRef<ReassociationIndices> reassociation,
                          bool strict = false) {
  int64_t srcOffset;
  SmallVector<int64_t> srcStrides;
  auto srcShape = srcType.getShape();
  if (failed(srcType.getStridesAndOffset(srcStrides, srcOffset)))
    return failure();

  // The result stride of a reassociation group is the stride of the last entry
  // of the reassociation. (TODO: Should be the minimum stride in the
  // reassociation because strides are not necessarily sorted. E.g., when using
  // memref.transpose.) Dimensions of size 1 should be skipped, because their
  // strides are meaningless and could have any arbitrary value.
  SmallVector<int64_t> resultStrides;
  resultStrides.reserve(reassociation.size());
  for (const ReassociationIndices &reassoc : reassociation) {
    ArrayRef<int64_t> ref = llvm::ArrayRef(reassoc);
    while (srcShape[ref.back()] == 1 && ref.size() > 1)
      ref = ref.drop_back();
    if (ShapedType::isStatic(srcShape[ref.back()]) || ref.size() == 1) {
      resultStrides.push_back(srcStrides[ref.back()]);
    } else {
      // Dynamically-sized dims may turn out to be dims of size 1 at runtime, so
      // the corresponding stride may have to be skipped. (See above comment.)
      // Therefore, the result stride cannot be statically determined and must
      // be dynamic.
      resultStrides.push_back(ShapedType::kDynamic);
    }
  }

  // Validate that each reassociation group is contiguous.
  unsigned resultStrideIndex = resultStrides.size() - 1;
  for (const ReassociationIndices &reassoc : llvm::reverse(reassociation)) {
    auto trailingReassocs = ArrayRef<int64_t>(reassoc).drop_front();
    auto stride = SaturatedInteger::wrap(resultStrides[resultStrideIndex--]);
    for (int64_t idx : llvm::reverse(trailingReassocs)) {
      stride = stride * SaturatedInteger::wrap(srcShape[idx]);

      // Both source and result stride must have the same static value. In that
      // case, we can be sure, that the dimensions are collapsible (because they
      // are contiguous).
      // If `strict = false` (default during op verification), we accept cases
      // where one or both strides are dynamic. This is best effort: We reject
      // ops where obviously non-contiguous dims are collapsed, but accept ops
      // where we cannot be sure statically. Such ops may fail at runtime. See
      // the op documentation for details.
      auto srcStride = SaturatedInteger::wrap(srcStrides[idx - 1]);
      if (strict && (stride.saturated || srcStride.saturated))
        return failure();

      // Dimensions of size 1 should be skipped, because their strides are
      // meaningless and could have any arbitrary value.
      if (srcShape[idx - 1] == 1)
        continue;

      if (!stride.saturated && !srcStride.saturated && stride != srcStride)
        return failure();
    }
  }
  return StridedLayoutAttr::get(srcType.getContext(), srcOffset, resultStrides);
}

bool CollapseShapeOp::isGuaranteedCollapsible(
    MemRefType srcType, ArrayRef<ReassociationIndices> reassociation) {
  // MemRefs with identity layout are always collapsible.
  if (srcType.getLayout().isIdentity())
    return true;

  return succeeded(computeCollapsedLayoutMap(srcType, reassociation,
                                             /*strict=*/true));
}

MemRefType CollapseShapeOp::computeCollapsedType(
    MemRefType srcType, ArrayRef<ReassociationIndices> reassociation) {
  SmallVector<int64_t> resultShape;
  resultShape.reserve(reassociation.size());
  for (const ReassociationIndices &group : reassociation) {
    auto groupSize = SaturatedInteger::wrap(1);
    for (int64_t srcDim : group)
      groupSize =
          groupSize * SaturatedInteger::wrap(srcType.getDimSize(srcDim));
    resultShape.push_back(groupSize.asInteger());
  }

  if (srcType.getLayout().isIdentity()) {
    // If the source is contiguous (i.e., no layout map specified), so is the
    // result.
    MemRefLayoutAttrInterface layout;
    return MemRefType::get(resultShape, srcType.getElementType(), layout,
                           srcType.getMemorySpace());
  }

  // Source may not be fully contiguous. Compute the layout map.
  // Note: Dimensions that are collapsed into a single dim are assumed to be
  // contiguous.
  FailureOr<StridedLayoutAttr> computedLayout =
      computeCollapsedLayoutMap(srcType, reassociation);
  assert(succeeded(computedLayout) &&
         "invalid source layout map or collapsing non-contiguous dims");
  return MemRefType::get(resultShape, srcType.getElementType(), *computedLayout,
                         srcType.getMemorySpace());
}

void CollapseShapeOp::build(OpBuilder &b, OperationState &result, Value src,
                            ArrayRef<ReassociationIndices> reassociation,
                            ArrayRef<NamedAttribute> attrs) {
  auto srcType = llvm::cast<MemRefType>(src.getType());
  MemRefType resultType =
      CollapseShapeOp::computeCollapsedType(srcType, reassociation);
  result.addAttribute(::mlir::getReassociationAttrName(),
                      getReassociationIndicesAttribute(b, reassociation));
  build(b, result, resultType, src, attrs);
}

LogicalResult CollapseShapeOp::verify() {
  MemRefType srcType = getSrcType();
  MemRefType resultType = getResultType();

  if (srcType.getRank() < resultType.getRank()) {
    auto r0 = srcType.getRank();
    auto r1 = resultType.getRank();
    return emitOpError("has source rank ")
           << r0 << " and result rank " << r1 << ". This is not a collapse ("
           << r0 << " < " << r1 << ").";
  }

  // Verify result shape.
  if (failed(verifyCollapsedShape(getOperation(), resultType.getShape(),
                                  srcType.getShape(), getReassociationIndices(),
                                  /*allowMultipleDynamicDimsPerGroup=*/true)))
    return failure();

  // Compute expected result type (including layout map).
  MemRefType expectedResultType;
  if (srcType.getLayout().isIdentity()) {
    // If the source is contiguous (i.e., no layout map specified), so is the
    // result.
    MemRefLayoutAttrInterface layout;
    expectedResultType =
        MemRefType::get(resultType.getShape(), srcType.getElementType(), layout,
                        srcType.getMemorySpace());
  } else {
    // Source may not be fully contiguous. Compute the layout map.
    // Note: Dimensions that are collapsed into a single dim are assumed to be
    // contiguous.
    FailureOr<StridedLayoutAttr> computedLayout =
        computeCollapsedLayoutMap(srcType, getReassociationIndices());
    if (failed(computedLayout))
      return emitOpError(
          "invalid source layout map or collapsing non-contiguous dims");
    expectedResultType =
        MemRefType::get(resultType.getShape(), srcType.getElementType(),
                        *computedLayout, srcType.getMemorySpace());
  }

  if (expectedResultType != resultType)
    return emitOpError("expected collapsed type to be ")
           << expectedResultType << " but found " << resultType;

  return success();
}

struct CollapseShapeOpMemRefCastFolder
    : public OpRewritePattern<CollapseShapeOp> {
public:
  using OpRewritePattern<CollapseShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CollapseShapeOp op,
                                PatternRewriter &rewriter) const override {
    auto cast = op.getOperand().getDefiningOp<CastOp>();
    if (!cast)
      return failure();

    if (!CastOp::canFoldIntoConsumerOp(cast))
      return failure();

    Type newResultType = CollapseShapeOp::computeCollapsedType(
        llvm::cast<MemRefType>(cast.getOperand().getType()),
        op.getReassociationIndices());

    if (newResultType == op.getResultType()) {
      rewriter.modifyOpInPlace(
          op, [&]() { op.getSrcMutable().assign(cast.getSource()); });
    } else {
      Value newOp = rewriter.create<CollapseShapeOp>(
          op->getLoc(), cast.getSource(), op.getReassociationIndices());
      rewriter.replaceOpWithNewOp<CastOp>(op, op.getType(), newOp);
    }
    return success();
  }
};

void CollapseShapeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.add<
      ComposeReassociativeReshapeOps<CollapseShapeOp, ReshapeOpKind::kCollapse>,
      ComposeCollapseOfExpandOp<CollapseShapeOp, ExpandShapeOp, CastOp,
                                memref::DimOp, MemRefType>,
      CollapseShapeOpMemRefCastFolder>(context);
}

OpFoldResult ExpandShapeOp::fold(FoldAdaptor adaptor) {
  return foldReshapeOp<ExpandShapeOp, CollapseShapeOp>(*this,
                                                       adaptor.getOperands());
}

OpFoldResult CollapseShapeOp::fold(FoldAdaptor adaptor) {
  return foldReshapeOp<CollapseShapeOp, ExpandShapeOp>(*this,
                                                       adaptor.getOperands());
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

void ReshapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "reshape");
}

LogicalResult ReshapeOp::verify() {
  Type operandType = getSource().getType();
  Type resultType = getResult().getType();

  Type operandElementType =
      llvm::cast<ShapedType>(operandType).getElementType();
  Type resultElementType = llvm::cast<ShapedType>(resultType).getElementType();
  if (operandElementType != resultElementType)
    return emitOpError("element types of source and destination memref "
                       "types should be the same");

  if (auto operandMemRefType = llvm::dyn_cast<MemRefType>(operandType))
    if (!operandMemRefType.getLayout().isIdentity())
      return emitOpError("source memref type should have identity affine map");

  int64_t shapeSize =
      llvm::cast<MemRefType>(getShape().getType()).getDimSize(0);
  auto resultMemRefType = llvm::dyn_cast<MemRefType>(resultType);
  if (resultMemRefType) {
    if (!resultMemRefType.getLayout().isIdentity())
      return emitOpError("result memref type should have identity affine map");
    if (shapeSize == ShapedType::kDynamic)
      return emitOpError("cannot use shape operand with dynamic length to "
                         "reshape to statically-ranked memref type");
    if (shapeSize != resultMemRefType.getRank())
      return emitOpError(
          "length of shape operand differs from the result's memref rank");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// StoreOp
//===----------------------------------------------------------------------===//

LogicalResult StoreOp::verify() {
  if (getNumOperands() != 2 + getMemRefType().getRank())
    return emitOpError("store index operand count not equal to memref rank");

  return success();
}

LogicalResult StoreOp::fold(FoldAdaptor adaptor,
                            SmallVectorImpl<OpFoldResult> &results) {
  /// store(memrefcast) -> store
  return foldMemRefCast(*this, getValueToStore());
}

//===----------------------------------------------------------------------===//
// SubViewOp
//===----------------------------------------------------------------------===//

void SubViewOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "subview");
}

/// A subview result type can be fully inferred from the source type and the
/// static representation of offsets, sizes and strides. Special sentinels
/// encode the dynamic case.
MemRefType SubViewOp::inferResultType(MemRefType sourceMemRefType,
                                      ArrayRef<int64_t> staticOffsets,
                                      ArrayRef<int64_t> staticSizes,
                                      ArrayRef<int64_t> staticStrides) {
  unsigned rank = sourceMemRefType.getRank();
  (void)rank;
  assert(staticOffsets.size() == rank && "staticOffsets length mismatch");
  assert(staticSizes.size() == rank && "staticSizes length mismatch");
  assert(staticStrides.size() == rank && "staticStrides length mismatch");

  // Extract source offset and strides.
  auto [sourceStrides, sourceOffset] = sourceMemRefType.getStridesAndOffset();

  // Compute target offset whose value is:
  //   `sourceOffset + sum_i(staticOffset_i * sourceStrides_i)`.
  int64_t targetOffset = sourceOffset;
  for (auto it : llvm::zip(staticOffsets, sourceStrides)) {
    auto staticOffset = std::get<0>(it), sourceStride = std::get<1>(it);
    targetOffset = (SaturatedInteger::wrap(targetOffset) +
                    SaturatedInteger::wrap(staticOffset) *
                        SaturatedInteger::wrap(sourceStride))
                       .asInteger();
  }

  // Compute target stride whose value is:
  //   `sourceStrides_i * staticStrides_i`.
  SmallVector<int64_t, 4> targetStrides;
  targetStrides.reserve(staticOffsets.size());
  for (auto it : llvm::zip(sourceStrides, staticStrides)) {
    auto sourceStride = std::get<0>(it), staticStride = std::get<1>(it);
    targetStrides.push_back((SaturatedInteger::wrap(sourceStride) *
                             SaturatedInteger::wrap(staticStride))
                                .asInteger());
  }

  // The type is now known.
  return MemRefType::get(staticSizes, sourceMemRefType.getElementType(),
                         StridedLayoutAttr::get(sourceMemRefType.getContext(),
                                                targetOffset, targetStrides),
                         sourceMemRefType.getMemorySpace());
}

MemRefType SubViewOp::inferResultType(MemRefType sourceMemRefType,
                                      ArrayRef<OpFoldResult> offsets,
                                      ArrayRef<OpFoldResult> sizes,
                                      ArrayRef<OpFoldResult> strides) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  if (!hasValidSizesOffsets(staticOffsets))
    return {};
  if (!hasValidSizesOffsets(staticSizes))
    return {};
  if (!hasValidStrides(staticStrides))
    return {};
  return SubViewOp::inferResultType(sourceMemRefType, staticOffsets,
                                    staticSizes, staticStrides);
}

MemRefType SubViewOp::inferRankReducedResultType(
    ArrayRef<int64_t> resultShape, MemRefType sourceRankedTensorType,
    ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
    ArrayRef<int64_t> strides) {
  MemRefType inferredType =
      inferResultType(sourceRankedTensorType, offsets, sizes, strides);
  assert(inferredType.getRank() >= static_cast<int64_t>(resultShape.size()) &&
         "expected ");
  if (inferredType.getRank() == static_cast<int64_t>(resultShape.size()))
    return inferredType;

  // Compute which dimensions are dropped.
  std::optional<llvm::SmallDenseSet<unsigned>> dimsToProject =
      computeRankReductionMask(inferredType.getShape(), resultShape);
  assert(dimsToProject.has_value() && "invalid rank reduction");

  // Compute the layout and result type.
  auto inferredLayout = llvm::cast<StridedLayoutAttr>(inferredType.getLayout());
  SmallVector<int64_t> rankReducedStrides;
  rankReducedStrides.reserve(resultShape.size());
  for (auto [idx, value] : llvm::enumerate(inferredLayout.getStrides())) {
    if (!dimsToProject->contains(idx))
      rankReducedStrides.push_back(value);
  }
  return MemRefType::get(resultShape, inferredType.getElementType(),
                         StridedLayoutAttr::get(inferredLayout.getContext(),
                                                inferredLayout.getOffset(),
                                                rankReducedStrides),
                         inferredType.getMemorySpace());
}

MemRefType SubViewOp::inferRankReducedResultType(
    ArrayRef<int64_t> resultShape, MemRefType sourceRankedTensorType,
    ArrayRef<OpFoldResult> offsets, ArrayRef<OpFoldResult> sizes,
    ArrayRef<OpFoldResult> strides) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  return SubViewOp::inferRankReducedResultType(
      resultShape, sourceRankedTensorType, staticOffsets, staticSizes,
      staticStrides);
}

// Build a SubViewOp with mixed static and dynamic entries and custom result
// type. If the type passed is nullptr, it is inferred.
void SubViewOp::build(OpBuilder &b, OperationState &result,
                      MemRefType resultType, Value source,
                      ArrayRef<OpFoldResult> offsets,
                      ArrayRef<OpFoldResult> sizes,
                      ArrayRef<OpFoldResult> strides,
                      ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  auto sourceMemRefType = llvm::cast<MemRefType>(source.getType());
  // Structuring implementation this way avoids duplication between builders.
  if (!resultType) {
    resultType = SubViewOp::inferResultType(sourceMemRefType, staticOffsets,
                                            staticSizes, staticStrides);
  }
  result.addAttributes(attrs);
  build(b, result, resultType, source, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getDenseI64ArrayAttr(staticOffsets),
        b.getDenseI64ArrayAttr(staticSizes),
        b.getDenseI64ArrayAttr(staticStrides));
}

// Build a SubViewOp with mixed static and dynamic entries and inferred result
// type.
void SubViewOp::build(OpBuilder &b, OperationState &result, Value source,
                      ArrayRef<OpFoldResult> offsets,
                      ArrayRef<OpFoldResult> sizes,
                      ArrayRef<OpFoldResult> strides,
                      ArrayRef<NamedAttribute> attrs) {
  build(b, result, MemRefType(), source, offsets, sizes, strides, attrs);
}

// Build a SubViewOp with static entries and inferred result type.
void SubViewOp::build(OpBuilder &b, OperationState &result, Value source,
                      ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
                      ArrayRef<int64_t> strides,
                      ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  SmallVector<OpFoldResult> sizeValues =
      llvm::to_vector<4>(llvm::map_range(sizes, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  build(b, result, source, offsetValues, sizeValues, strideValues, attrs);
}

// Build a SubViewOp with dynamic entries and custom result type. If the
// type passed is nullptr, it is inferred.
void SubViewOp::build(OpBuilder &b, OperationState &result,
                      MemRefType resultType, Value source,
                      ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
                      ArrayRef<int64_t> strides,
                      ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  SmallVector<OpFoldResult> sizeValues =
      llvm::to_vector<4>(llvm::map_range(sizes, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [&](int64_t v) -> OpFoldResult {
        return b.getI64IntegerAttr(v);
      }));
  build(b, result, resultType, source, offsetValues, sizeValues, strideValues,
        attrs);
}

// Build a SubViewOp with dynamic entries and custom result type. If the type
// passed is nullptr, it is inferred.
void SubViewOp::build(OpBuilder &b, OperationState &result,
                      MemRefType resultType, Value source, ValueRange offsets,
                      ValueRange sizes, ValueRange strides,
                      ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, resultType, source, offsetValues, sizeValues, strideValues);
}

// Build a SubViewOp with dynamic entries and inferred result type.
void SubViewOp::build(OpBuilder &b, OperationState &result, Value source,
                      ValueRange offsets, ValueRange sizes, ValueRange strides,
                      ArrayRef<NamedAttribute> attrs) {
  build(b, result, MemRefType(), source, offsets, sizes, strides, attrs);
}

/// For ViewLikeOpInterface.
Value SubViewOp::getViewSource() { return getSource(); }

/// Return true if `t1` and `t2` have equal offsets (both dynamic or of same
/// static value).
static bool haveCompatibleOffsets(MemRefType t1, MemRefType t2) {
  int64_t t1Offset, t2Offset;
  SmallVector<int64_t> t1Strides, t2Strides;
  auto res1 = t1.getStridesAndOffset(t1Strides, t1Offset);
  auto res2 = t2.getStridesAndOffset(t2Strides, t2Offset);
  return succeeded(res1) && succeeded(res2) && t1Offset == t2Offset;
}

/// Return true if `t1` and `t2` have equal strides (both dynamic or of same
/// static value). Dimensions of `t1` may be dropped in `t2`; these must be
/// marked as dropped in `droppedDims`.
static bool haveCompatibleStrides(MemRefType t1, MemRefType t2,
                                  const llvm::SmallBitVector &droppedDims) {
  assert(size_t(t1.getRank()) == droppedDims.size() &&
         "incorrect number of bits");
  assert(size_t(t1.getRank() - t2.getRank()) == droppedDims.count() &&
         "incorrect number of dropped dims");
  int64_t t1Offset, t2Offset;
  SmallVector<int64_t> t1Strides, t2Strides;
  auto res1 = t1.getStridesAndOffset(t1Strides, t1Offset);
  auto res2 = t2.getStridesAndOffset(t2Strides, t2Offset);
  if (failed(res1) || failed(res2))
    return false;
  for (int64_t i = 0, j = 0, e = t1.getRank(); i < e; ++i) {
    if (droppedDims[i])
      continue;
    if (t1Strides[i] != t2Strides[j])
      return false;
    ++j;
  }
  return true;
}

static LogicalResult produceSubViewErrorMsg(SliceVerificationResult result,
                                            SubViewOp op, Type expectedType) {
  auto memrefType = llvm::cast<ShapedType>(expectedType);
  switch (result) {
  case SliceVerificationResult::Success:
    return success();
  case SliceVerificationResult::RankTooLarge:
    return op->emitError("expected result rank to be smaller or equal to ")
           << "the source rank, but got " << op.getType();
  case SliceVerificationResult::SizeMismatch:
    return op->emitError("expected result type to be ")
           << expectedType
           << " or a rank-reduced version. (mismatch of result sizes), but got "
           << op.getType();
  case SliceVerificationResult::ElemTypeMismatch:
    return op->emitError("expected result element type to be ")
           << memrefType.getElementType() << ", but got " << op.getType();
  case SliceVerificationResult::MemSpaceMismatch:
    return op->emitError(
               "expected result and source memory spaces to match, but got ")
           << op.getType();
  case SliceVerificationResult::LayoutMismatch:
    return op->emitError("expected result type to be ")
           << expectedType
           << " or a rank-reduced version. (mismatch of result layout), but "
              "got "
           << op.getType();
  }
  llvm_unreachable("unexpected subview verification result");
}

/// Verifier for SubViewOp.
LogicalResult SubViewOp::verify() {
  MemRefType baseType = getSourceType();
  MemRefType subViewType = getType();
  ArrayRef<int64_t> staticOffsets = getStaticOffsets();
  ArrayRef<int64_t> staticSizes = getStaticSizes();
  ArrayRef<int64_t> staticStrides = getStaticStrides();

  // The base memref and the view memref should be in the same memory space.
  if (baseType.getMemorySpace() != subViewType.getMemorySpace())
    return emitError("different memory spaces specified for base memref "
                     "type ")
           << baseType << " and subview memref type " << subViewType;

  // Verify that the base memref type has a strided layout map.
  if (!baseType.isStrided())
    return emitError("base type ") << baseType << " is not strided";

  // Compute the expected result type, assuming that there are no rank
  // reductions.
  MemRefType expectedType = SubViewOp::inferResultType(
      baseType, staticOffsets, staticSizes, staticStrides);

  // Verify all properties of a shaped type: rank, element type and dimension
  // sizes. This takes into account potential rank reductions.
  auto shapedTypeVerification = isRankReducedType(
      /*originalType=*/expectedType, /*candidateReducedType=*/subViewType);
  if (shapedTypeVerification != SliceVerificationResult::Success)
    return produceSubViewErrorMsg(shapedTypeVerification, *this, expectedType);

  // Make sure that the memory space did not change.
  if (expectedType.getMemorySpace() != subViewType.getMemorySpace())
    return produceSubViewErrorMsg(SliceVerificationResult::MemSpaceMismatch,
                                  *this, expectedType);

  // Verify the offset of the layout map.
  if (!haveCompatibleOffsets(expectedType, subViewType))
    return produceSubViewErrorMsg(SliceVerificationResult::LayoutMismatch,
                                  *this, expectedType);

  // The only thing that's left to verify now are the strides. First, compute
  // the unused dimensions due to rank reductions. We have to look at sizes and
  // strides to decide which dimensions were dropped. This function also
  // partially verifies strides in case of rank reductions.
  auto unusedDims = computeMemRefRankReductionMask(expectedType, subViewType,
                                                   getMixedSizes());
  if (failed(unusedDims))
    return produceSubViewErrorMsg(SliceVerificationResult::LayoutMismatch,
                                  *this, expectedType);

  // Strides must match.
  if (!haveCompatibleStrides(expectedType, subViewType, *unusedDims))
    return produceSubViewErrorMsg(SliceVerificationResult::LayoutMismatch,
                                  *this, expectedType);

  // Verify that offsets, sizes, strides do not run out-of-bounds with respect
  // to the base memref.
  SliceBoundsVerificationResult boundsResult =
      verifyInBoundsSlice(baseType.getShape(), staticOffsets, staticSizes,
                          staticStrides, /*generateErrorMessage=*/true);
  if (!boundsResult.isValid)
    return getOperation()->emitError(boundsResult.errorMessage);

  return success();
}

raw_ostream &mlir::operator<<(raw_ostream &os, const Range &range) {
  return os << "range " << range.offset << ":" << range.size << ":"
            << range.stride;
}

/// Return the list of Range (i.e. offset, size, stride). Each Range
/// entry contains either the dynamic value or a ConstantIndexOp constructed
/// with `b` at location `loc`.
SmallVector<Range, 8> mlir::getOrCreateRanges(OffsetSizeAndStrideOpInterface op,
                                              OpBuilder &b, Location loc) {
  std::array<unsigned, 3> ranks = op.getArrayAttrMaxRanks();
  assert(ranks[0] == ranks[1] && "expected offset and sizes of equal ranks");
  assert(ranks[1] == ranks[2] && "expected sizes and strides of equal ranks");
  SmallVector<Range, 8> res;
  unsigned rank = ranks[0];
  res.reserve(rank);
  for (unsigned idx = 0; idx < rank; ++idx) {
    Value offset =
        op.isDynamicOffset(idx)
            ? op.getDynamicOffset(idx)
            : b.create<arith::ConstantIndexOp>(loc, op.getStaticOffset(idx));
    Value size =
        op.isDynamicSize(idx)
            ? op.getDynamicSize(idx)
            : b.create<arith::ConstantIndexOp>(loc, op.getStaticSize(idx));
    Value stride =
        op.isDynamicStride(idx)
            ? op.getDynamicStride(idx)
            : b.create<arith::ConstantIndexOp>(loc, op.getStaticStride(idx));
    res.emplace_back(Range{offset, size, stride});
  }
  return res;
}

/// Compute the canonical result type of a SubViewOp. Call `inferResultType`
/// to deduce the result type for the given `sourceType`. Additionally, reduce
/// the rank of the inferred result type if `currentResultType` is lower rank
/// than `currentSourceType`. Use this signature if `sourceType` is updated
/// together with the result type. In this case, it is important to compute
/// the dropped dimensions using `currentSourceType` whose strides align with
/// `currentResultType`.
static MemRefType getCanonicalSubViewResultType(
    MemRefType currentResultType, MemRefType currentSourceType,
    MemRefType sourceType, ArrayRef<OpFoldResult> mixedOffsets,
    ArrayRef<OpFoldResult> mixedSizes, ArrayRef<OpFoldResult> mixedStrides) {
  MemRefType nonRankReducedType = SubViewOp::inferResultType(
      sourceType, mixedOffsets, mixedSizes, mixedStrides);
  FailureOr<llvm::SmallBitVector> unusedDims = computeMemRefRankReductionMask(
      currentSourceType, currentResultType, mixedSizes);
  if (failed(unusedDims))
    return nullptr;

  auto layout = llvm::cast<StridedLayoutAttr>(nonRankReducedType.getLayout());
  SmallVector<int64_t> shape, strides;
  unsigned numDimsAfterReduction =
      nonRankReducedType.getRank() - unusedDims->count();
  shape.reserve(numDimsAfterReduction);
  strides.reserve(numDimsAfterReduction);
  for (const auto &[idx, size, stride] :
       llvm::zip(llvm::seq<unsigned>(0, nonRankReducedType.getRank()),
                 nonRankReducedType.getShape(), layout.getStrides())) {
    if (unusedDims->test(idx))
      continue;
    shape.push_back(size);
    strides.push_back(stride);
  }

  return MemRefType::get(shape, nonRankReducedType.getElementType(),
                         StridedLayoutAttr::get(sourceType.getContext(),
                                                layout.getOffset(), strides),
                         nonRankReducedType.getMemorySpace());
}

Value mlir::memref::createCanonicalRankReducingSubViewOp(
    OpBuilder &b, Location loc, Value memref, ArrayRef<int64_t> targetShape) {
  auto memrefType = llvm::cast<MemRefType>(memref.getType());
  unsigned rank = memrefType.getRank();
  SmallVector<OpFoldResult> offsets(rank, b.getIndexAttr(0));
  SmallVector<OpFoldResult> sizes = getMixedSizes(b, loc, memref);
  SmallVector<OpFoldResult> strides(rank, b.getIndexAttr(1));
  MemRefType targetType = SubViewOp::inferRankReducedResultType(
      targetShape, memrefType, offsets, sizes, strides);
  return b.createOrFold<memref::SubViewOp>(loc, targetType, memref, offsets,
                                           sizes, strides);
}

FailureOr<Value> SubViewOp::rankReduceIfNeeded(OpBuilder &b, Location loc,
                                               Value value,
                                               ArrayRef<int64_t> desiredShape) {
  auto sourceMemrefType = llvm::dyn_cast<MemRefType>(value.getType());
  assert(sourceMemrefType && "not a ranked memref type");
  auto sourceShape = sourceMemrefType.getShape();
  if (sourceShape.equals(desiredShape))
    return value;
  auto maybeRankReductionMask =
      mlir::computeRankReductionMask(sourceShape, desiredShape);
  if (!maybeRankReductionMask)
    return failure();
  return createCanonicalRankReducingSubViewOp(b, loc, value, desiredShape);
}

/// Helper method to check if a `subview` operation is trivially a no-op. This
/// is the case if the all offsets are zero, all strides are 1, and the source
/// shape is same as the size of the subview. In such cases, the subview can
/// be folded into its source.
static bool isTrivialSubViewOp(SubViewOp subViewOp) {
  if (subViewOp.getSourceType().getRank() != subViewOp.getType().getRank())
    return false;

  auto mixedOffsets = subViewOp.getMixedOffsets();
  auto mixedSizes = subViewOp.getMixedSizes();
  auto mixedStrides = subViewOp.getMixedStrides();

  // Check offsets are zero.
  if (llvm::any_of(mixedOffsets, [](OpFoldResult ofr) {
        std::optional<int64_t> intValue = getConstantIntValue(ofr);
        return !intValue || intValue.value() != 0;
      }))
    return false;

  // Check strides are one.
  if (llvm::any_of(mixedStrides, [](OpFoldResult ofr) {
        std::optional<int64_t> intValue = getConstantIntValue(ofr);
        return !intValue || intValue.value() != 1;
      }))
    return false;

  // Check all size values are static and matches the (static) source shape.
  ArrayRef<int64_t> sourceShape = subViewOp.getSourceType().getShape();
  for (const auto &size : llvm::enumerate(mixedSizes)) {
    std::optional<int64_t> intValue = getConstantIntValue(size.value());
    if (!intValue || *intValue != sourceShape[size.index()])
      return false;
  }
  // All conditions met. The `SubViewOp` is foldable as a no-op.
  return true;
}

namespace {
/// Pattern to rewrite a subview op with MemRefCast arguments.
/// This essentially pushes memref.cast past its consuming subview when
/// `canFoldIntoConsumerOp` is true.
///
/// Example:
/// ```
///   %0 = memref.cast %V : memref<16x16xf32> to memref<?x?xf32>
///   %1 = memref.subview %0[0, 0][3, 4][1, 1] :
///     memref<?x?xf32> to memref<3x4xf32, strided<[?, 1], offset: ?>>
/// ```
/// is rewritten into:
/// ```
///   %0 = memref.subview %V: memref<16x16xf32> to memref<3x4xf32, #[[map0]]>
///   %1 = memref.cast %0: memref<3x4xf32, strided<[16, 1], offset: 0>> to
///     memref<3x4xf32, strided<[?, 1], offset: ?>>
/// ```
class SubViewOpMemRefCastFolder final : public OpRewritePattern<SubViewOp> {
public:
  using OpRewritePattern<SubViewOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SubViewOp subViewOp,
                                PatternRewriter &rewriter) const override {
    // Any constant operand, just return to let SubViewOpConstantFolder kick
    // in.
    if (llvm::any_of(subViewOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    auto castOp = subViewOp.getSource().getDefiningOp<CastOp>();
    if (!castOp)
      return failure();

    if (!CastOp::canFoldIntoConsumerOp(castOp))
      return failure();

    // Compute the SubViewOp result type after folding the MemRefCastOp. Use
    // the MemRefCastOp source operand type to infer the result type and the
    // current SubViewOp source operand type to compute the dropped dimensions
    // if the operation is rank-reducing.
    auto resultType = getCanonicalSubViewResultType(
        subViewOp.getType(), subViewOp.getSourceType(),
        llvm::cast<MemRefType>(castOp.getSource().getType()),
        subViewOp.getMixedOffsets(), subViewOp.getMixedSizes(),
        subViewOp.getMixedStrides());
    if (!resultType)
      return failure();

    Value newSubView = rewriter.create<SubViewOp>(
        subViewOp.getLoc(), resultType, castOp.getSource(),
        subViewOp.getOffsets(), subViewOp.getSizes(), subViewOp.getStrides(),
        subViewOp.getStaticOffsets(), subViewOp.getStaticSizes(),
        subViewOp.getStaticStrides());
    rewriter.replaceOpWithNewOp<CastOp>(subViewOp, subViewOp.getType(),
                                        newSubView);
    return success();
  }
};

/// Canonicalize subview ops that are no-ops. When the source shape is not
/// same as a result shape due to use of `affine_map`.
class TrivialSubViewOpFolder final : public OpRewritePattern<SubViewOp> {
public:
  using OpRewritePattern<SubViewOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SubViewOp subViewOp,
                                PatternRewriter &rewriter) const override {
    if (!isTrivialSubViewOp(subViewOp))
      return failure();
    if (subViewOp.getSourceType() == subViewOp.getType()) {
      rewriter.replaceOp(subViewOp, subViewOp.getSource());
      return success();
    }
    rewriter.replaceOpWithNewOp<CastOp>(subViewOp, subViewOp.getType(),
                                        subViewOp.getSource());
    return success();
  }
};
} // namespace

/// Return the canonical type of the result of a subview.
struct SubViewReturnTypeCanonicalizer {
  MemRefType operator()(SubViewOp op, ArrayRef<OpFoldResult> mixedOffsets,
                        ArrayRef<OpFoldResult> mixedSizes,
                        ArrayRef<OpFoldResult> mixedStrides) {
    // Infer a memref type without taking into account any rank reductions.
    MemRefType resTy = SubViewOp::inferResultType(
        op.getSourceType(), mixedOffsets, mixedSizes, mixedStrides);
    if (!resTy)
      return {};
    MemRefType nonReducedType = resTy;

    // Directly return the non-rank reduced type if there are no dropped dims.
    llvm::SmallBitVector droppedDims = op.getDroppedDims();
    if (droppedDims.none())
      return nonReducedType;

    // Take the strides and offset from the non-rank reduced type.
    auto [nonReducedStrides, offset] = nonReducedType.getStridesAndOffset();

    // Drop dims from shape and strides.
    SmallVector<int64_t> targetShape;
    SmallVector<int64_t> targetStrides;
    for (int64_t i = 0; i < static_cast<int64_t>(mixedSizes.size()); ++i) {
      if (droppedDims.test(i))
        continue;
      targetStrides.push_back(nonReducedStrides[i]);
      targetShape.push_back(nonReducedType.getDimSize(i));
    }

    return MemRefType::get(targetShape, nonReducedType.getElementType(),
                           StridedLayoutAttr::get(nonReducedType.getContext(),
                                                  offset, targetStrides),
                           nonReducedType.getMemorySpace());
  }
};

/// A canonicalizer wrapper to replace SubViewOps.
struct SubViewCanonicalizer {
  void operator()(PatternRewriter &rewriter, SubViewOp op, SubViewOp newOp) {
    rewriter.replaceOpWithNewOp<CastOp>(op, op.getType(), newOp);
  }
};

void SubViewOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results
      .add<OpWithOffsetSizesAndStridesConstantArgumentFolder<
               SubViewOp, SubViewReturnTypeCanonicalizer, SubViewCanonicalizer>,
           SubViewOpMemRefCastFolder, TrivialSubViewOpFolder>(context);
}

OpFoldResult SubViewOp::fold(FoldAdaptor adaptor) {
  MemRefType sourceMemrefType = getSource().getType();
  MemRefType resultMemrefType = getResult().getType();
  auto resultLayout =
      dyn_cast_if_present<StridedLayoutAttr>(resultMemrefType.getLayout());

  if (resultMemrefType == sourceMemrefType &&
      resultMemrefType.hasStaticShape() &&
      (!resultLayout || resultLayout.hasStaticLayout())) {
    return getViewSource();
  }

  // Fold subview(subview(x)), where both subviews have the same size and the
  // second subview's offsets are all zero. (I.e., the second subview is a
  // no-op.)
  if (auto srcSubview = getViewSource().getDefiningOp<SubViewOp>()) {
    auto srcSizes = srcSubview.getMixedSizes();
    auto sizes = getMixedSizes();
    auto offsets = getMixedOffsets();
    bool allOffsetsZero = llvm::all_of(offsets, isZeroInteger);
    auto strides = getMixedStrides();
    bool allStridesOne = llvm::all_of(strides, isOneInteger);
    bool allSizesSame = llvm::equal(sizes, srcSizes);
    if (allOffsetsZero && allStridesOne && allSizesSame &&
        resultMemrefType == sourceMemrefType)
      return getViewSource();
  }

  return {};
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

void TransposeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "transpose");
}

/// Build a strided memref type by applying `permutationMap` to `memRefType`.
static MemRefType inferTransposeResultType(MemRefType memRefType,
                                           AffineMap permutationMap) {
  auto originalSizes = memRefType.getShape();
  auto [originalStrides, offset] = memRefType.getStridesAndOffset();
  assert(originalStrides.size() == static_cast<unsigned>(memRefType.getRank()));

  // Compute permuted sizes and strides.
  auto sizes = applyPermutationMap<int64_t>(permutationMap, originalSizes);
  auto strides = applyPermutationMap<int64_t>(permutationMap, originalStrides);

  return MemRefType::Builder(memRefType)
      .setShape(sizes)
      .setLayout(
          StridedLayoutAttr::get(memRefType.getContext(), offset, strides));
}

void TransposeOp::build(OpBuilder &b, OperationState &result, Value in,
                        AffineMapAttr permutation,
                        ArrayRef<NamedAttribute> attrs) {
  auto permutationMap = permutation.getValue();
  assert(permutationMap);

  auto memRefType = llvm::cast<MemRefType>(in.getType());
  // Compute result type.
  MemRefType resultType = inferTransposeResultType(memRefType, permutationMap);

  result.addAttribute(TransposeOp::getPermutationAttrStrName(), permutation);
  build(b, result, resultType, in, attrs);
}

// transpose $in $permutation attr-dict : type($in) `to` type(results)
void TransposeOp::print(OpAsmPrinter &p) {
  p << " " << getIn() << " " << getPermutation();
  p.printOptionalAttrDict((*this)->getAttrs(), {getPermutationAttrStrName()});
  p << " : " << getIn().getType() << " to " << getType();
}

ParseResult TransposeOp::parse(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::UnresolvedOperand in;
  AffineMap permutation;
  MemRefType srcType, dstType;
  if (parser.parseOperand(in) || parser.parseAffineMap(permutation) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(srcType) ||
      parser.resolveOperand(in, srcType, result.operands) ||
      parser.parseKeywordType("to", dstType) ||
      parser.addTypeToList(dstType, result.types))
    return failure();

  result.addAttribute(TransposeOp::getPermutationAttrStrName(),
                      AffineMapAttr::get(permutation));
  return success();
}

LogicalResult TransposeOp::verify() {
  if (!getPermutation().isPermutation())
    return emitOpError("expected a permutation map");
  if (getPermutation().getNumDims() != getIn().getType().getRank())
    return emitOpError("expected a permutation map of same rank as the input");

  auto srcType = llvm::cast<MemRefType>(getIn().getType());
  auto resultType = llvm::cast<MemRefType>(getType());
  auto canonicalResultType = inferTransposeResultType(srcType, getPermutation())
                                 .canonicalizeStridedLayout();

  if (resultType.canonicalizeStridedLayout() != canonicalResultType)
    return emitOpError("result type ")
           << resultType
           << " is not equivalent to the canonical transposed input type "
           << canonicalResultType;
  return success();
}

OpFoldResult TransposeOp::fold(FoldAdaptor) {
  // First check for identity permutation, we can fold it away if input and
  // result types are identical already.
  if (getPermutation().isIdentity() && getType() == getIn().getType())
    return getIn();
  // Fold two consecutive memref.transpose Ops into one by composing their
  // permutation maps.
  if (auto otherTransposeOp = getIn().getDefiningOp<memref::TransposeOp>()) {
    AffineMap composedPermutation =
        getPermutation().compose(otherTransposeOp.getPermutation());
    getInMutable().assign(otherTransposeOp.getIn());
    setPermutation(composedPermutation);
    return getResult();
  }
  return {};
}

//===----------------------------------------------------------------------===//
// ViewOp
//===----------------------------------------------------------------------===//

void ViewOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "view");
}

LogicalResult ViewOp::verify() {
  auto baseType = llvm::cast<MemRefType>(getOperand(0).getType());
  auto viewType = getType();

  // The base memref should have identity layout map (or none).
  if (!baseType.getLayout().isIdentity())
    return emitError("unsupported map for base memref type ") << baseType;

  // The result memref should have identity layout map (or none).
  if (!viewType.getLayout().isIdentity())
    return emitError("unsupported map for result memref type ") << viewType;

  // The base memref and the view memref should be in the same memory space.
  if (baseType.getMemorySpace() != viewType.getMemorySpace())
    return emitError("different memory spaces specified for base memref "
                     "type ")
           << baseType << " and view memref type " << viewType;

  // Verify that we have the correct number of sizes for the result type.
  unsigned numDynamicDims = viewType.getNumDynamicDims();
  if (getSizes().size() != numDynamicDims)
    return emitError("incorrect number of size operands for type ") << viewType;

  return success();
}

Value ViewOp::getViewSource() { return getSource(); }

OpFoldResult ViewOp::fold(FoldAdaptor adaptor) {
  MemRefType sourceMemrefType = getSource().getType();
  MemRefType resultMemrefType = getResult().getType();

  if (resultMemrefType == sourceMemrefType && resultMemrefType.hasStaticShape())
    return getViewSource();

  return {};
}

namespace {

struct ViewOpShapeFolder : public OpRewritePattern<ViewOp> {
  using OpRewritePattern<ViewOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ViewOp viewOp,
                                PatternRewriter &rewriter) const override {
    // Return if none of the operands are constants.
    if (llvm::none_of(viewOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    // Get result memref type.
    auto memrefType = viewOp.getType();

    // Get offset from old memref view type 'memRefType'.
    int64_t oldOffset;
    SmallVector<int64_t, 4> oldStrides;
    if (failed(memrefType.getStridesAndOffset(oldStrides, oldOffset)))
      return failure();
    assert(oldOffset == 0 && "Expected 0 offset");

    SmallVector<Value, 4> newOperands;

    // Offset cannot be folded into result type.

    // Fold any dynamic dim operands which are produced by a constant.
    SmallVector<int64_t, 4> newShapeConstants;
    newShapeConstants.reserve(memrefType.getRank());

    unsigned dynamicDimPos = 0;
    unsigned rank = memrefType.getRank();
    for (unsigned dim = 0, e = rank; dim < e; ++dim) {
      int64_t dimSize = memrefType.getDimSize(dim);
      // If this is already static dimension, keep it.
      if (ShapedType::isStatic(dimSize)) {
        newShapeConstants.push_back(dimSize);
        continue;
      }
      auto *defOp = viewOp.getSizes()[dynamicDimPos].getDefiningOp();
      if (auto constantIndexOp =
              dyn_cast_or_null<arith::ConstantIndexOp>(defOp)) {
        // Dynamic shape dimension will be folded.
        newShapeConstants.push_back(constantIndexOp.value());
      } else {
        // Dynamic shape dimension not folded; copy operand from old memref.
        newShapeConstants.push_back(dimSize);
        newOperands.push_back(viewOp.getSizes()[dynamicDimPos]);
      }
      dynamicDimPos++;
    }

    // Create new memref type with constant folded dims.
    MemRefType newMemRefType =
        MemRefType::Builder(memrefType).setShape(newShapeConstants);
    // Nothing new, don't fold.
    if (newMemRefType == memrefType)
      return failure();

    // Create new ViewOp.
    auto newViewOp = rewriter.create<ViewOp>(
        viewOp.getLoc(), newMemRefType, viewOp.getOperand(0),
        viewOp.getByteShift(), newOperands);
    // Insert a cast so we have the same type as the old memref type.
    rewriter.replaceOpWithNewOp<CastOp>(viewOp, viewOp.getType(), newViewOp);
    return success();
  }
};

struct ViewOpMemrefCastFolder : public OpRewritePattern<ViewOp> {
  using OpRewritePattern<ViewOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ViewOp viewOp,
                                PatternRewriter &rewriter) const override {
    Value memrefOperand = viewOp.getOperand(0);
    CastOp memrefCastOp = memrefOperand.getDefiningOp<CastOp>();
    if (!memrefCastOp)
      return failure();
    Value allocOperand = memrefCastOp.getOperand();
    AllocOp allocOp = allocOperand.getDefiningOp<AllocOp>();
    if (!allocOp)
      return failure();
    rewriter.replaceOpWithNewOp<ViewOp>(viewOp, viewOp.getType(), allocOperand,
                                        viewOp.getByteShift(),
                                        viewOp.getSizes());
    return success();
  }
};

} // namespace

void ViewOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                         MLIRContext *context) {
  results.add<ViewOpShapeFolder, ViewOpMemrefCastFolder>(context);
}

//===----------------------------------------------------------------------===//
// AtomicRMWOp
//===----------------------------------------------------------------------===//

LogicalResult AtomicRMWOp::verify() {
  if (getMemRefType().getRank() != getNumOperands() - 2)
    return emitOpError(
        "expects the number of subscripts to be equal to memref rank");
  switch (getKind()) {
  case arith::AtomicRMWKind::addf:
  case arith::AtomicRMWKind::maximumf:
  case arith::AtomicRMWKind::minimumf:
  case arith::AtomicRMWKind::mulf:
    if (!llvm::isa<FloatType>(getValue().getType()))
      return emitOpError() << "with kind '"
                           << arith::stringifyAtomicRMWKind(getKind())
                           << "' expects a floating-point type";
    break;
  case arith::AtomicRMWKind::addi:
  case arith::AtomicRMWKind::maxs:
  case arith::AtomicRMWKind::maxu:
  case arith::AtomicRMWKind::mins:
  case arith::AtomicRMWKind::minu:
  case arith::AtomicRMWKind::muli:
  case arith::AtomicRMWKind::ori:
  case arith::AtomicRMWKind::andi:
    if (!llvm::isa<IntegerType>(getValue().getType()))
      return emitOpError() << "with kind '"
                           << arith::stringifyAtomicRMWKind(getKind())
                           << "' expects an integer type";
    break;
  default:
    break;
  }
  return success();
}

OpFoldResult AtomicRMWOp::fold(FoldAdaptor adaptor) {
  /// atomicrmw(memrefcast) -> atomicrmw
  if (succeeded(foldMemRefCast(*this, getValue())))
    return getResult();
  return OpFoldResult();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/MemRef/IR/MemRefOps.cpp.inc"
