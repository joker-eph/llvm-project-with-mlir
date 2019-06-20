// RUN: mlir-opt %s -split-input-file -test-constant-fold | FileCheck %s

// -----

// CHECK-LABEL: @test(%arg0: memref<f32>) {
func @test(%p : memref<f32>) {
  // CHECK: %cst = constant 6.000000e+00 : f32
  affine.for %i0 = 0 to 128 {
    affine.for %i1 = 0 to 8 { // CHECK: affine.for %i1 = 0 to 8 {
      %0 = constant 4.5 : f32
      %1 = constant 1.5 : f32

      %2 = addf %0, %1 : f32

      // CHECK-NEXT: store %cst, %arg0[]
      store %2, %p[] : memref<f32>
    }
  }
  return
}

// -----

// CHECK-LABEL: func @simple_addf
func @simple_addf() -> f32 {
  %0 = constant 4.5 : f32
  %1 = constant 1.5 : f32

  // CHECK-NEXT: %cst = constant 6.000000e+00 : f32
  %2 = addf %0, %1 : f32

  // CHECK-NEXT: return %cst
  return %2 : f32
}

// -----

// CHECK-LABEL: func @addf_splat_tensor
func @addf_splat_tensor() -> tensor<4xf32> {
  %0 = constant dense<tensor<4xf32>, 4.5>
  %1 = constant dense<tensor<4xf32>, 1.5>

  // CHECK-NEXT: %cst = constant dense<tensor<4xf32>, 6.000000e+00>
  %2 = addf %0, %1 : tensor<4xf32>

  // CHECK-NEXT: return %cst
  return %2 : tensor<4xf32>
}

// -----

// CHECK-LABEL: func @simple_addi
func @simple_addi() -> i32 {
  %0 = constant 1 : i32
  %1 = constant 5 : i32

  // CHECK-NEXT: %c6_i32 = constant 6 : i32
  %2 = addi %0, %1 : i32

  // CHECK-NEXT: return %c6_i32
  return %2 : i32
}

// -----

// CHECK-LABEL: func @addi_splat_vector
func @addi_splat_vector() -> vector<8xi32> {
  %0 = constant dense<vector<8xi32>, 1>
  %1 = constant dense<vector<8xi32>, 5>

  // CHECK-NEXT: %cst = constant dense<vector<8xi32>, 6>
  %2 = addi %0, %1 : vector<8xi32>

  // CHECK-NEXT: return %cst
  return %2 : vector<8xi32>
}

// -----

// CHECK-LABEL: func @simple_subf
func @simple_subf() -> f32 {
  %0 = constant 4.5 : f32
  %1 = constant 1.5 : f32

  // CHECK-NEXT: %cst = constant 3.000000e+00 : f32
  %2 = subf %0, %1 : f32

  // CHECK-NEXT: return %cst
  return %2 : f32
}

// -----

// CHECK-LABEL: func @subf_splat_vector
func @subf_splat_vector() -> vector<4xf32> {
  %0 = constant dense<vector<4xf32>, 4.5>
  %1 = constant dense<vector<4xf32>, 1.5>

  // CHECK-NEXT: %cst = constant dense<vector<4xf32>, 3.000000e+00>
  %2 = subf %0, %1 : vector<4xf32>

  // CHECK-NEXT: return %cst
  return %2 : vector<4xf32>
}

// -----

// CHECK-LABEL: func @simple_subi
func @simple_subi() -> i32 {
  %0 = constant 4 : i32
  %1 = constant 1 : i32

  // CHECK-NEXT: %c3_i32 = constant 3 : i32
  %2 = subi %0, %1 : i32

  // CHECK-NEXT: return %c3_i32
  return %2 : i32
}

// -----

// CHECK-LABEL: func @subi_splat_tensor
func @subi_splat_tensor() -> tensor<4xi32> {
  %0 = constant dense<tensor<4xi32>, 4>
  %1 = constant dense<tensor<4xi32>, 1>

  // CHECK-NEXT: %cst = constant dense<tensor<4xi32>, 3>
  %2 = subi %0, %1 : tensor<4xi32>

  // CHECK-NEXT: return %cst
  return %2 : tensor<4xi32>
}

// -----

// CHECK-LABEL: func @affine_apply
func @affine_apply(%variable : index) -> (index, index, index) {
  %c177 = constant 177 : index
  %c211 = constant 211 : index
  %N = constant 1075 : index

  // CHECK: %c1159 = constant 1159 : index
  // CHECK: %c1152 = constant 1152 : index
  %x0 = affine.apply (d0, d1)[S0] -> ( (d0 + 128 * S0) floordiv 128 + d1 mod 128)
           (%c177, %c211)[%N]
  %x1 = affine.apply (d0, d1)[S0] -> (128 * (S0 ceildiv 128))
           (%c177, %c211)[%N]

  // CHECK: %c42 = constant 42 : index
  %y = affine.apply (d0) -> (42) (%variable)

  // CHECK: return %c1159, %c1152, %c42
  return %x0, %x1, %y : index, index, index
}

// -----

// CHECK-LABEL: func @simple_mulf
func @simple_mulf() -> f32 {
  %0 = constant 4.5 : f32
  %1 = constant 1.5 : f32

  // CHECK-NEXT: %cst = constant 6.750000e+00 : f32
  %2 = mulf %0, %1 : f32

  // CHECK-NEXT: return %cst
  return %2 : f32
}

// -----

// CHECK-LABEL: func @mulf_splat_tensor
func @mulf_splat_tensor() -> tensor<4xf32> {
  %0 = constant dense<tensor<4xf32>, 4.5>
  %1 = constant dense<tensor<4xf32>, 1.5>

  // CHECK-NEXT: %cst = constant dense<tensor<4xf32>, 6.750000e+00>
  %2 = mulf %0, %1 : tensor<4xf32>

  // CHECK-NEXT: return %cst
  return %2 : tensor<4xf32>
}

// -----

// CHECK-LABEL: func @simple_divis
func @simple_divis() -> (i32, i32) {
  %0 = constant 6 : i32
  %1 = constant 2 : i32

  // CHECK-NEXT: %c3_i32 = constant 3 : i32
  %2 = divis %0, %1 : i32

  %3 = constant -2 : i32

  // CHECK-NEXT: %c-3_i32 = constant -3 : i32
  %4 = divis %0, %3 : i32

  // CHECK-NEXT: return %c3_i32, %c-3_i32 : i32, i32
  return %2, %4 : i32, i32
}

// -----

// CHECK-LABEL: func @simple_diviu
func @simple_diviu() -> (i32, i32) {
  %0 = constant 6 : i32
  %1 = constant 2 : i32

  // CHECK-NEXT: %c3_i32 = constant 3 : i32
  %2 = diviu %0, %1 : i32

  %3 = constant -2 : i32

  // Unsigned division interprets -2 as 2^32-2, so the result is 0.
  // CHECK-NEXT: %c0_i32 = constant 0 : i32
  %4 = diviu %0, %3 : i32

  // CHECK-NEXT: return %c3_i32, %c0_i32 : i32, i32
  return %2, %4 : i32, i32
}

// -----

// CHECK-LABEL: func @simple_remis
func @simple_remis(%a : i32) -> (i32, i32, i32) {
  %0 = constant 5 : i32
  %1 = constant 2 : i32
  %2 = constant 1 : i32
  %3 = constant -2 : i32

  // CHECK-NEXT: %c1_i32 = constant 1 : i32
  %4 = remis %0, %1 : i32
  %5 = remis %0, %3 : i32
  // CHECK-NEXT: %c0_i32 = constant 0 : i32
  %6 = remis %a, %2 : i32

  // CHECK-NEXT: return %c1_i32, %c1_i32, %c0_i32 : i32, i32, i32
  return %4, %5, %6 : i32, i32, i32
}

// -----

// CHECK-LABEL: func @simple_remiu
func @simple_remiu(%a : i32) -> (i32, i32, i32) {
  %0 = constant 5 : i32
  %1 = constant 2 : i32
  %2 = constant 1 : i32
  %3 = constant -2 : i32

  // CHECK-DAG: %c1_i32 = constant 1 : i32
  %4 = remiu %0, %1 : i32
  // CHECK-DAG: %c5_i32 = constant 5 : i32
  %5 = remiu %0, %3 : i32
  // CHECK-DAG: %c0_i32 = constant 0 : i32
  %6 = remiu %a, %2 : i32

  // CHECK-NEXT: return %c1_i32, %c5_i32, %c0_i32 : i32, i32, i32
  return %4, %5, %6 : i32, i32, i32
}

// -----

// CHECK-LABEL: func @muli
func @muli() -> i32 {
  %0 = constant 4 : i32
  %1 = constant 2 : i32

  // CHECK-NEXT: %c8_i32 = constant 8 : i32
  %2 = muli %0, %1 : i32

  // CHECK-NEXT: return %c8_i32
  return %2 : i32
}

// -----

// CHECK-LABEL: func @muli_splat_vector
func @muli_splat_vector() -> vector<4xi32> {
  %0 = constant dense<vector<4xi32>, 4>
  %1 = constant dense<vector<4xi32>, 2>

  // CHECK-NEXT: %cst = constant dense<vector<4xi32>, 8>
  %2 = muli %0, %1 : vector<4xi32>

  // CHECK-NEXT: return %cst
  return %2 : vector<4xi32>
}

// CHECK-LABEL: func @dim
func @dim(%x : tensor<8x4xf32>) -> index {

  // CHECK: %c4 = constant 4 : index
  %0 = dim %x, 1 : tensor<8x4xf32>

  // CHECK-NEXT: return %c4
  return %0 : index
}

// -----

// CHECK-LABEL: func @cmpi
func @cmpi() -> (i1, i1, i1, i1, i1, i1, i1, i1, i1, i1) {
  %c42 = constant 42 : i32
  %cm1 = constant -1 : i32
  // CHECK-DAG: %false = constant 0 : i1
  // CHECK-DAG: %true = constant 1 : i1
  // CHECK-NEXT: return %false,
  %0 = cmpi "eq", %c42, %cm1 : i32
  // CHECK-SAME: %true,
  %1 = cmpi "ne", %c42, %cm1 : i32
  // CHECK-SAME: %false,
  %2 = cmpi "slt", %c42, %cm1 : i32
  // CHECK-SAME: %false,
  %3 = cmpi "sle", %c42, %cm1 : i32
  // CHECK-SAME: %true,
  %4 = cmpi "sgt", %c42, %cm1 : i32
  // CHECK-SAME: %true,
  %5 = cmpi "sge", %c42, %cm1 : i32
  // CHECK-SAME: %true,
  %6 = cmpi "ult", %c42, %cm1 : i32
  // CHECK-SAME: %true,
  %7 = cmpi "ule", %c42, %cm1 : i32
  // CHECK-SAME: %false,
  %8 = cmpi "ugt", %c42, %cm1 : i32
  // CHECK-SAME: %false
  %9 = cmpi "uge", %c42, %cm1 : i32
  return %0, %1, %2, %3, %4, %5, %6, %7, %8, %9 : i1, i1, i1, i1, i1, i1, i1, i1, i1, i1
}

// -----

// CHECK-LABEL: func @cmpf_normal_numbers
func @cmpf_normal_numbers() -> (i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1) {
  %c42 = constant 42. : f32
  %cm1 = constant -1. : f32
  // CHECK-DAG: %false = constant 0 : i1
  // CHECK-DAG: %true = constant 1 : i1
  // CHECK-NEXT: return %false,
  %0 = cmpf "false", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %1 = cmpf "oeq", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %2 = cmpf "ogt", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %3 = cmpf "oge", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %4 = cmpf "olt", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %5 = cmpf "ole", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %6 = cmpf "one", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %7 = cmpf "ord", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %8 = cmpf "ueq", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %9 = cmpf "ugt", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %10 = cmpf "uge", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %11 = cmpf "ult", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %12 = cmpf "ule", %c42, %cm1 : f32
  // CHECK-SAME: %true,
  %13 = cmpf "une", %c42, %cm1 : f32
  // CHECK-SAME: %false,
  %14 = cmpf "uno", %c42, %cm1 : f32
  // CHECK-SAME: %true
  %15 = cmpf "true", %c42, %cm1 : f32
  return %0, %1, %2, %3, %4, %5, %6, %7, %8, %9, %10, %11, %12, %13, %14, %15 : i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1, i1
}

// -----

// CHECK-LABEL: func @cmpf_nan
func @cmpf_nans() {
  // TODO(b/122019992) Add tests for nan constant folding when it's possible to
  // have nan constants
  return
}

// -----

// CHECK-LABEL: func @cmpf_inf
func @cmpf_inf() {
  // TODO(b/122019992) Add tests for inf constant folding when it's possible to
  // have inf constants
  return
}

// -----

// CHECK-LABEL: func @fold_extract_element
func @fold_extract_element(%arg0 : index) -> (f32, f16, f16, i32) {
  %const_0 = constant 0 : index
  %const_1 = constant 1 : index
  %const_3 = constant 3 : index

  // Fold an extract into a splat.
  // CHECK-NEXT: {{.*}} = constant 4.500000e+00 : f32
  %0 = constant dense<tensor<4xf32>, 4.5>
  %ext_1 = extract_element %0[%arg0] : tensor<4xf32>

  // Fold an extract into a sparse with a sparse index.
  // CHECK-NEXT: {{.*}} = constant -2.000000e+00 : f16
  %1 = constant sparse<vector<1x1x1xf16>, [[0, 0, 0], [1, 1, 1]],  [-5.0, -2.0]>
  %ext_2 = extract_element %1[%const_1, %const_1, %const_1] : vector<1x1x1xf16>

  // Fold an extract into a sparse with a non sparse index.
  // CHECK-NEXT: {{.*}} = constant 0.000000e+00 : f16
  %2 = constant sparse<vector<1x1x1xf16>, [[1, 1, 1]],  [-2.0]>
  %ext_3 = extract_element %2[%const_0, %const_0, %const_0] : vector<1x1x1xf16>

  // Fold an extract into a dense tensor.
  // CHECK-NEXT: {{.*}} = constant 64 : i32
  %3 = constant dense<tensor<2x1x4xi32>, [[[1, -2, 1, 36]], [[0, 2, -1, 64]]]>
  %ext_4 = extract_element %3[%const_1, %const_0, %const_3] : tensor<2x1x4xi32>

  // CHECK-NEXT: return
  return %ext_1, %ext_2, %ext_3, %ext_4 : f32, f16, f16, i32
}


// CHECK-LABEL: func @fold_rank
func @fold_rank() -> (index) {
  %const_0 = constant dense<tensor<2x1x4xi32>, [[[1, -2, 1, 36]], [[0, 2, -1, 64]]]>

  // Fold a rank into a constant
  // CHECK-NEXT: {{.*}} = constant 3 : index
  %rank_0 = rank %const_0 : tensor<2x1x4xi32>

  // CHECK-NEXT: return
  return %rank_0 : index
}

