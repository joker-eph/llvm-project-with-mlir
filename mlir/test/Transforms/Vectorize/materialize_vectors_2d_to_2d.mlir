// RUN: mlir-opt %s -affine-vectorize -virtual-vector-size 3 -virtual-vector-size 32 --test-fastest-varying=1 --test-fastest-varying=0 -affine-materialize-vectors -vector-size=3 -vector-size=16 | FileCheck %s 

// vector<3x32xf32> -> vector<3x16xf32>
// CHECK-DAG: [[ID1:#.*]] = (d0) -> (d0)
// CHECK-DAG: [[ID2:#.*]] = (d0, d1) -> (d0, d1)
// CHECK-DAG: [[D0P16:#.*]] = (d0) -> (d0 + 16)

// CHECK-LABEL: func @vector_add_2d
func @vector_add_2d(%M : index, %N : index) -> f32 {
  %A = alloc (%M, %N) : memref<?x?xf32, 0>
  %B = alloc (%M, %N) : memref<?x?xf32, 0>
  %C = alloc (%M, %N) : memref<?x?xf32, 0>
  %f1 = constant 1.0 : f32
  %f2 = constant 2.0 : f32
  // 2x unroll (jammed by construction).
  // CHECK: affine.for %i0 = 0 to %arg0 step 3 {
  // CHECK-NEXT:   affine.for %i1 = 0 to %arg1 step 32 {
  // CHECK-NEXT:     {{.*}} = constant dense<1.000000e+00> : vector<3x16xf32>
  // CHECK-NEXT:     {{.*}} = constant dense<1.000000e+00> : vector<3x16xf32>
  // CHECK-NEXT:     %[[VAL00:.*]] = affine.apply [[ID1]](%i0)
  // CHECK-NEXT:     %[[VAL01:.*]] = affine.apply [[ID1]](%i1)
  // CHECK-NEXT:     vector.transfer_write {{.*}}, {{.*}}[%[[VAL00]], %[[VAL01]]] {permutation_map: [[ID2]]} : vector<3x16xf32>, memref<?x?xf32>
  // CHECK-NEXT:     %[[VAL10:.*]] = affine.apply [[ID1]](%i0)
  // CHECK-NEXT:     %[[VAL11:.*]] = affine.apply [[D0P16]](%i1)
  // CHECK-NEXT:     vector.transfer_write {{.*}}, {{.*}}[%[[VAL10]], %[[VAL11]]] {permutation_map: [[ID2]]} : vector<3x16xf32>, memref<?x?xf32>
  //
  affine.for %i0 = 0 to %M {
    affine.for %i1 = 0 to %N {
      // non-scoped %f1
      store %f1, %A[%i0, %i1] : memref<?x?xf32, 0>
    }
  }
  // 2x unroll (jammed by construction).
  // CHECK: affine.for %i2 = 0 to %arg0 step 3 {
  // CHECK-NEXT:   affine.for %i3 = 0 to %arg1 step 32 {
  // CHECK-NEXT:     {{.*}} = constant dense<2.000000e+00> : vector<3x16xf32>
  // CHECK-NEXT:     {{.*}} = constant dense<2.000000e+00> : vector<3x16xf32>
  // CHECK-NEXT:     %[[VAL00:.*]] = affine.apply [[ID1]](%i2)
  // CHECK-NEXT:     %[[VAL01:.*]] = affine.apply [[ID1]](%i3)
  // CHECK-NEXT:     vector.transfer_write {{.*}}, {{.*}}[%[[VAL00]], %[[VAL01]]] {permutation_map: [[ID2]]} : vector<3x16xf32>, memref<?x?xf32>
  // CHECK-NEXT:     %[[VAL10:.*]] = affine.apply [[ID1]](%i2)
  // CHECK-NEXT:     %[[VAL11:.*]] = affine.apply [[D0P16]](%i3)
  // CHECK-NEXT:     vector.transfer_write {{.*}}, {{.*}}[%[[VAL10]], %[[VAL11]]] {permutation_map: [[ID2]]} : vector<3x16xf32>, memref<?x?xf32>
  //
  affine.for %i2 = 0 to %M {
    affine.for %i3 = 0 to %N {
      // non-scoped %f2
      store %f2, %B[%i2, %i3] : memref<?x?xf32, 0>
    }
  }
  // 2x unroll (jammed by construction).
  // CHECK: affine.for %i4 = 0 to %arg0 step 3 {
  // CHECK-NEXT:   affine.for %i5 = 0 to %arg1 step 32 {
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = vector.transfer_read
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = vector.transfer_read
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = vector.transfer_read
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = vector.transfer_read
  // CHECK-NEXT:     {{.*}} = addf {{.*}} : vector<3x16xf32>
  // CHECK-NEXT:     {{.*}} = addf {{.*}} : vector<3x16xf32>
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     vector.transfer_write
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     {{.*}} = affine.apply
  // CHECK-NEXT:     vector.transfer_write
  //
  affine.for %i4 = 0 to %M {
    affine.for %i5 = 0 to %N {
      %a5 = load %A[%i4, %i5] : memref<?x?xf32, 0>
      %b5 = load %B[%i4, %i5] : memref<?x?xf32, 0>
      %s5 = addf %a5, %b5 : f32
      store %s5, %C[%i4, %i5] : memref<?x?xf32, 0>
    }
  }
  %c7 = constant 7 : index
  %c42 = constant 42 : index
  %res = load %C[%c7, %c42] : memref<?x?xf32, 0>
  return %res : f32
}
