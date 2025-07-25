; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx906 -O0 < %s | FileCheck %s

; Test that source locations (.loc directives) are not added to the code within the prologue.

; Function Attrs: convergent mustprogress nounwind
define hidden void @_ZL3barv() #0 !dbg !1644 {
; CHECK-LABEL: _ZL3barv:
; CHECK:       .Lfunc_begin0:
; CHECK-NEXT:    .file 0 "/tmp" "lane-info.cpp" md5 0x4ab9b75a30baffdf0f6f536a80e3e382
; CHECK-NEXT:    .loc 0 30 0 ; lane-info.cpp:30:0
; CHECK-NEXT:    .cfi_sections .debug_frame
; CHECK-NEXT:    .cfi_startproc
; CHECK-NEXT:  ; %bb.0: ; %entry
; CHECK-NEXT:    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
; CHECK-NEXT:    s_mov_b32 s16, s33
; CHECK-NEXT:    s_mov_b32 s33, s32
; CHECK-NEXT:    s_or_saveexec_b64 s[18:19], -1
; CHECK-NEXT:    buffer_store_dword v40, off, s[0:3], s33 ; 4-byte Folded Spill
; CHECK-NEXT:    s_mov_b64 exec, s[18:19]
; CHECK-NEXT:    v_writelane_b32 v40, s16, 2
; CHECK-NEXT:    s_add_i32 s32, s32, 0x400
; CHECK-NEXT:    v_writelane_b32 v40, s30, 0
; CHECK-NEXT:    v_writelane_b32 v40, s31, 1
; CHECK-NEXT:  .Ltmp0:
; CHECK-NEXT:    .loc 0 31 3 prologue_end ; lane-info.cpp:31:3
; CHECK-NEXT:    s_getpc_b64 s[16:17]
; CHECK-NEXT:    s_add_u32 s16, s16, _ZL13sleep_foreverv@gotpcrel32@lo+4
; CHECK-NEXT:    s_addc_u32 s17, s17, _ZL13sleep_foreverv@gotpcrel32@hi+12
; CHECK-NEXT:    s_load_dwordx2 s[16:17], s[16:17], 0x0
; CHECK-NEXT:    s_mov_b64 s[22:23], s[2:3]
; CHECK-NEXT:    s_mov_b64 s[20:21], s[0:1]
; CHECK-NEXT:    s_mov_b64 s[0:1], s[20:21]
; CHECK-NEXT:    s_mov_b64 s[2:3], s[22:23]
; CHECK-NEXT:    s_waitcnt lgkmcnt(0)
; CHECK-NEXT:    s_swappc_b64 s[30:31], s[16:17]
; CHECK-NEXT:  .Ltmp1:
; CHECK-NEXT:    .loc 0 32 1 ; lane-info.cpp:32:1
; CHECK-NEXT:    v_readlane_b32 s31, v40, 1
; CHECK-NEXT:    v_readlane_b32 s30, v40, 0
; CHECK-NEXT:    s_mov_b32 s32, s33
; CHECK-NEXT:    v_readlane_b32 s4, v40, 2
; CHECK-NEXT:    s_or_saveexec_b64 s[6:7], -1
; CHECK-NEXT:    buffer_load_dword v40, off, s[0:3], s33 ; 4-byte Folded Reload
; CHECK-NEXT:    s_mov_b64 exec, s[6:7]
; CHECK-NEXT:    s_mov_b32 s33, s4
; CHECK-NEXT:    s_waitcnt vmcnt(0)
; CHECK-NEXT:    s_setpc_b64 s[30:31]
; CHECK-NEXT:  .Ltmp2:
entry:
  call void @_ZL13sleep_foreverv(), !dbg !1646
  ret void, !dbg !1647
}

; Function Attrs: convergent nounwind
declare void @_ZL13sleep_foreverv() #0

attributes #0 = { nounwind "frame-pointer"="all" }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!1638, !1639, !1640, !1641}
!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_11, file: !1, producer: "clang version 13.0.0)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "lane-info.cpp", directory: "/tmp", checksumkind: CSK_MD5, checksum: "4ab9b75a30baffdf0f6f536a80e3e382")
!371 = !DISubroutineType(types: !372)
!372 = !{null}
!1638 = !{i32 7, !"Dwarf Version", i32 5}
!1639 = !{i32 2, !"Debug Info Version", i32 3}
!1640 = !{i32 1, !"wchar_size", i32 4}
!1641 = !{i32 7, !"PIC Level", i32 1}
!1644 = distinct !DISubprogram(name: "bar", linkageName: "_ZL3barv", scope: !1, file: !1, line: 29, type: !371, scopeLine: 30, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagLocalToUnit | DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !1645)
!1645 = !{}
!1646 = !DILocation(line: 31, column: 3, scope: !1644)
!1647 = !DILocation(line: 32, column: 1, scope: !1644)
