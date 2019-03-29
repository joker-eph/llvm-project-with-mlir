// RUN: mlir-opt %s -dma-generate -canonicalize | FileCheck %s

// Index of the buffer for the second DMA is remapped.
// CHECK-DAG: [[MAP:#map[0-9]+]] = (d0) -> (d0 - 256)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (d0 * 16 + d1)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (d0, d1)
// CHECK-DAG: [[MAP_INDEX_DIFF:#map[0-9]+]] = (d0, d1, d2, d3) -> (d2 - d0, d3 - d1)

// CHECK-LABEL: mlfunc @loop_nest_1d() {
mlfunc @loop_nest_1d() {
  %A = alloc() : memref<256 x f32>
  %B = alloc() : memref<512 x f32>
  %F = alloc() : memref<256 x f32, 1>
  // First DMA buffer.
  // CHECK:  %3 = alloc() : memref<256xf32, 1>
  // Tag for first DMA.
  // CHECK:  %4 = alloc() : memref<1xi32>
  // First DMA transfer.
  // CHECK:  dma_start %0[%c0], %3[%c0], %c256, %4[%c0] : memref<256xf32>, memref<256xf32, 1>, memref<1xi32>
  // CHECK:  dma_wait %4[%c0], %c256 : memref<1xi32>
  // Second DMA buffer.
  // CHECK:  %5 = alloc() : memref<256xf32, 1>
  // Tag for second DMA.
  // CHECK:  %6 = alloc() : memref<1xi32>
  // Second DMA transfer.
  // CHECK:       dma_start %1[%c256], %5[%c0], %c256, %6[%c0] : memref<512xf32>, memref<256xf32, 1>, memref<1xi32>
  // CHECK-NEXT:  dma_wait %6[%c0], %c256 : memref<1xi32>
  // CHECK: for %i0 = 0 to 256 {
      // CHECK:      %7 = affine_apply #map{{[0-9]+}}(%i0)
      // CHECK-NEXT: %8 = load %3[%7] : memref<256xf32, 1>
      // CHECK:      %9 = affine_apply #map{{[0-9]+}}(%i0)
      // CHECK:      %10 = affine_apply [[MAP]](%9)
      // CHECK-NEXT: %11 = load %5[%10] : memref<256xf32, 1>
      // Already in faster memory space.
      // CHECK:     %12 = load %2[%i0] : memref<256xf32, 1>
  // CHECK-NEXT: }
  // CHECK-NEXT: return
  for %i = 0 to 256 {
    load %A[%i] : memref<256 x f32>
    %idx = affine_apply (d0) -> (d0 + 256)(%i)
    load %B[%idx] : memref<512 x f32>
    load %F[%i] : memref<256 x f32, 1>
  }
  return
}

// CHECK-LABEL: mlfunc @loop_nest_high_d
// CHECK:       %c16384 = constant 16384 : index
// CHECK-NEXT:  %0 = alloc() : memref<512x32xf32, 1>
// CHECK-NEXT:  %1 = alloc() : memref<1xi32>
// INCOMING DMA for B
// CHECK-NEXT:  dma_start %arg1[%c0, %c0], %0[%c0, %c0], %c16384, %1[%c0] : memref<512x32xf32>, memref<512x32xf32, 1>, memref<1xi32>
// CHECK-NEXT:  dma_wait %1[%c0], %c16384 : memref<1xi32>
// CHECK-NEXT:  %2 = alloc() : memref<512x32xf32, 1>
// CHECK-NEXT:  %3 = alloc() : memref<1xi32>
// INCOMING DMA for A.
// CHECK-NEXT:  dma_start %arg0[%c0, %c0], %2[%c0, %c0], %c16384, %3[%c0] : memref<512x32xf32>, memref<512x32xf32, 1>, memref<1xi32>
// CHECK-NEXT:  dma_wait %3[%c0], %c16384 : memref<1xi32>
// CHECK-NEXT:  %4 = alloc() : memref<512x32xf32, 1>
// CHECK-NEXT:  %5 = alloc() : memref<1xi32>
// INCOMING DMA for C.
// CHECK-NEXT:  dma_start %arg2[%c0, %c0], %4[%c0, %c0], %c16384, %5[%c0] : memref<512x32xf32>, memref<512x32xf32, 1>, memref<1xi32>
// CHECK-NEXT:  dma_wait %5[%c0], %c16384 : memref<1xi32>
// CHECK-NEXT:  %6 = alloc() : memref<1xi32>
// CHECK-NEXT:  for %i0 = 0 to 32 {
// CHECK-NEXT:    for %i1 = 0 to 32 {
// CHECK-NEXT:      for %i2 = 0 to 32 {
// CHECK-NEXT:        for %i3 = 0 to 16 {
// CHECK-NEXT:          %7 = affine_apply #map{{[0-9]+}}(%i1, %i3)
// CHECK-NEXT:          %8 = affine_apply #map{{[0-9]+}}(%7, %i0)
// CHECK-NEXT:          %9 = load %0[%8#0, %8#1] : memref<512x32xf32, 1>
// CHECK-NEXT:          "foo"(%9) : (f32) -> ()
// CHECK-NEXT:        }
// CHECK-NEXT:        for %i4 = 0 to 16 {
// CHECK-NEXT:          %10 = affine_apply #map{{[0-9]+}}(%i2, %i4)
// CHECK-NEXT:          %11 = affine_apply #map{{[0-9]+}}(%10, %i1)
// CHECK-NEXT:          %12 = load %2[%11#0, %11#1] : memref<512x32xf32, 1>
// CHECK-NEXT:          "bar"(%12) {mxu_id: 0} : (f32) -> ()
// CHECK-NEXT:        }
// CHECK-NEXT:        for %i5 = 0 to 16 {
// CHECK-NEXT:          %13 = "abc_compute"() : () -> f32
// CHECK-NEXT:          %14 = affine_apply #map{{[0-9]+}}(%i2, %i5)
// CHECK-NEXT:          %15 = affine_apply #map{{[0-9]+}}(%14, %i0)
// CHECK-NEXT:          %16 = load %4[%15#0, %15#1] : memref<512x32xf32, 1>
// CHECK-NEXT:          %17 = "addf32"(%13, %16) : (f32, f32) -> f32
// CHECK-NEXT:          %18 = affine_apply #map{{[0-9]+}}(%14, %i0)
// CHECK-NEXT:          store %17, %4[%18#0, %18#1] : memref<512x32xf32, 1>
// CHECK-NEXT:        }
// CHECK-NEXT:        "foobar"() : () -> ()
// CHECK-NEXT:      }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
// OUTGOING DMA for C.
// CHECK-NEXT:  dma_start %4[%c0, %c0], %arg2[%c0, %c0], %c16384, %6[%c0] : memref<512x32xf32, 1>, memref<512x32xf32>, memref<1xi32>
// CHECK-NEXT:  dma_wait %6[%c0], %c16384 : memref<1xi32>
// CHECK-NEXT:  return
// CHECK-NEXT:}
mlfunc @loop_nest_high_d(%A: memref<512 x 32 x f32>,
    %B: memref<512 x 32 x f32>, %C: memref<512 x 32 x f32>) {
  // DMAs will be performed at this level (jT is the first loop without a stride).
  // A and B are read, while C is both read and written. A total of three new buffers
  // are allocated and existing load's/store's are replaced by accesses to those buffers.
  for %jT = 0 to 32 {
    for %kT = 0 to 32 {
      for %iT = 0 to 32 {
        for %kk = 0 to 16 { // k intratile
          %k = affine_apply (d0, d1) -> (16*d0 + d1) (%kT, %kk)
          %v0 = load %B[%k, %jT] : memref<512 x 32 x f32>
          "foo"(%v0) : (f32) -> ()
        }
        for %ii = 0 to 16 { // i intratile.
          %i = affine_apply (d0, d1) -> (16*d0 + d1)(%iT, %ii)
          %v1 = load %A[%i, %kT] : memref<512 x 32 x f32>
          "bar"(%v1) {mxu_id: 0} : (f32) -> ()
        }
        for %ii_ = 0 to 16 { // i intratile.
          %v2 = "abc_compute"() : () -> f32
          %i_ = affine_apply (d0, d1) -> (16*d0 + d1)(%iT, %ii_)
          %v3 =  load %C[%i_, %jT] : memref<512 x 32 x f32>
          %v4 = "addf32"(%v2, %v3) : (f32, f32) -> (f32)
          store %v4, %C[%i_, %jT] : memref<512 x 32 x f32>
        }
        "foobar"() : () -> ()
      }
    }
  }
  return
}

// A loop nest with a modulo 2 access.
//
// CHECK-LABEL: mlfunc @loop_nest_modulo() {
// CHECK:       %0 = alloc() : memref<256x8xf32>
// CHECK-NEXT:    for %i0 = 0 to 32 step 4 {
// CHECK-NEXT:    %1 = alloc() : memref<32x2xf32, 1>
// CHECK-NEXT:    %2 = alloc() : memref<1xi32>
// CHECK-NEXT:    dma_start %0[%c0, %c0], %1[%c0, %c0], %c64, %2[%c0] : memref<256x8xf32>, memref<32x2xf32, 1>, memref<1xi32>
// CHECK-NEXT:    dma_wait %2[%c0], %c64 : memref<1xi32>
// CHECK-NEXT:    for %i1 = 0 to 8 {
//                  ...
//                  ...
// CHECK:         }
// CHECK-NEXT:  }
// CHECK-NEXT:  return
mlfunc @loop_nest_modulo() {
  %A = alloc() : memref<256 x 8 x f32>
  for %i = 0 to 32 step 4 {
    // DMAs will be performed at this level (%j is the first unit stride loop)
    for %j = 0 to 8 {
      %idx = affine_apply (d0) -> (d0 mod 2) (%j)
      // A buffer of size 32 x 2 will be allocated (original buffer was 256 x 8).
      %v = load %A[%i, %idx] : memref<256 x 8 x f32>
    }
  }
  return
}


// DMA on tiled loop nest. This also tests the case where the bounds are
// dependent on outer loop IVs.
// CHECK-LABEL: mlfunc @loop_nest_tiled() -> memref<256x1024xf32> {
mlfunc @loop_nest_tiled() -> memref<256x1024xf32> {
  %0 = alloc() : memref<256x1024xf32>
  for %i0 = 0 to 256 step 32 {
    for %i1 = 0 to 1024 step 32 {
// CHECK:      %3 = alloc() : memref<32x32xf32, 1>
// CHECK-NEXT: %4 = alloc() : memref<1xi32>
// CHECK-NEXT: dma_start %0[
// CHECK-NEXT: dma_wait
// CHECK-NEXT: for %i2 = #map
// CHECK-NEXT:   for %i3 = #map
      for %i2 = (d0) -> (d0)(%i0) to (d0) -> (d0 + 32)(%i0) {
        for %i3 = (d0) -> (d0)(%i1) to (d0) -> (d0 + 32)(%i1) {
          // CHECK:      %5 = affine_apply [[MAP_INDEX_DIFF]](%i0, %i1, %i2, %i3)
          // CHECK-NEXT: %6 = load %3[%5#0, %5#1] : memref<32x32xf32, 1>
          %1 = load %0[%i2, %i3] : memref<256x1024xf32>
        } // CHECK-NEXT: }
      }
    }
  }
  // CHECK: return %0 : memref<256x1024xf32>
  return %0 : memref<256x1024xf32>
}
