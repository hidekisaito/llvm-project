; RUN: llc < %s -mtriple=amdgcn-amd-amdhsa -mcpu=gfx900 -start-before=amdgpu-isel -stop-after=amdgpu-isel | FileCheck %s --check-prefixes=CHECK
; RUN: llc < %s -mtriple=amdgcn-amd-amdhsa -mcpu=gfx900 -start-before=amdgpu-isel -stop-after=amdgpu-isel -enable-new-pm | FileCheck %s --check-prefixes=CHECK

; This caused failure in infinite cycle in Selection DAG (combine) due to missing insert_subvector.
;
; CHECK-LABEL: name: test1
; CHECK: GLOBAL_LOAD_DWORDX4
; CHECK: GLOBAL_LOAD_DWORDX4
; CHECK: GLOBAL_STORE_DWORDX4
define protected amdgpu_kernel void @test1(ptr addrspace(4) %ptr) local_unnamed_addr !kernel_arg_addr_space !0 !kernel_arg_access_qual !1 !kernel_arg_type !2 !kernel_arg_base_type !2 !kernel_arg_type_qual !3 !kernel_arg_name !4 {
entry:
  %tmp = load <3 x i64>, ptr addrspace(4) %ptr, align 16, !invariant.load !5
  %srcA.load2 = extractelement <3 x i64> %tmp, i32 0
  %tmp1 = inttoptr i64 %srcA.load2 to ptr addrspace(1)
  %tmp2 = getelementptr inbounds double, ptr addrspace(1) %tmp1, i64 0
  %tmp4 = load <3 x double>, ptr addrspace(1) %tmp2, align 8, !tbaa !6
  %tmp5 = extractelement <3 x double> %tmp4, i32 1
  %tmp6 = insertelement <3 x double> poison, double %tmp5, i32 1
  %tmp7 = insertelement <3 x double> %tmp6, double poison, i32 2
  %tmp8 = load <3 x double>, ptr addrspace(1) poison, align 8, !tbaa !6
  %tmp9 = extractelement <3 x double> %tmp8, i32 2
  %tmp10 = insertelement <3 x double> poison, double %tmp9, i32 2
  %tmp11 = fcmp olt <3 x double> %tmp10, %tmp7
  %tmp12 = select <3 x i1> %tmp11, <3 x double> zeroinitializer, <3 x double> <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %tmp13 = extractelement <3 x double> %tmp12, i64 1
  %tmp14 = insertelement <2 x double> poison, double %tmp13, i32 1
  store <2 x double> %tmp14, ptr addrspace(1) poison, align 8, !tbaa !6
  ret void
}

; This caused failure in Selection DAG due to lack of insert_subvector implementation.
;
; CHECK-LABEL: name: test2
; CHECK: GLOBAL_LOAD_DWORDX2
; CHECK: GLOBAL_LOAD_DWORDX2
; CHECK: GLOBAL_STORE_DWORDX2
define protected amdgpu_kernel void @test2(ptr addrspace(4) %ptr) local_unnamed_addr !kernel_arg_addr_space !0 !kernel_arg_access_qual !1 !kernel_arg_type !2 !kernel_arg_base_type !2 !kernel_arg_type_qual !3 !kernel_arg_name !4 {
entry:
  %tmp = load <3 x i64>, ptr addrspace(4) %ptr, align 16, !invariant.load !5
  %srcA.load2 = extractelement <3 x i64> %tmp, i32 0
  %tmp1 = inttoptr i64 %srcA.load2 to ptr addrspace(1)
  %tmp2 = getelementptr inbounds double, ptr addrspace(1) %tmp1, i64 0
  %tmp4 = load <3 x double>, ptr addrspace(1) %tmp2, align 8, !tbaa !6
  %tmp5 = extractelement <3 x double> %tmp4, i32 1
  %tmp6 = insertelement <3 x double> poison, double %tmp5, i32 1
  %tmp7 = insertelement <3 x double> %tmp6, double poison, i32 2
  %tmp8 = load <3 x double>, ptr addrspace(1) poison, align 8, !tbaa !6
  %tmp9 = extractelement <3 x double> %tmp8, i32 1
  %tmp10 = insertelement <3 x double> poison, double %tmp9, i32 1
  %tmp11 = insertelement <3 x double> %tmp10, double poison, i32 2
  %tmp12 = fcmp olt <3 x double> %tmp11, %tmp7
  %tmp13 = select <3 x i1> %tmp12, <3 x double> zeroinitializer, <3 x double> <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00>
  %tmp14 = extractelement <3 x double> %tmp13, i64 2
  store double %tmp14, ptr addrspace(1) poison, align 8, !tbaa !6
  ret void
}

!0 = !{i32 1, i32 1, i32 1}
!1 = !{!"none", !"none", !"none"}
!2 = !{!"ptr", !"ptr", !"ptr"}
!3 = !{!"", !"", !""}
!4 = !{!"srcA", !"srcB", !"dst"}
!5 = !{}
!6 = !{!7, !7, i64 0}
!7 = !{!"double", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
