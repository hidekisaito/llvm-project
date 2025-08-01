//===-- Unittests for atanf -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/math_macros.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/libc_errno.h"
#include "src/math/atanf.h"
#include "test/UnitTest/FPMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/stdint_proxy.h"

using LlvmLibcAtanfTest = LIBC_NAMESPACE::testing::FPTest<float>;

TEST_F(LlvmLibcAtanfTest, SpecialNumbers) {
  libc_errno = 0;
  EXPECT_FP_EQ_WITH_EXCEPTION(aNaN, LIBC_NAMESPACE::atanf(sNaN), FE_INVALID);
  EXPECT_MATH_ERRNO(0);

  // TODO: Strengthen errno,exception checks and remove these assert macros
  // after new matchers/test fixtures are added
  // https://github.com/llvm/llvm-project/issues/90653
  LIBC_NAMESPACE::fputil::clear_except(FE_ALL_EXCEPT);
  EXPECT_FP_EQ_ALL_ROUNDING(aNaN, LIBC_NAMESPACE::atanf(aNaN));
  // TODO: Uncomment these checks later, RoundingMode affects running
  // tests in this way https://github.com/llvm/llvm-project/issues/90653.
  // EXPECT_FP_EXCEPTION(0);
  EXPECT_MATH_ERRNO(0);

  LIBC_NAMESPACE::fputil::clear_except(FE_ALL_EXCEPT);
  EXPECT_FP_EQ_ALL_ROUNDING(0.0f, LIBC_NAMESPACE::atanf(0.0f));
  // See above TODO
  // EXPECT_FP_EXCEPTION(0);
  EXPECT_MATH_ERRNO(0);

  LIBC_NAMESPACE::fputil::clear_except(FE_ALL_EXCEPT);
  EXPECT_FP_EQ_ALL_ROUNDING(-0.0f, LIBC_NAMESPACE::atanf(-0.0f));
  // See above TODO
  // EXPECT_FP_EXCEPTION(0);
  EXPECT_MATH_ERRNO(0);
}

#ifdef LIBC_TEST_FTZ_DAZ

using namespace LIBC_NAMESPACE::testing;

TEST_F(LlvmLibcAtanfTest, FTZMode) {
  ModifyMXCSR mxcsr(FTZ);

  EXPECT_FP_EQ(0.0f, LIBC_NAMESPACE::atanf(min_denormal));
}

TEST_F(LlvmLibcAtanfTest, DAZMode) {
  ModifyMXCSR mxcsr(DAZ);

  EXPECT_FP_EQ(0.0f, LIBC_NAMESPACE::atanf(min_denormal));
}

TEST_F(LlvmLibcAtanfTest, FTZDAZMode) {
  ModifyMXCSR mxcsr(FTZ | DAZ);

  EXPECT_FP_EQ(0.0f, LIBC_NAMESPACE::atanf(min_denormal));
}

#endif
