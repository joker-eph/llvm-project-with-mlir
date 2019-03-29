// RUN: mlir-opt -convert-to-llvmir %s | FileCheck %s


// CHECK-LABEL: func @check_arguments(%arg0: !llvm<"float*">, %arg1: !llvm<"{ float*, i64, i64 }">, %arg2: !llvm<"{ float*, i64 }">)
func @check_arguments(%static: memref<10x20xf32>, %dynamic : memref<?x?xf32>, %mixed : memref<10x?xf32>) {
  return
}

// CHECK-LABEL: func @check_static_return(%arg0: !llvm<"float*">) -> !llvm<"float*"> {
func @check_static_return(%static : memref<32x18xf32>) -> memref<32x18xf32> {
// CHECK-NEXT: "llvm.return"(%arg0) : (!llvm<"float*">) -> ()
  return %static : memref<32x18xf32>
}


// CHECK-LABEL: func @mixed_alloc(%arg0: !llvm<"i64">, %arg1: !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }"> {
func @mixed_alloc(%arg0: index, %arg1: index) -> memref<?x42x?xf32> {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.mul"(%arg0, %0) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%1, %arg1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.constant"() {value: 4 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.mul"(%2, %3) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %5 = "llvm.call"(%4) {callee: @malloc : (!llvm<"i64">) -> !llvm<"i8*">} : (!llvm<"i64">) -> !llvm<"i8*">
// CHECK-NEXT: %6 = "llvm.bitcast"(%5) : (!llvm<"i8*">) -> !llvm<"float*">
// CHECK-NEXT: %7 = "llvm.undef"() : () -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %8 = "llvm.insertvalue"(%7, %6) {position: [0]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %9 = "llvm.insertvalue"(%8, %arg0) {position: [1]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %10 = "llvm.insertvalue"(%9, %arg1) {position: [2]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
  %0 = alloc(%arg0, %arg1) : memref<?x42x?xf32>
  return %0 : memref<?x42x?xf32>
}

// CHECK-LABEL: func @mixed_dealloc(%arg0: !llvm<"{ float*, i64, i64 }">) {
func @mixed_dealloc(%arg0: memref<?x42x?xf32>) {
// CHECK-NEXT:  %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"float*">
// CHECK-NEXT:  %1 = "llvm.bitcast"(%0) : (!llvm<"float*">) -> !llvm<"i8*">
// CHECK-NEXT:  "llvm.call"(%1) {callee: @free : (!llvm<"i8*">) -> ()} : (!llvm<"i8*">) -> ()
  dealloc %arg0 : memref<?x42x?xf32>
// CHECK-NEXT:  "llvm.return"() : () -> ()
  return
}

// CHECK-LABEL: func @dynamic_alloc(%arg0: !llvm<"i64">, %arg1: !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }"> {
func @dynamic_alloc(%arg0: index, %arg1: index) -> memref<?x?xf32> {
// CHECK-NEXT: %0 = "llvm.mul"(%arg0, %arg1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.constant"() {value: 4 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%0, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.call"(%2) {callee: @malloc : (!llvm<"i64">) -> !llvm<"i8*">} : (!llvm<"i64">) -> !llvm<"i8*">
// CHECK-NEXT: %4 = "llvm.bitcast"(%3) : (!llvm<"i8*">) -> !llvm<"float*">
// CHECK-NEXT: %5 = "llvm.undef"() : () -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %6 = "llvm.insertvalue"(%5, %4) {position: [0]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %7 = "llvm.insertvalue"(%6, %arg0) {position: [1]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %8 = "llvm.insertvalue"(%7, %arg1) {position: [2]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
  %0 = alloc(%arg0, %arg1) : memref<?x?xf32>
  return %0 : memref<?x?xf32>
}

// CHECK-LABEL: func @dynamic_dealloc(%arg0: !llvm<"{ float*, i64, i64 }">) {
func @dynamic_dealloc(%arg0: memref<?x?xf32>) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %1 = "llvm.bitcast"(%0) : (!llvm<"float*">) -> !llvm<"i8*">
// CHECK-NEXT: "llvm.call"(%1) {callee: @free : (!llvm<"i8*">) -> ()} : (!llvm<"i8*">) -> ()
  dealloc %arg0 : memref<?x?xf32>
  return
}

// CHECK-LABEL: func @static_alloc() -> !llvm<"float*"> {
func @static_alloc() -> memref<32x18xf32> {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 32 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.constant"() {value: 18 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%0, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.constant"() {value: 4 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.mul"(%2, %3) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %5 = "llvm.call"(%4) {callee: @malloc : (!llvm<"i64">) -> !llvm<"i8*">} : (!llvm<"i64">) -> !llvm<"i8*">
// CHECK-NEXT: %6 = "llvm.bitcast"(%5) : (!llvm<"i8*">) -> !llvm<"float*">
 %0 = alloc() : memref<32x18xf32>
 return %0 : memref<32x18xf32>
}

// CHECK-LABEL: func @static_dealloc(%arg0: !llvm<"float*">) {
func @static_dealloc(%static: memref<10x8xf32>) {
// CHECK-NEXT: %0 = "llvm.bitcast"(%arg0) : (!llvm<"float*">) -> !llvm<"i8*">
// CHECK-NEXT: "llvm.call"(%0) {callee: @free : (!llvm<"i8*">) -> ()} : (!llvm<"i8*">) -> ()
  dealloc %static : memref<10x8xf32>
  return
}

// CHECK-LABEL: func @static_load
func @static_load(%static : memref<10x42xf32>, %i : index, %j : index) {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 10 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%arg1, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.add"(%2, %arg2) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.getelementptr"(%arg0, %3) : (!llvm<"float*">, !llvm<"i64">) -> !llvm<"float*">
// CHECK-NEXT: %5 = "llvm.load"(%4) : (!llvm<"float*">) -> !llvm<"float">
  %0 = load %static[%i, %j] : memref<10x42xf32>
  return
}

// CHECK-LABEL: func @mixed_load
func @mixed_load(%mixed : memref<42x?xf32>, %i : index, %j : index) {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%arg1, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.add"(%2, %arg2) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %5 = "llvm.getelementptr"(%4, %3) : (!llvm<"float*">, !llvm<"i64">) -> !llvm<"float*">
// CHECK-NEXT: %6 = "llvm.load"(%5) : (!llvm<"float*">) -> !llvm<"float">
  %0 = load %mixed[%i, %j] : memref<42x?xf32>
  return
}

// CHECK-LABEL: func @dynamic_load
func @dynamic_load(%dynamic : memref<?x?xf32>, %i : index, %j : index) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.extractvalue"(%arg0) {position: [2]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%arg1, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.add"(%2, %arg2) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %5 = "llvm.getelementptr"(%4, %3) : (!llvm<"float*">, !llvm<"i64">) -> !llvm<"float*">
// CHECK-NEXT: %6 = "llvm.load"(%5) : (!llvm<"float*">) -> !llvm<"float">
  %0 = load %dynamic[%i, %j] : memref<?x?xf32>
  return
}

// CHECK-LABEL: func @static_store
func @static_store(%static : memref<10x42xf32>, %i : index, %j : index, %val : f32) {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 10 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%arg1, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.add"(%2, %arg2) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.getelementptr"(%arg0, %3) : (!llvm<"float*">, !llvm<"i64">) -> !llvm<"float*">
// CHECK-NEXT: "llvm.store"(%arg3, %4) : (!llvm<"float">, !llvm<"float*">) -> ()
  store %val, %static[%i, %j] : memref<10x42xf32>
  return
}

// CHECK-LABEL: func @dynamic_store
func @dynamic_store(%dynamic : memref<?x?xf32>, %i : index, %j : index, %val : f32) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.extractvalue"(%arg0) {position: [2]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%arg1, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.add"(%2, %arg2) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %5 = "llvm.getelementptr"(%4, %3) : (!llvm<"float*">, !llvm<"i64">) -> !llvm<"float*">
// CHECK-NEXT: "llvm.store"(%arg3, %5) : (!llvm<"float">, !llvm<"float*">) -> ()
  store %val, %dynamic[%i, %j] : memref<?x?xf32>
  return
}

// CHECK-LABEL: func @mixed_store
func @mixed_store(%mixed : memref<42x?xf32>, %i : index, %j : index, %val : f32) {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %1 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %2 = "llvm.mul"(%arg1, %1) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.add"(%2, %arg2) : (!llvm<"i64">, !llvm<"i64">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %5 = "llvm.getelementptr"(%4, %3) : (!llvm<"float*">, !llvm<"i64">) -> !llvm<"float*">
// CHECK-NEXT: "llvm.store"(%arg3, %5) : (!llvm<"float">, !llvm<"float*">) -> ()
  store %val, %mixed[%i, %j] : memref<42x?xf32>
  return
}

// CHECK-LABEL: func @memref_cast_static_to_dynamic
func @memref_cast_static_to_dynamic(%static : memref<10x42xf32>) {
// CHECK-NEXT: %0 = "llvm.undef"() : () -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %1 = "llvm.insertvalue"(%0, %arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %2 = "llvm.constant"() {value: 10 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.insertvalue"(%1, %2) {position: [1]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %4 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %5 = "llvm.insertvalue"(%3, %4) {position: [2]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
  %0 = memref_cast %static : memref<10x42xf32> to memref<?x?xf32>
  return
}

// CHECK-LABEL: func @memref_cast_static_to_mixed
func @memref_cast_static_to_mixed(%static : memref<10x42xf32>) {
// CHECK-NEXT: %0 = "llvm.undef"() : () -> !llvm<"{ float*, i64 }">
// CHECK-NEXT: %1 = "llvm.insertvalue"(%0, %arg0) {position: [0]} : (!llvm<"{ float*, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64 }">
// CHECK-NEXT: %2 = "llvm.constant"() {value: 10 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %3 = "llvm.insertvalue"(%1, %2) {position: [1]} : (!llvm<"{ float*, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64 }">
  %0 = memref_cast %static : memref<10x42xf32> to memref<?x42xf32>
  return
}

// CHECK-LABEL: func @memref_cast_dynamic_to_static
func @memref_cast_dynamic_to_static(%dynamic : memref<?x?xf32>) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"float*">
  %0 = memref_cast %dynamic : memref<?x?xf32> to memref<10x12xf32>
  return
}

// CHECK-LABEL: func @memref_cast_dynamic_to_mixed
func @memref_cast_dynamic_to_mixed(%dynamic : memref<?x?xf32>) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %1 = "llvm.undef"() : () -> !llvm<"{ float*, i64 }">
// CHECK-NEXT: %2 = "llvm.insertvalue"(%1, %0) {position: [0]} : (!llvm<"{ float*, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64 }">
// CHECK-NEXT: %3 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.insertvalue"(%2, %3) {position: [1]} : (!llvm<"{ float*, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64 }">
  %0 = memref_cast %dynamic : memref<?x?xf32> to memref<?x12xf32>
  return
}

// CHECK-LABEL: func @memref_cast_mixed_to_dynamic
func @memref_cast_mixed_to_dynamic(%mixed : memref<42x?xf32>) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %1 = "llvm.undef"() : () -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %2 = "llvm.insertvalue"(%1, %0) {position: [0]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %3 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.insertvalue"(%2, %3) {position: [1]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
// CHECK-NEXT: %5 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64 }">) -> !llvm<"i64">
// CHECK-NEXT: %6 = "llvm.insertvalue"(%4, %5) {position: [2]} : (!llvm<"{ float*, i64, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64, i64 }">
  %0 = memref_cast %mixed : memref<42x?xf32> to memref<?x?xf32>
  return
}

// CHECK-LABEL: func @memref_cast_mixed_to_static
func @memref_cast_mixed_to_static(%mixed : memref<42x?xf32>) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64 }">) -> !llvm<"float*">
  %0 = memref_cast %mixed : memref<42x?xf32> to memref<42x1xf32>
  return
}

// CHECK-LABEL: func @memref_cast_mixed_to_mixed
func @memref_cast_mixed_to_mixed(%mixed : memref<42x?xf32>) {
// CHECK-NEXT: %0 = "llvm.extractvalue"(%arg0) {position: [0]} : (!llvm<"{ float*, i64 }">) -> !llvm<"float*">
// CHECK-NEXT: %1 = "llvm.undef"() : () -> !llvm<"{ float*, i64 }">
// CHECK-NEXT: %2 = "llvm.insertvalue"(%1, %0) {position: [0]} : (!llvm<"{ float*, i64 }">, !llvm<"float*">) -> !llvm<"{ float*, i64 }">
// CHECK-NEXT: %3 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
// CHECK-NEXT: %4 = "llvm.insertvalue"(%2, %3) {position: [1]} : (!llvm<"{ float*, i64 }">, !llvm<"i64">) -> !llvm<"{ float*, i64 }">
  %0 = memref_cast %mixed : memref<42x?xf32> to memref<?x1xf32>
  return
}

// CHECK-LABEL: func @mixed_memref_dim(%arg0: !llvm<"{ float*, i64, i64, i64 }">)
func @mixed_memref_dim(%mixed : memref<42x?x?x13x?xf32>) {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
  %0 = dim %mixed, 0 : memref<42x?x?x13x?xf32>
// CHECK-NEXT: %1 = "llvm.extractvalue"(%arg0) {position: [1]} : (!llvm<"{ float*, i64, i64, i64 }">) -> !llvm<"i64">
  %1 = dim %mixed, 1 : memref<42x?x?x13x?xf32>
// CHECK-NEXT: %2 = "llvm.extractvalue"(%arg0) {position: [2]} : (!llvm<"{ float*, i64, i64, i64 }">) -> !llvm<"i64">
  %2 = dim %mixed, 2 : memref<42x?x?x13x?xf32>
// CHECK-NEXT: %3 = "llvm.constant"() {value: 13 : index} : () -> !llvm<"i64">
  %3 = dim %mixed, 3 : memref<42x?x?x13x?xf32>
// CHECK-NEXT: %4 = "llvm.extractvalue"(%arg0) {position: [3]} : (!llvm<"{ float*, i64, i64, i64 }">) -> !llvm<"i64">
  %4 = dim %mixed, 4 : memref<42x?x?x13x?xf32>
  return
}

// CHECK-LABEL: func @static_memref_dim(%arg0: !llvm<"float*">)
func @static_memref_dim(%static : memref<42x32x15x13x27xf32>) {
// CHECK-NEXT: %0 = "llvm.constant"() {value: 42 : index} : () -> !llvm<"i64">
  %0 = dim %static, 0 : memref<42x32x15x13x27xf32>
// CHECK-NEXT: %1 = "llvm.constant"() {value: 32 : index} : () -> !llvm<"i64">
  %1 = dim %static, 1 : memref<42x32x15x13x27xf32>
// CHECK-NEXT: %2 = "llvm.constant"() {value: 15 : index} : () -> !llvm<"i64">
  %2 = dim %static, 2 : memref<42x32x15x13x27xf32>
// CHECK-NEXT: %3 = "llvm.constant"() {value: 13 : index} : () -> !llvm<"i64">
  %3 = dim %static, 3 : memref<42x32x15x13x27xf32>
// CHECK-NEXT: %4 = "llvm.constant"() {value: 27 : index} : () -> !llvm<"i64">
  %4 = dim %static, 4 : memref<42x32x15x13x27xf32>
  return
}

