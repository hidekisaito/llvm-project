//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef __CLC_ATOMIC_CLC_ATOMIC_LOAD_H__
#define __CLC_ATOMIC_CLC_ATOMIC_LOAD_H__

#include <clc/internal/clc.h>

#define FUNCTION __clc_atomic_load
#define __CLC_NO_VALUE_ARG

#define __CLC_BODY <clc/atomic/atomic_decl.inc>
#include <clc/integer/gentype.inc>

#define __CLC_BODY <clc/atomic/atomic_decl.inc>
#include <clc/math/gentype.inc>

#undef __CLC_NO_VALUE_ARG
#undef FUNCTION

#endif // __CLC_ATOMIC_CLC_ATOMIC_LOAD_H__
