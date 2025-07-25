; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py UTC_ARGS: --extra_scrub --version 5
; RUN: llc < %s -mtriple=nvptx64 -mcpu=sm_70 -mattr=+ptx83 | FileCheck %s
; RUN: %if ptxas-12.3 %{ llc < %s -mtriple=nvptx64 -mcpu=sm_70 -mattr=+ptx83 | %ptxas-verify -arch=sm_70 %}

target triple = "nvptx64-nvidia-cuda"

@value = internal addrspace(1) global i128 0, align 16

define void @test_b128_input_from_const() {
; CHECK-LABEL: test_b128_input_from_const(
; CHECK:       {
; CHECK-NEXT:    .reg .b64 %rd<5>;
; CHECK-NEXT:    .reg .b128 %rq<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    mov.b64 %rd2, 0;
; CHECK-NEXT:    mov.b64 %rd3, 42;
; CHECK-NEXT:    mov.b128 %rq1, {%rd3, %rd2};
; CHECK-NEXT:    mov.b64 %rd4, value;
; CHECK-NEXT:    cvta.global.u64 %rd1, %rd4;
; CHECK-NEXT:    // begin inline asm
; CHECK-NEXT:    { st.b128 [%rd1], %rq1; }
; CHECK-NEXT:    // end inline asm
; CHECK-NEXT:    ret;
  tail call void asm sideeffect "{ st.b128 [$0], $1; }", "l,q"(ptr nonnull addrspacecast (ptr addrspace(1) @value to ptr), i128 42)
  ret void
}

define void @test_b128_input_from_load(ptr nocapture readonly %data) {
; CHECK-LABEL: test_b128_input_from_load(
; CHECK:       {
; CHECK-NEXT:    .reg .b64 %rd<7>;
; CHECK-NEXT:    .reg .b128 %rq<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    ld.param.b64 %rd2, [test_b128_input_from_load_param_0];
; CHECK-NEXT:    cvta.to.global.u64 %rd3, %rd2;
; CHECK-NEXT:    ld.global.v2.b64 {%rd4, %rd5}, [%rd3];
; CHECK-NEXT:    mov.b64 %rd6, value;
; CHECK-NEXT:    cvta.global.u64 %rd1, %rd6;
; CHECK-NEXT:    mov.b128 %rq1, {%rd4, %rd5};
; CHECK-NEXT:    // begin inline asm
; CHECK-NEXT:    { st.b128 [%rd1], %rq1; }
; CHECK-NEXT:    // end inline asm
; CHECK-NEXT:    ret;
  %1 = addrspacecast ptr %data to ptr addrspace(1)
  %2 = load <2 x i64>, ptr addrspace(1) %1, align 16
  %3 = bitcast <2 x i64> %2 to i128
  tail call void asm sideeffect "{ st.b128 [$0], $1; }", "l,q"(ptr nonnull addrspacecast (ptr addrspace(1) @value to ptr), i128 %3)
  ret void
}

define void @test_b128_input_from_select(ptr nocapture readonly %flag) {
; CHECK-LABEL: test_b128_input_from_select(
; CHECK:       {
; CHECK-NEXT:    .reg .pred %p<2>;
; CHECK-NEXT:    .reg .b16 %rs<2>;
; CHECK-NEXT:    .reg .b64 %rd<7>;
; CHECK-NEXT:    .reg .b128 %rq<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    ld.param.b64 %rd2, [test_b128_input_from_select_param_0];
; CHECK-NEXT:    cvta.to.global.u64 %rd3, %rd2;
; CHECK-NEXT:    ld.global.b8 %rs1, [%rd3];
; CHECK-NEXT:    setp.eq.b16 %p1, %rs1, 0;
; CHECK-NEXT:    selp.b64 %rd4, 24, 42, %p1;
; CHECK-NEXT:    mov.b64 %rd5, 0;
; CHECK-NEXT:    mov.b128 %rq1, {%rd4, %rd5};
; CHECK-NEXT:    mov.b64 %rd6, value;
; CHECK-NEXT:    cvta.global.u64 %rd1, %rd6;
; CHECK-NEXT:    // begin inline asm
; CHECK-NEXT:    { st.b128 [%rd1], %rq1; }
; CHECK-NEXT:    // end inline asm
; CHECK-NEXT:    ret;
  %1 = addrspacecast ptr %flag to ptr addrspace(1)
  %2 = load i8, ptr addrspace(1) %1, align 1
  %3 = icmp eq i8 %2, 0
  %4 = select i1 %3, i128 24, i128 42
  tail call void asm sideeffect "{ st.b128 [$0], $1; }", "l,q"(ptr nonnull addrspacecast (ptr addrspace(1) @value to ptr), i128 %4)
  ret void
}

define void @test_store_b128_output() {
; CHECK-LABEL: test_store_b128_output(
; CHECK:       {
; CHECK-NEXT:    .reg .b64 %rd<5>;
; CHECK-NEXT:    .reg .b128 %rq<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    // begin inline asm
; CHECK-NEXT:    { mov.b128 %rq1, 41; }
; CHECK-NEXT:    // end inline asm
; CHECK-NEXT:    mov.b128 {%rd1, %rd2}, %rq1;
; CHECK-NEXT:    add.cc.s64 %rd3, %rd1, 1;
; CHECK-NEXT:    addc.cc.s64 %rd4, %rd2, 0;
; CHECK-NEXT:    st.global.v2.b64 [value], {%rd3, %rd4};
; CHECK-NEXT:    ret;
  %1 = tail call i128 asm "{ mov.b128 $0, 41; }", "=q"()
  %add = add nsw i128 %1, 1
  %2 = bitcast i128 %add to <2 x i64>
  store <2 x i64> %2, ptr addrspace(1) @value, align 16
  ret void
}

define void @test_use_of_b128_output(ptr nocapture readonly %data) {
; CHECK-LABEL: test_use_of_b128_output(
; CHECK:       {
; CHECK-NEXT:    .reg .b64 %rd<9>;
; CHECK-NEXT:    .reg .b128 %rq<3>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    ld.param.b64 %rd1, [test_use_of_b128_output_param_0];
; CHECK-NEXT:    cvta.to.global.u64 %rd2, %rd1;
; CHECK-NEXT:    ld.global.v2.b64 {%rd3, %rd4}, [%rd2];
; CHECK-NEXT:    mov.b128 %rq2, {%rd3, %rd4};
; CHECK-NEXT:    // begin inline asm
; CHECK-NEXT:    { mov.b128 %rq1, %rq2; }
; CHECK-NEXT:    // end inline asm
; CHECK-NEXT:    mov.b128 {%rd5, %rd6}, %rq1;
; CHECK-NEXT:    add.cc.s64 %rd7, %rd5, 1;
; CHECK-NEXT:    addc.cc.s64 %rd8, %rd6, 0;
; CHECK-NEXT:    st.global.v2.b64 [value], {%rd7, %rd8};
; CHECK-NEXT:    ret;
  %1 = addrspacecast ptr %data to ptr addrspace(1)
  %2 = load <2 x i64>, ptr addrspace(1) %1, align 16
  %3 = bitcast <2 x i64> %2 to i128
  %4 = tail call i128 asm "{ mov.b128 $0, $1; }", "=q,q"(i128 %3)
  %add = add nsw i128 %4, 1
  %5 = bitcast i128 %add to <2 x i64>
  store <2 x i64> %5, ptr addrspace(1) @value, align 16
  ret void
}
