; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; NOTE: Only %local_3_4 is important here.
;       All other instructions are needed to lure LLVM into executing
;       specific code to trigger a bug.
; RUN: opt < %s -indvars -S | FileCheck %s
define i32 @test() {
; CHECK-LABEL: @test(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[OUTER:%.*]]
; CHECK:       outer:
; CHECK-NEXT:    [[LOCAL_6_6:%.*]] = phi i32 [ 10, [[ENTRY:%.*]] ], [ [[TMP4:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[LOCAL_4_5:%.*]] = phi i32 [ 56587, [[ENTRY]] ], [ 0, [[LATCH]] ]
; CHECK-NEXT:    [[LOCAL_3_4:%.*]] = phi i32 [ 2, [[ENTRY]] ], [ [[TMP4]], [[LATCH]] ]
; CHECK-NEXT:    [[DOTUDIV:%.*]] = udiv i32 [[LOCAL_6_6]], 8361
; CHECK-NEXT:    br label [[INNER:%.*]]
; CHECK:       inner:
; CHECK-NEXT:    [[LOCAL_7_3:%.*]] = phi i32 [ 2, [[OUTER]] ], [ [[TMP3:%.*]], [[INNER]] ]
; CHECK-NEXT:    [[LOCAL_4_5_PN:%.*]] = phi i32 [ [[LOCAL_4_5]], [[OUTER]] ], [ [[TMP2:%.*]], [[INNER]] ]
; CHECK-NEXT:    [[LOCAL_3_31:%.*]] = mul i32 [[LOCAL_4_5_PN]], [[DOTUDIV]]
; CHECK-NEXT:    [[TMP0:%.*]] = mul nuw nsw i32 [[LOCAL_7_3]], [[DOTUDIV]]
; CHECK-NEXT:    [[TMP1:%.*]] = sub i32 [[TMP0]], [[LOCAL_3_4]]
; CHECK-NEXT:    [[TMP2]] = add i32 [[TMP1]], [[LOCAL_3_31]]
; CHECK-NEXT:    [[TMP3]] = add nuw nsw i32 [[LOCAL_7_3]], 1
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp eq i32 [[TMP3]], 6
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[LATCH]], label [[INNER]]
; CHECK:       latch:
; CHECK-NEXT:    [[DOTLCSSA:%.*]] = phi i32 [ [[TMP2]], [[INNER]] ]
; CHECK-NEXT:    [[TMP4]] = add nuw nsw i32 [[LOCAL_6_6]], 1
; CHECK-NEXT:    [[EXITCOND1:%.*]] = icmp eq i32 [[TMP4]], 278
; CHECK-NEXT:    br i1 [[EXITCOND1]], label [[RETURN:%.*]], label [[OUTER]]
; CHECK:       return:
; CHECK-NEXT:    [[DOTLCSSA_LCSSA:%.*]] = phi i32 [ [[DOTLCSSA]], [[LATCH]] ]
; CHECK-NEXT:    ret i32 [[DOTLCSSA_LCSSA]]
;
entry:
  br label %outer

outer:
  %local_6_6 = phi i32 [ 10, %entry ], [ %5, %latch ]
  %local_4_5 = phi i32 [ 56587, %entry ], [ 0, %latch ]
  %local_3_4 = phi i32 [ 2, %entry ], [ %5, %latch ]
  %.udiv = udiv i32 %local_6_6, 8361
  br label %inner

inner:
  %local_7_3 = phi i32 [ 2, %outer ], [ %3, %inner ]
  %local_4_5.pn = phi i32 [ %local_4_5, %outer ], [ %2, %inner ]
  %local_3_31 = mul i32 %local_4_5.pn, %.udiv
  %0 = mul i32 %local_7_3, %.udiv
  %1 = sub i32 %0, %local_3_4
  %2 = add i32 %1, %local_3_31
  %3 = add nuw nsw i32 %local_7_3, 1
  %4 = icmp ugt i32 %local_7_3, 4
  br i1 %4, label %latch, label %inner

latch:
  %.lcssa = phi i32 [ %2, %inner ]
  %5 = add nuw nsw i32 %local_6_6, 1
  %6 = icmp ugt i32 %local_6_6, 276
  br i1 %6, label %return, label %outer

return:
  %.lcssa.lcssa = phi i32 [ %.lcssa, %latch ]
  ret i32 %.lcssa.lcssa

}

