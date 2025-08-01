//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if defined(__opencl_c_atomic_order_seq_cst) &&                                \
    defined(__opencl_c_atomic_scope_device)

#include <clc/atomic/clc_atomic_store.h>
#include <clc/opencl/atomic/atomic_store.h>

#define FUNCTION atomic_store
#define __IMPL_FUNCTION __clc_atomic_store
#define __CLC_RETURN_VOID

#define __CLC_BODY <atomic_def.inc>
#include <clc/integer/gentype.inc>

#define __CLC_BODY <atomic_def.inc>
#include <clc/math/gentype.inc>

#endif // defined(__opencl_c_atomic_order_seq_cst) &&
       // defined(__opencl_c_atomic_scope_device)
