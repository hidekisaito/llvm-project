! REQUIRES: x86-registered-target
! RUN: bbc -target x86_64-unknown-linux-gnu --use-desc-for-alloc=false -emit-fir -hlfir=false %s -o - | FileCheck %s

! CHECK-LABEL: func @_QPproduct_test(
! CHECK-SAME: %[[arg0:.*]]: !fir.box<!fir.array<?xi32>>{{.*}}) -> i32
integer function product_test(a)
integer :: a(:)
! CHECK-DAG:  %[[c0:.*]] = arith.constant 0 : index
! CHECK-DAG:  %[[a1:.*]] = fir.absent !fir.box<i1>
! CHECK-DAG: %[[a3:.*]] = fir.convert %[[arg0]] : (!fir.box<!fir.array<?xi32>>) -> !fir.box<none>
! CHECK-DAG:  %[[a5:.*]] = fir.convert %[[c0]] : (index) -> i32
! CHECK-DAG:  %[[a6:.*]] = fir.convert %[[a1]] : (!fir.box<i1>) -> !fir.box<none>
product_test = product(a)
! CHECK:  %{{.*}} = fir.call @_FortranAProductInteger4(%[[a3]], %{{.*}}, %{{.*}}, %[[a5]], %[[a6]]) {{.*}}: (!fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> i32
end function

! CHECK-LABEL: func @_QPproduct_test2(
! CHECK-SAME: %[[arg0:.*]]: !fir.box<!fir.array<?x?xi32>>{{.*}}, %[[arg1:.*]]: !fir.box<!fir.array<?xi32>>
subroutine product_test2(a,r)
integer :: a(:,:)
integer :: r(:)
! CHECK-DAG:  %[[c2_i32:.*]] = arith.constant 2 : i32
! CHECK-DAG:  %[[a0:.*]] = fir.alloca !fir.box<!fir.heap<!fir.array<?xi32>>>
! CHECK-DAG:  %[[a1:.*]] = fir.absent !fir.box<i1>
! CHECK-DAG:  %[[a6:.*]] = fir.convert %[[a0]] : (!fir.ref<!fir.box<!fir.heap<!fir.array<?xi32>>>>) -> !fir.ref<!fir.box<none>>
! CHECK-DAG:  %[[a7:.*]] = fir.convert %[[arg0]] : (!fir.box<!fir.array<?x?xi32>>) -> !fir.box<none>
! CHECK-DAG:  %[[a9:.*]] = fir.convert %[[a1]] : (!fir.box<i1>) -> !fir.box<none>
r = product(a,dim=2)
! CHECK:  fir.call @_FortranAProductDim(%[[a6]], %[[a7]], %[[c2_i32]], %{{.*}}, %{{.*}}, %[[a9]]) {{.*}}: (!fir.ref<!fir.box<none>>, !fir.box<none>, i32, !fir.ref<i8>, i32, !fir.box<none>) -> ()
! CHECK-DAG: %[[a11:.*]] = fir.load %[[a0]] : !fir.ref<!fir.box<!fir.heap<!fir.array<?xi32>>>>
! CHECK-DAG:  %[[a13:.*]] = fir.box_addr %[[a11]] : (!fir.box<!fir.heap<!fir.array<?xi32>>>) -> !fir.heap<!fir.array<?xi32>>
! CHECK-DAG:  fir.freemem %[[a13]]
end subroutine

! CHECK-LABEL: func @_QPproduct_test3(
! CHECK-SAME: %[[arg0:.*]]: !fir.box<!fir.array<?xcomplex<f32>>>{{.*}}) -> complex<f32>
complex function product_test3(a)
complex :: a(:)
! CHECK-DAG:  %[[c0:.*]] = arith.constant 0 : index
! CHECK-DAG:  %[[a0:.*]] = fir.alloca complex<f32>
! CHECK-DAG:  %[[a3:.*]] = fir.absent !fir.box<i1>
! CHECK-DAG:  %[[a6:.*]] = fir.convert %[[arg0]] : (!fir.box<!fir.array<?xcomplex<f32>>>) -> !fir.box<none>
! CHECK-DAG:  %[[a8:.*]] = fir.convert %[[c0]] : (index) -> i32
! CHECK-DAG:  %[[a9:.*]] = fir.convert %[[a3]] : (!fir.box<i1>) -> !fir.box<none>
product_test3 = product(a)
! CHECK:  fir.call @_FortranACppProductComplex4(%[[a0]], %[[a6]], %{{.*}}, %{{.*}}, %[[a8]], %[[a9]]) {{.*}}: (!fir.ref<complex<f32>>, !fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> ()
end function

! CHECK-LABEL: func @_QPproduct_test4(
! CHECK-SAME: %[[arg0:.*]]: !fir.box<!fir.array<?xcomplex<f80>>>{{.*}}) -> complex<f80>
complex(10) function product_test4(x)
complex(10):: x(:)
! CHECK-DAG:  %[[c0:.*]] = arith.constant 0 : index
! CHECK-DAG:  %[[a0:.*]] = fir.alloca complex<f80>
product_test4 = product(x)
! CHECK-DAG: %[[a2:.*]] = fir.absent !fir.box<i1>
! CHECK-DAG: %[[a5:.*]] = fir.convert %[[arg0]] : (!fir.box<!fir.array<?xcomplex<f80>>>) -> !fir.box<none>
! CHECK-DAG:  %[[a7:.*]] = fir.convert %[[c0]] : (index) -> i32
! CHECK-DAG:  %[[a8:.*]] = fir.convert %[[a2]] : (!fir.box<i1>) -> !fir.box<none>
! CHECK: fir.call @_FortranACppProductComplex10(%[[a0]], %[[a5]], %{{.*}}, %{{.*}}, %[[a7]], %{{[0-9]+}}) {{.*}}: (!fir.ref<complex<f80>>, !fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> ()
end

! CHECK-LABEL: func @_QPproduct_test_optional(
! CHECK-SAME:  %[[VAL_0:.*]]: !fir.box<!fir.array<?x!fir.logical<4>>>
real function product_test_optional(mask, x)
real :: x(:)
logical, optional :: mask(:)
product_test_optional = product(x, mask=mask)
! CHECK:  %[[VAL_9:.*]] = fir.convert %[[VAL_0]] : (!fir.box<!fir.array<?x!fir.logical<4>>>) -> !fir.box<none>
! CHECK:  fir.call @_FortranAProductReal4(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %[[VAL_9]]) {{.*}}: (!fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> f32
end function

! CHECK-LABEL: func @_QPproduct_test_optional_2(
! CHECK-SAME:  %[[VAL_0:.*]]: !fir.ref<!fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>>
real function product_test_optional_2(mask, x)
real :: x(:)
logical, pointer :: mask(:)
product_test_optional = product(x, mask=mask)
! CHECK:  %[[VAL_4:.*]] = fir.load %[[VAL_0]] : !fir.ref<!fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>>
! CHECK:  %[[VAL_5:.*]] = fir.box_addr %[[VAL_4]] : (!fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>) -> !fir.ptr<!fir.array<?x!fir.logical<4>>>
! CHECK:  %[[VAL_6:.*]] = fir.convert %[[VAL_5]] : (!fir.ptr<!fir.array<?x!fir.logical<4>>>) -> i64
! CHECK:  %[[VAL_7:.*]] = arith.constant 0 : i64
! CHECK:  %[[VAL_8:.*]] = arith.cmpi ne, %[[VAL_6]], %[[VAL_7]] : i64
! CHECK:  %[[VAL_9:.*]] = fir.load %[[VAL_0]] : !fir.ref<!fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>>
! CHECK:  %[[VAL_10:.*]] = fir.absent !fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>
! CHECK:  %[[VAL_11:.*]] = arith.select %[[VAL_8]], %[[VAL_9]], %[[VAL_10]] : !fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>
! CHECK:  %[[VAL_18:.*]] = fir.convert %[[VAL_11]] : (!fir.box<!fir.ptr<!fir.array<?x!fir.logical<4>>>>) -> !fir.box<none>
! CHECK:  fir.call @_FortranAProductReal4(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %[[VAL_18]]) {{.*}}: (!fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> f32
end function

! CHECK-LABEL: func @_QPproduct_test_optional_3(
! CHECK-SAME:  %[[VAL_0:.*]]: !fir.ref<!fir.array<10x!fir.logical<4>>>
real function product_test_optional_3(mask, x)
real :: x(:)
logical, optional :: mask(10)
product_test_optional = product(x, mask=mask)
! CHECK:  %[[VAL_2:.*]] = arith.constant 10 : index
! CHECK:  %[[VAL_5:.*]] = fir.is_present %[[VAL_0]] : (!fir.ref<!fir.array<10x!fir.logical<4>>>) -> i1
! CHECK:  %[[VAL_6:.*]] = fir.shape %[[VAL_2]] : (index) -> !fir.shape<1>
! CHECK:  %[[VAL_7:.*]] = fir.embox %[[VAL_0]](%[[VAL_6]]) : (!fir.ref<!fir.array<10x!fir.logical<4>>>, !fir.shape<1>) -> !fir.box<!fir.array<10x!fir.logical<4>>>
! CHECK:  %[[VAL_8:.*]] = fir.absent !fir.box<!fir.array<10x!fir.logical<4>>>
! CHECK:  %[[VAL_9:.*]] = arith.select %[[VAL_5]], %[[VAL_7]], %[[VAL_8]] : !fir.box<!fir.array<10x!fir.logical<4>>>
! CHECK:  %[[VAL_18:.*]] = fir.convert %[[VAL_9]] : (!fir.box<!fir.array<10x!fir.logical<4>>>) -> !fir.box<none>
! CHECK:  fir.call @_FortranAProductReal4(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %[[VAL_18]]) {{.*}}: (!fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> f32
end function

! CHECK-LABEL: func @_QPproduct_test_optional_4(
real function product_test_optional_4(x, use_mask)
! Test that local allocatable tracked in local variables
! are dealt as optional argument correctly.
real :: x(:)
logical :: use_mask
logical, allocatable :: mask(:)
if (use_mask) then 
  allocate(mask(size(x, 1)))
  call set_mask(mask)
  ! CHECK: fir.call @_QPset_mask
end if
product_test_optional = product(x, mask=mask)
! CHECK:  %[[VAL_20:.*]] = fir.load %[[VAL_3:.*]] : !fir.ref<!fir.heap<!fir.array<?x!fir.logical<4>>>>
! CHECK:  %[[VAL_21:.*]] = fir.convert %[[VAL_20]] : (!fir.heap<!fir.array<?x!fir.logical<4>>>) -> i64
! CHECK:  %[[VAL_22:.*]] = arith.constant 0 : i64
! CHECK:  %[[VAL_23:.*]] = arith.cmpi ne, %[[VAL_21]], %[[VAL_22]] : i64
! CHECK:  %[[VAL_24:.*]] = fir.load %[[VAL_4:.*]] : !fir.ref<index>
! CHECK:  %[[VAL_25:.*]] = fir.load %[[VAL_5:.*]] : !fir.ref<index>
! CHECK:  %[[VAL_26:.*]] = fir.load %[[VAL_3]] : !fir.ref<!fir.heap<!fir.array<?x!fir.logical<4>>>>
! CHECK:  %[[VAL_27:.*]] = fir.shape_shift %[[VAL_24]], %[[VAL_25]] : (index, index) -> !fir.shapeshift<1>
! CHECK:  %[[VAL_28:.*]] = fir.embox %[[VAL_26]](%[[VAL_27]]) : (!fir.heap<!fir.array<?x!fir.logical<4>>>, !fir.shapeshift<1>) -> !fir.box<!fir.array<?x!fir.logical<4>>>
! CHECK:  %[[VAL_29:.*]] = fir.absent !fir.box<!fir.array<?x!fir.logical<4>>>
! CHECK:  %[[VAL_30:.*]] = arith.select %[[VAL_23]], %[[VAL_28]], %[[VAL_29]] : !fir.box<!fir.array<?x!fir.logical<4>>>
! CHECK:  %[[VAL_37:.*]] = fir.convert %[[VAL_30]] : (!fir.box<!fir.array<?x!fir.logical<4>>>) -> !fir.box<none>
! CHECK:  fir.call @_FortranAProductReal4(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %[[VAL_37]]) {{.*}}: (!fir.box<none>, !fir.ref<i8>, i32, i32, !fir.box<none>) -> f32
end function
