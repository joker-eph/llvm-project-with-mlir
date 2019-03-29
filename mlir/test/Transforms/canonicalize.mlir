// RUN: mlir-opt %s -canonicalize | FileCheck %s

// CHECK-LABEL: func @test_subi_zero
func @test_subi_zero(%arg0: i32) -> i32 {
  // CHECK-NEXT: %c0_i32 = constant 0 : i32
  // CHECK-NEXT: return %c0
  %y = subi %arg0, %arg0 : i32
  return %y: i32
}

// CHECK-LABEL: func @test_subi_zero_cfg(%arg0: i32)
func @test_subi_zero_cfg(%arg0: i32) -> i32 {
  // CHECK-NEXT: %c0_i32 = constant 0 : i32
  // CHECK-NEXT: return %c0
  %y = subi %arg0, %arg0 : i32
  return %y: i32
}

// CHECK-LABEL: func @dim
func @dim(%arg0: tensor<8x4xf32>) -> index {

  // CHECK: %c4 = constant 4 : index
  %0 = dim %arg0, 1 : tensor<8x4xf32>

  // CHECK-NEXT: return %c4
  return %0 : index
}

// CHECK-LABEL: func @test_commutative
func @test_commutative(%arg0: i32) -> (i32, i32) {
  // CHECK: %c42_i32 = constant 42 : i32
  %c42_i32 = constant 42 : i32
  // CHECK-NEXT: %0 = addi %arg0, %c42_i32 : i32
  %y = addi %c42_i32, %arg0 : i32

  // This should not be swapped.
  // CHECK-NEXT: %1 = subi %c42_i32, %arg0 : i32
  %z = subi %c42_i32, %arg0 : i32

  // CHECK-NEXT: return %0, %1
  return %y, %z: i32, i32
}

// CHECK-LABEL: func @trivial_dce
func @trivial_dce(%arg0: tensor<8x4xf32>) {
  %0 = dim %arg0, 1 : tensor<8x4xf32>
  // CHECK-NEXT: return
  return
}

// CHECK-LABEL: func @addi_zero
func @addi_zero(%arg0: i32) -> i32 {
  // CHECK-NEXT: return %arg0
  %c0_i32 = constant 0 : i32
  %y = addi %c0_i32, %arg0 : i32
  return %y: i32
}

// CHECK-LABEL: func @addi_zero_vector
func @addi_zero_vector(%arg0: vector<4 x i32>) -> vector<4 x i32> {
  // CHECK-NEXT: return %arg0
  %c0_v4i32 = constant splat<vector<4 x i32>, 0> : vector<4 x i32>
  %y = addi %c0_v4i32, %arg0 : vector<4 x i32>
  return %y: vector<4 x i32>
}

// CHECK-LABEL: func @addi_zero_tensor
func @addi_zero_tensor(%arg0: tensor<4 x 5 x i32>) -> tensor<4 x 5 x i32> {
  // CHECK-NEXT: return %arg0
  %c0_t45i32 = constant splat<tensor<4 x 5 x i32>, 0> : tensor<4 x 5 x i32>
  %y = addi %arg0, %c0_t45i32 : tensor<4 x 5 x i32>
  return %y: tensor<4 x 5 x i32>
}

// CHECK-LABEL: func @muli_zero
func @muli_zero(%arg0: i32) -> i32 {
  // CHECK-NEXT: %c0_i32 = constant 0 : i32
  %c0_i32 = constant 0 : i32

  %y = muli %c0_i32, %arg0 : i32

  // CHECK-NEXT: return %c0_i32
  return %y: i32
}

// CHECK-LABEL: func @muli_zero_vector
func @muli_zero_vector(%arg0: vector<4 x i32>) -> vector<4 x i32> {
  // CHECK-NEXT: %cst = constant splat<vector<4xi32>, 0> : vector<4xi32>
  %cst = constant splat<vector<4 x i32>, 0> : vector<4 x i32>

  %y = muli %cst, %arg0 : vector<4 x i32>

  // CHECK-NEXT: return %cst
  return %y: vector<4 x i32>
}

// CHECK-LABEL: func @muli_zero_tensor
func @muli_zero_tensor(%arg0: tensor<4 x 5 x i32>) -> tensor<4 x 5 x i32> {
  // CHECK-NEXT: %cst = constant splat<tensor<4x5xi32>, 0> : tensor<4x5xi32>
  %cst = constant splat<tensor<4 x 5 x i32>, 0> : tensor<4 x 5 x i32>

  %y = muli %arg0, %cst : tensor<4 x 5 x i32>

  // CHECK-NEXT: return %cst
  return %y: tensor<4 x 5 x i32>
}

// CHECK-LABEL: func @muli_one
func @muli_one(%arg0: i32) -> i32 {
  // CHECK-NEXT: return %arg0
  %c0_i32 = constant 1 : i32
  %y = muli %c0_i32, %arg0 : i32
  return %y: i32
}

// CHECK-LABEL: func @muli_one_vector
func @muli_one_vector(%arg0: vector<4 x i32>) -> vector<4 x i32> {
  // CHECK-NEXT: return %arg0
  %c1_v4i32 = constant splat<vector<4 x i32>, 1> : vector<4 x i32>
  %y = muli %c1_v4i32, %arg0 : vector<4 x i32>
  return %y: vector<4 x i32>
}

// CHECK-LABEL: func @muli_one_tensor
func @muli_one_tensor(%arg0: tensor<4 x 5 x i32>) -> tensor<4 x 5 x i32> {
  // CHECK-NEXT: return %arg0
  %c1_t45i32 = constant splat<tensor<4 x 5 x i32>, 1> : tensor<4 x 5 x i32>
  %y = muli %arg0, %c1_t45i32 : tensor<4 x 5 x i32>
  return %y: tensor<4 x 5 x i32>
}

// CHECK-LABEL: func @memref_cast_folding
func @memref_cast_folding(%arg0: memref<4 x f32>, %arg1: f32) -> f32 {
  %1 = memref_cast %arg0 : memref<4xf32> to memref<?xf32>

  // CHECK-NEXT: %c0 = constant 0 : index
  %c0 = constant 0 : index

  // CHECK-NEXT: store %arg1, %arg0[%c0] : memref<4xf32>
  store %arg1, %1[%c0] : memref<?xf32>

  // CHECK-NEXT: %0 = load %arg0[%c0] : memref<4xf32>
  %0 = load %1[%c0] : memref<?xf32>

  // CHECK-NEXT: dealloc %arg0 : memref<4xf32>
  dealloc %1: memref<?xf32>

  // CHECK-NEXT: return %0
  return %0 : f32
}

// CHECK-LABEL: func @alloc_const_fold
func @alloc_const_fold() -> memref<?xf32> {
  // CHECK-NEXT: %0 = alloc() : memref<4xf32>
  %c4 = constant 4 : index
  %a = alloc(%c4) : memref<?xf32>

  // CHECK-NEXT: %1 = memref_cast %0 : memref<4xf32> to memref<?xf32>
  // CHECK-NEXT: return %1 : memref<?xf32>
  return %a : memref<?xf32>
}

// CHECK-LABEL: func @dead_alloc_fold
func @dead_alloc_fold() {
  // CHECK-NEXT: return
  %c4 = constant 4 : index
  %a = alloc(%c4) : memref<?xf32>
  return
}

// CHECK-LABEL: func @dead_dealloc_fold
func @dead_dealloc_fold() {
  // CHECK-NEXT: return
  %a = alloc() : memref<4xf32>
  dealloc %a: memref<4xf32>
  return
}

// CHECK-LABEL: func @dead_dealloc_fold_multi_use
func @dead_dealloc_fold_multi_use(%cond : i1) {
  // CHECK-NEXT: cond_br
  %a = alloc() : memref<4xf32>
  cond_br %cond, ^bb1, ^bb2

  // CHECK-LABEL: bb1:
^bb1:
  // CHECK-NEXT: return
  dealloc %a: memref<4xf32>
  return

  // CHECK-LABEL: bb2:
^bb2:
  // CHECK-NEXT: return
  dealloc %a: memref<4xf32>
  return
}

// CHECK-LABEL: func @dyn_shape_fold(%arg0: index, %arg1: index)
func @dyn_shape_fold(%L : index, %M : index) -> (memref<? x ? x i32>, memref<? x ? x f32>) {
  // CHECK: %c0 = constant 0 : index
  %zero = constant 0 : index
  // The constants below disappear after they propagate into shapes.
  %nine = constant 9 : index
  %N = constant 1024 : index
  %K = constant 512 : index

  // CHECK-NEXT: %0 = alloc(%arg0) : memref<?x1024xf32>
  %a = alloc(%L, %N) : memref<? x ? x f32>

  // CHECK-NEXT: %1 = alloc(%arg1) : memref<4x1024x8x512x?xf32>
  %b = alloc(%N, %K, %M) : memref<4 x ? x 8 x ? x ? x f32>

  // CHECK-NEXT: %2 = alloc() : memref<512x1024xi32>
  %c = alloc(%K, %N) : memref<? x ? x i32>

  // CHECK: affine.for %i0 =
  affine.for %i = 0 to %L {
    // CHECK-NEXT: affine.for %i1 =
    affine.for %j = 0 to 10 {
      // CHECK-NEXT: %4 = load %0[%i0, %i1] : memref<?x1024xf32>
      // CHECK-NEXT: store %4, %1[%c0, %c0, %i0, %i1, %c0] : memref<4x1024x8x512x?xf32>
      %v = load %a[%i, %j] : memref<?x?xf32>
      store %v, %b[%zero, %zero, %i, %j, %zero] : memref<4x?x8x?x?xf32>
    }
  }

  // CHECK: %5 = alloc() : memref<9x9xf32>
  %d = alloc(%nine, %nine) : memref<? x ? x f32>

  return %c, %d : memref<? x ? x i32>, memref<? x ? x f32>
}

// CHECK-LABEL: func @merge_constants
func @merge_constants() -> (index, index) {
  // CHECK-NEXT: %c42 = constant 42 : index
  %0 = constant 42 : index
  %1 = constant 42 : index
  // CHECK-NEXT: return %c42, %c42
  return %0, %1: index, index
}

// CHECK-LABEL: func @hoist_constant
func @hoist_constant(%arg0: memref<8xi32>) {
  // CHECK-NEXT: %c42_i32 = constant 42 : i32
  // CHECK-NEXT: affine.for %i0 = 0 to 8 {
  affine.for %i0 = 0 to 8 {
    // CHECK-NEXT: store %c42_i32, %arg0[%i0]
    %c42_i32 = constant 42 : i32
    store %c42_i32, %arg0[%i0] : memref<8xi32>
  }
  return
}

// CHECK-LABEL: func @const_fold_propagate
func @const_fold_propagate() -> memref<?x?xf32> {
  %VT_i = constant 512 : index

  %VT_i_s = affine.apply (d0) -> (d0 floordiv  8) (%VT_i)
  %VT_k_l = affine.apply (d0) -> (d0 floordiv  16) (%VT_i)

  // CHECK: = alloc() : memref<64x32xf32>
  %Av = alloc(%VT_i_s, %VT_k_l) : memref<?x?xf32>
  return %Av : memref<?x?xf32>
}

// CHECK-LABEL: func @cond_br_folding
func @cond_br_folding(%a : i32) {
  %false_cond = constant 0 : i1
  %true_cond = constant 1 : i1

  // CHECK-NEXT: br ^bb1(%arg0 : i32)
  cond_br %true_cond, ^bb1(%a : i32), ^bb2

^bb1(%x : i32):
  // CHECK: br ^bb2
  cond_br %false_cond, ^bb1(%x : i32), ^bb2

^bb2:
  return
}

// CHECK-LABEL: func @indirect_call_folding
func @indirect_target() {
  return
}

func @indirect_call_folding() {
  // CHECK-NEXT: call @indirect_target() : () -> ()
  // CHECK-NEXT: return
  %indirect_fn = constant @indirect_target : () -> ()
  call_indirect %indirect_fn() : () -> ()
  return
}
