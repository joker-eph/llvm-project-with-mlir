// RUN: mlir-test-opt %s -split-input-file -verify | FileCheck %s

// -----

// CHECK-LABEL: @tuple_success
func @tuple_success() {
  %0 = "test.tuple_32_bit"() : () -> (tuple<i32>)
  return
}

// -----

// CHECK-LABEL: @tuple_mixed_success
func @tuple_mixed_success() {
  %0 = "test.tuple_32_bit"() : () -> (tuple<i32, f32>)
  return
}

// -----

func @tuple_empty_success() {
  %0 = "test.tuple_32_bit"() : () -> (tuple<>)
  return
}

// -----

func @tuple_wrong_type_scalar() {
  // expected-error@+1 {{must be tuple with any combination of 32-bit integer or 32-bit float values}}
  %0 = "test.tuple_32_bit"() : () -> (tuple<i64>)
  return
}

// -----

func @tuple_wrong_type_tensor() {
  // expected-error@+1 {{must be tuple with any combination of 32-bit integer or 32-bit float values}}
  %0 = "test.tuple_32_bit"() : () -> (tuple<tensor<i32>>)
  return
}

// -----

// CHECK-LABEL: @nested_tuple_empty_success
func @nested_tuple_empty_success() {
  %0 = "test.nested_tuple_32_bit"() : () -> (tuple<>)
  return
}

// -----

// CHECK-LABEL: @nested_tuple_one_level_success
func @nested_tuple_one_level_success() {
  %0 = "test.nested_tuple_32_bit"() : () -> (tuple<i32>)
  return
}

// -----

// CHECK-LABEL: @nested_tuple_multi_level_success
func @nested_tuple_multi_level_success() {
  %0 = "test.nested_tuple_32_bit"() : () -> (tuple<i32, tuple<i32, tuple<i32>>>)
  return
}

// -----

// CHECK-LABEL: @nested_tuple_multi_level_mixed_success
func @nested_tuple_multi_level_mixed_success() {
  %0 = "test.nested_tuple_32_bit"() : () -> (tuple<i32, tuple<f32, tuple<i32>>>)
  return
}

// -----

func @nested_tuple_multi_level_wrong_type() {
  // expected-error@+1 {{must be nested tuple with any combination of 32-bit integer or 32-bit float values}}
  %0 = "test.nested_tuple_32_bit"() : () -> (tuple<i32, tuple<i32, tuple<i64>>>)
  return
}

// -----

// CHECK-LABEL: @fixed_element_types
func @fixed_element_types(%arg0: tensor<* x i32>, %arg1: tensor<* x f32>) {
  %0 = "test.arg_and_res_have_fixed_element_types"(%arg0, %arg1) {attr: ""} : (tensor<* x i32>, tensor<* x f32>) -> tensor<* x i16>
  return
}

// -----

// CHECK-LABEL: @fixed_element_types
func @fixed_element_types(%arg0: tensor<* x i32>, %arg1: tensor<* x f32>) {
  %0 = "test.arg_and_res_have_fixed_element_types"(%arg0, %arg1) {attr: splat<tensor<2xi8>, 1>}: (tensor<* x i32>, tensor<* x f32>) -> tensor<* x i32>
  return
}

// -----

func @fixed_element_types(%arg0: tensor<* x i32>, %arg1: tensor<* x f32>) {
  // expected-error@+1 {{fixed type combination}}
  %0 = "test.arg_and_res_have_fixed_element_types"(%arg0, %arg1) {attr: ""}: (tensor<* x i32>, tensor<* x f32>) -> tensor<* x i32>
  return
}

// -----

func @fixed_element_types(%arg0: tensor<* x i32>, %arg1: tensor<* x f32>) {
  // expected-error@+1 {{fixed type combination}}
  %0 = "test.arg_and_res_have_fixed_element_types"(%arg1, %arg0) {attr: ""}: (tensor<* x f32>, tensor<* x i32>) -> tensor<* x i16>
  return
}

// -----

func @fixed_element_types(%arg0: tensor<* x i32>, %arg1: tensor<* x f32>) {
  // expected-error@+1 {{failed to verify that first and second operand have same element type}}
  "test.operands_have_same_element_type"(%arg1, %arg0): (tensor<* x f32>, tensor<* x i32>) -> ()
  return
}
