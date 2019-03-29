// RUN: mlir-opt %s -vectorize -virtual-vector-size 3 -virtual-vector-size 16 --test-fastest-varying=1 --test-fastest-varying=0 -materialize-vectors -vector-size=8 | FileCheck %s -check-prefix=CHECK

// Capture permutation maps used in vectorization.
// CHECK-DAG: #[[map_proj_d0d1_d1:map[0-9]+]] = (d0, d1) -> (d1)

// vector<3x16xf32> -> vector<8xf32>
// CHECK-DAG: [[MAP0:#.*]] = (d0, d1) -> (d0, d1)
// CHECK-DAG: [[MAP1:#.*]] = (d0, d1) -> (d0, d1 + 8)
// CHECK-DAG: [[MAP2:#.*]] = (d0, d1) -> (d0 + 1, d1)
// CHECK-DAG: [[MAP3:#.*]] = (d0, d1) -> (d0 + 1, d1 + 8)
// CHECK-DAG: [[MAP4:#.*]] = (d0, d1) -> (d0 + 2, d1)
// CHECK-DAG: [[MAP5:#.*]] = (d0, d1) -> (d0 + 2, d1 + 8)
mlfunc @vector_add_2d(%M : index, %N : index) -> f32 {
  %A = alloc (%M, %N) : memref<?x?xf32, 0>
  %B = alloc (%M, %N) : memref<?x?xf32, 0>
  %C = alloc (%M, %N) : memref<?x?xf32, 0>
  %f1 = constant 1.0 : f32
  %f2 = constant 2.0 : f32
  for %i0 = 0 to %M {
    for %i1 = 0 to %N {
      // non-scoped %f1
      store %f1, %A[%i0, %i1] : memref<?x?xf32, 0>
    }
  }
  for %i2 = 0 to %M {
    for %i3 = 0 to %N {
      // non-scoped %f2
      // CHECK does (3x4)x unrolling.
      // CHECK-COUNT-6: {{.*}} = constant splat<vector<8xf32>, 1.000000e+00> : vector<8xf32>
      // CHECK: [[VAL0:%.*]] = affine_apply [[MAP0]]{{.*}}
      // CHECK: vector_transfer_write {{.*}}, [[VAL0]]#0, [[VAL0]]#1 {permutation_map: #[[map_proj_d0d1_d1]]} : vector<8xf32>
      // ... 4 other interleaved affine_apply, vector_transfer_write
      // CHECK: [[VAL5:%.*]] = affine_apply [[MAP5]]{{.*}}
      // CHECK: vector_transfer_write {{.*}}, [[VAL5]]#0, [[VAL5]]#1 {permutation_map: #[[map_proj_d0d1_d1]]} : vector<8xf32>
      store %f2, %B[%i2, %i3] : memref<?x?xf32, 0>
    }
  }
  for %i4 = 0 to %M {
    for %i5 = 0 to %N {
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
