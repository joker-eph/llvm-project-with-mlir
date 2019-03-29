# MLIR Specification

MLIR is a compiler intermediate representation with similarities to traditional
three-address SSA representations (like
[LLVM IR](http://llvm.org/docs/LangRef.html) or
[SIL](https://github.com/apple/swift/blob/master/docs/SIL.rst)), but which
introduces notions from polyhedral loop optimization as first-class concepts.
This hybrid design is optimized to represent, analyze, and transform high level
dataflow graphs as well as target-specific code generated for high performance
data parallel systems. Beyond its representational capabilities, its single
continuous design provides a framework to lower from dataflow graphs to
high-performance target-specific code.

MLIR stands for one of "Multi-Level IR" or "Multi-dimensional Loop IR" or
"Machine Learning IR" - the MLIR team prefers the first interpretation. This
document defines and describes the key concepts in MLIR, and is intended to be a
dry reference document - [rationale documentation](Rationale.md),
[system overview documentation](https://docs.google.com/document/d/1yRqja94Da6NtKmPxSYtTx6xbUtughLANyeD7dZ7mOBM/edit#)
and other content is hosted elsewhere.

MLIR is designed to be used in three different forms: a human-readable textual
form suitable for debugging, an in-memory form suitable for programmatic
transformations and analysis, and a compact serialized form suitable for storage
and transport. The different forms all describe the same semantic content. This
document describes the human-readable textual form.

[TOC]

## High-Level Structure {#high-level-structure}

The top-level unit of code in MLIR is a [Module](#module). A module contains a
list of [Functions](#functions). Functions are represented as a composition of
[operations](#operations) and contain a Control Flow Graph (CFG) of
[Blocks](#blocks), which contain instructions and end with
[terminator operations](#terminator-operations) (like branches).

MLIR is an
[SSA-based](https://en.wikipedia.org/wiki/Static_single_assignment_form) IR,
which means that values are defined before use and have scope defined by their
dominance relations. Operations may produce zero or more results, and each is a
distinct SSA value with its own type defined by the [type system](#type-system).

MLIR incorporates polyhedral compiler concepts, including `for` and `if`
instructions, which model affine loops and affine conditionals. It also includes
affine maps integrated into the type system - they are key to the representation
of data and [MemRefs](#memref-type), which are the representation for tensors in
addressable memory. MLIR also supports a first-class Tensor type allowing it to
concisely represent operations on N-dimensional arrays.

Finally, MLIR supports operations for allocating buffers, producing views to
transform them, represent target-independent arithmetic, target-specific
instructions, and even supports arbitrary user-defined high-level tensor
operations.

Here's an example of an MLIR module:

```mlir {.mlir}
// Compute A*B using an implementation of multiply kernel and print the
// result using a TensorFlow op. The dimensions of A and B are partially
// known. The shapes are assumed to match.
func @mul(%A: tensor<100x?xf32>, %B: tensor<?x50xf32>) -> (tensor<100x50xf32>) {
  // Compute the inner dimension of %A using the dim operation.
  %n = dim %A, 1 : tensor<100x?xf32>

  // Allocate addressable "buffers" and copy tensors %A and %B into them.
  %A_m = alloc memref<100x?xf32>(%n)
  tensor_store %A to %A_m : memref<100x?xf32>

  %B_m = alloc memref<?x50xf32>(%n)
  tensor_store %B to %B_m : memref<?x50xf32>

  // Call ML function @multiply passing memrefs as arguments,
  // and getting returned the result of the multiplication.
  %C_m = call @multiply(%A_m, %B_m)
          : (memref<100x?xf32>, memref<?x50xf32>) -> (memref<100x50xf32>)

  dealloc %A_m : memref<100x?xf32>
  dealloc %B_m : memref<?x50xf32>

  // Load the buffer data into a higher level "tensor" value.
  %C = tensor_load %C_m : memref<100x50xf32>
  dealloc %C_m : memref<100x50xf32>

  // Call TensorFlow built-in function to print the result tensor.
  "tf.Print"(%C){message: "mul result"}
                  : (tensor<100x50xf32) -> (tensor<100x50xf32>)

  return %C : tensor<100x50xf32>
}

// A function that multiplies two memrefs and returns the result.
func @multiply(%A: memref<100x?xf32>, %B: memref<?x50xf32>)
          -> (memref<100x50xf32>)  {
  // Compute the inner dimension of %A.
  %n = dim %A, 1 : memref<100x?xf32>

  // Allocate memory for the multiplication result.
  %C = alloc memref<100x50xf32>()

  // Multiplication loop nest.
  for  %i = 0 to 100 {
     for %j = 0 to 50 {
        store 0 to %C[%i, %j] : memref<100x50xf32>
        for %k = 0 to %n {
           %a_v  = load %A[%i, %k] : memref<100x?xf32>
           %b_v  = load %B[%k, %j] : memref<?x50xf32>
           %prod = mulf %a_v, %b_v : f32
           %c_v  = load %C[%i, %j] : memref<100x50xf32>
           %sum  = addf %c_v, %prod : f32
           store %sum, %C[%i, %j] : memref<100x50xf32>
        }
     }
  }
  return %C : memref<100x50xf32>
}
```

## Notation {#notation}

MLIR has a simple and unambiguous grammar, allowing it to reliably round-trip
through a textual form. This is important for development of the compiler - e.g.
understanding the state of code as it is being transformed and for writing test
cases.

This document describes the grammar using
[Extended Backus-Naur Form (EBNF)](https://en.wikipedia.org/wiki/Extended_Backus%E2%80%93Naur_form).

This is the EBNF grammar used in this document, presented in yellow boxes.

``` {.ebnf}
alternation ::= expr0 | expr1 | expr2  // Either expr0 or expr1 or expr2.
sequence    ::= expr0 expr1 expr2      // Sequence of expr0 expr1 expr2.
repetition0 ::= expr*  // 0 or more occurrences.
repetition1 ::= expr+  // 1 or more occurrences.
optionality ::= expr?  // 0 or 1 occurrence.
grouping    ::= (expr) // Everything inside parens is grouped together.
literal     ::= `abcd` // Matches the literal `abcd`.
```

Code examples are presented in blue boxes.

```mlir {.mlir}
// This is an example use of the grammar above:
// This matches things like: ba, bana, boma, banana, banoma, bomana...
example ::= `b` (`an` | `om`)* `a`
```

### Common syntax {#common-syntax}

The following core grammar productions are used in this document:

``` {.ebnf}
// TODO: Clarify the split between lexing (tokens) and parsing (grammar).
digit     ::= [0-9]
hex_digit ::= [0-9a-fA-F]
letter    ::= [a-zA-Z]
id-punct  ::= [$._-]

integer-literal ::= digit+ | `0x` hex_digit+
float-literal   ::= TODO
string-literal  ::= `"` [^"\n\f\v\r]* `"`   TODO define escaping rules
```

Not listed here, but MLIR does support comments. They use standard BCPL syntax,
starting with a `//` and going until the end of the line.

### Identifiers and keywords {#identifiers-and-keywords}

Syntax:

``` {.ebnf}
// Identifiers
bare-id ::= letter (letter|digit|[_])*
bare-id-list ::= bare-id (`,` bare-id)*
suffix-id ::= digit+ | ((letter|id-punct) (letter|id-punct|digit)*)

function-id ::= `@` bare-id
ssa-id ::= `%` suffix-id
ssa-id-list ::= ssa-id (`,` ssa-id)*

// Uses of an SSA value, e.g. in an operand list to an instruction.
ssa-use ::= ssa-id
ssa-use-list ::= ssa-use (`,` ssa-use)*
```

Identifiers name entities such as SSA values, types and functions, and are
chosen by the writer of MLIR code. Identifiers may be descriptive (e.g.
`%batch_size`, `@matmul`), or may be non-descriptive when they are
auto-generated (e.g. `%23`, `@func42`). Identifier names for SSA values may be
used in an MLIR text file but are not persisted as part of the IR - the printer
will give them anonymous names like `%42`.

MLIR guarantees identifiers never collide with keywords by prefixing identifiers
with a sigil (e.g. `%`, `#`, `@`, `^`). In certain unambiguous contexts (e.g.
affine expressions), identifiers are not prefixed, for brevity. New keywords may
be added to future versions of MLIR without danger of collision with existing
identifiers.

The scope of SSA values is defined based on the standard definition of
[dominance](https://en.wikipedia.org/wiki/Dominator_\(graph_theory\)). Argument
identifiers in mapping functions are in scope for the mapping body. Function
identifiers and mapping identifiers are visible across the entire module.

## Polyhedral Structures {#polyhedral-structures}

MLIR uses techniques from polyhedral compilation to make dependence analysis and
loop transformations efficient and reliable. This section introduces some of the
core concepts that are used throughout the document.

### Dimensions and Symbols {#dimensions-and-symbols}

Dimensions and symbols are the two kinds of identifiers that can appear in the
polyhedral structures, and are always of '[index](#index-type)' type. Dimensions
are declared in parentheses and symbols are declared in square brackets.

Examples:

```mlir {.mlir}
// A 2d to 3d affine mapping.
// d0/d1 are dimensions, s0 is a symbol
#affine_map2to3 = (d0, d1)[s0] -> (d0, d1 + s0, d1 - s0) size (10, 20, 30)
```

Dimensional identifiers correspond to the dimensions of the underlying structure
being represented (a map, set, or more concretely a loop nest or a tensor); for
example, a three-dimensional loop nest has three dimensional identifiers. Symbol
identifiers represent an unknown quantity that can be treated as constant for a
region of interest.

Dimensions and symbols are bound to SSA values by various operations in MLIR and
use the same parenthesized vs square bracket list to distinguish the two.

Syntax:

``` {.ebnf}
// Uses of SSA values that are passed to dimensional identifiers.
dim-use-list ::= `(` ssa-use-list? `)`

// Uses of SSA values that are used to bind symbols.
symbol-use-list ::= `[` ssa-use-list? `]`

// Most things that bind SSA values bind dimensions and symbols.
dim-and-symbol-use-list ::= dim-use-list symbol-use-list?
```

SSA values bound to dimensions and symbols must always have 'index' type.

A symbolic identifier can be bound to an SSA value that is either an argument to
the function, a value defined at the top level of that function (outside of all
loops and if instructions), the result of a
[`constant` operation](#'constant'-operation), or the result of an
[`affine_apply`](#'affine_apply'-operation) operation that recursively takes as
arguments any symbolic identifiers. Dimensions may be bound not only to anything
that a symbol is bound to, but also to induction variables of enclosing
[for instructions](#'for'-instruction), and the result of an
[`affine_apply` operation](#'affine_apply'-operation) (which recursively may use
other dimensions and symbols).

Example:

```mlir {.mlir}
#affine_map2to3 = (d0, d1)[s0] -> (d0, d1 + s0, d1 - s0) size (10,20,30)
// Binds %N to the s0 symbol in affine_map2to3.
%x = alloc()[%N] : memref<40x50xf32, #affine_map2to3>
```

### Affine Expressions {#affine-expressions}

Syntax:

``` {.ebnf}
affine-expr ::= `(` affine-expr `)`
              | affine-expr `+` affine-expr
              | affine-expr `-` affine-expr
              | `-`? integer-literal `*` affine-expr
              | affine-expr `ceildiv` integer-literal
              | affine-expr `floordiv` integer-literal
              | affine-expr `mod` integer-literal
              | `-`affine-expr
              | bare-id
              | `-`? integer-literal

multi-dim-affine-expr ::= `(` affine-expr (`,` affine-expr)* `)`
```

`ceildiv` is the ceiling function which maps the result of the division of its
first argument by its second argument to the smallest integer greater than or
equal to that result. `floordiv` is a function which maps the result of the
division of its first argument by its second argument to the largest integer
less than or equal to that result. `mod` is the modulo operation: since its
second argument is always positive, its results are always positive in our
usage. The `integer-literal` operand for ceildiv, floordiv, and mod is always
expected to be positive. `bare-id` is an identifier which must have type
[index](#index-type). The precedence of operations in an affine expression are
ordered from highest to lowest in the order: (1) parenthesization, (2) negation,
(3) modulo, multiplication, floordiv, and ceildiv, and (4) addition and
subtraction. All of these operators associate from left to right.

A _multi-dimensional affine expression_ is a comma separated list of
one-dimensional affine expressions, with the entire list enclosed in
parentheses.

**Context:** An affine function, informally, is a linear function plus a
constant. More formally, a function f defined on a vector $$\vec{v} \in
\mathbb{Z}^n$$ is a multidimensional affine function of $$\vec{v}$$ if
$$f(\vec{v})$$ can be expressed in the form $$M \vec{v} + \vec{c}$$ where $$M$$
is a constant matrix from $$\mathbb{Z}^{m \times n}$$ and $$\vec{c}$$ is a
constant vector from $$\mathbb{Z}$$. $$m$$ is the dimensionality of such an
affine function. MLIR further extends the definition of an affine function to
allow 'floordiv', 'ceildiv', and 'mod' with respect to positive integer
constants. Such extensions to affine functions have often been referred to as
quasi-affine functions by the polyhedral compiler community. MLIR uses the term
'affine map' to refer to these multi-dimensional quasi-affine functions. As
examples, $$(i+j+1, j)$$, $$(i \mod 2, j+i)$$, $$(j, i/4, i \mod 4)$$, $$(2i+1,
j)$$ are two-dimensional affine functions of $$(i, j)$$, but $$(i \cdot j,
i^2)$$, $$(i \mod j, i/j)$$ are not affine functions of $$(i, j)$$.

### Affine Maps {#affine-maps}

Syntax:

``` {.ebnf}
affine-map-inline
   ::= dim-and-symbol-id-lists `->` multi-dim-affine-expr
       ( `size` `(` dim-size (`,` dim-size)* `)` )?

dim-size ::= affine-expr
           | `min` `(` affine-expr ( `,` affine-expr)+ `)`
```

The identifiers in the dimensions and symbols lists must be unique. These are
the only identifiers that may appear in 'multi-dim-affine-expr'. In addition,
only symbolic identifiers and constants can appear in 'dim-size'. Affine maps
with one or more symbols in its specification are known as "symbolic affine
maps", and those with no symbols as "non-symbolic affine maps". An affine map
has an optional "size" tuple which provides the size for each corresponding
dimension. Affine maps with a size in their specification are known as "bounded
affine maps", and those without a size are "unbounded affine maps".

**Context:** Affine maps are mathematical functions that transform a list of
dimension indices and symbols into a list of results, with affine expressions
combining the indices and symbols. Affine maps distinguish between
[indices and symbols](#dimensions-and-symbols) because indices are inputs to the
affine map when the latter is called through an
[affine_apply](#'affine_apply'-operation) operation, whereas symbols are bound
when an affine mapping is established (e.g. when a memref is formed,
establishing a memory [layout map](#layout-map)).

Affine maps are used for various core structures in MLIR. The restrictions we
impose on their form allows powerful analysis and transformation, while keeping
the representation closed with respect to several operations of interest.

#### Named affine mappings {#named-affine-mappings}

Syntax:

``` {.ebnf}
affine-map-id ::= `#` suffix-id

// Definitions of affine maps are at the top of the file.
affine-map-def    ::= affine-map-id `=` affine-map-inline
module-header-def ::= affine-map-def

// Uses of affine maps may use the inline form or the named form.
affine-map ::= affine-map-id | affine-map-inline
```

Affine mappings may be defined inline at the point of use, or may be hoisted to
the top of the file and given a name with an affine map definition, and used by
name.

Examples:

```mlir {.mlir}
// Affine map out-of-line definition and usage example.
#affine_map42 =
  (d0, d1)[s0] -> (d0, d0 + d1 + floordiv(s0,2)) size (10, s0)

// Use an affine mapping definition in an alloc instruction, binding the
// SSA value %N to the symbol s0.
%a = alloc memref<4x4xf32, #affine_map42> () [%N]

// Same thing with an inline affine mapping definition.
%b = alloc memref<4x4xf32, (d0, d1)[s0] -> (d0, d0 + d1 + floordiv(s0,2))
                                           size (10, s0)> () [%N]
```

### Semi-affine maps {#semi-affine-maps}

Semi-affine maps are extensions of affine maps to allow multiplication,
`floordiv`, `ceildiv`, and `mod` with respect to symbolic identifiers.
Semi-affine maps are thus a strict superset of affine maps.

Syntax of semi-affine expressions:

``` {.ebnf}
semi-affine-expr ::= `(` semi-affine-expr `)`
                   | semi-affine-expr `+` semi-affine-expr
                   | semi-affine-expr `-` semi-affine-expr
                   | symbol-or-const `*` semi-affine-expr
                   | `ceildiv` `(` semi-affine-expr `,` symbol-or-const `)`
                   | `floordiv` `(` semi-affine-expr `,` symbol-or-const `)`
                   | semi-affine-expr `mod` symbol-or-const
                   | bare-id
                   | `-`? integer-literal

symbol-or-const ::= `-`? integer-literal | symbol-id

multi-dim-semi-affine-expr ::= `(` semi-affine-expr (`,` semi-affine-expr)* `)`
```

The precedence and associativity of operations in the syntax above is the same
as that for [affine expressions](#affine-expressions).

Syntax of semi-affine maps:

``` {.ebnf}
semi-affine-map-inline
   ::= dim-and-symbol-id-lists `->` multi-dim-semi-affine-expr
       ( `size` `(` dim-size (`,` dim-size)* `)` )?
```

Semi-affine maps may be defined inline at the point of use, or may be hoisted to
the top of the file and given a name with a semi-affine map definition, and used
by name.

``` {.ebnf}
semi-affine-map-id ::= `#` suffix-id

// Definitions of semi-affine maps are at the top of file.
semi-affine-map-def ::= semi-affine-map-id `=` semi-affine-map-inline
module-header-def ::= semi-affine-map-def

// Uses of semi-affine maps may use the inline form or the named form.
semi-affine-map ::= semi-affine-map-id | semi-affine-map-inline
```

### Integer Sets {#integer-sets}

An integer set is a conjunction of affine constraints on a list of identifiers.
The identifiers associated with the integer set are separated out into two
classes: the set's dimension identifiers, and the set's symbolic identifiers.
The set is viewed as being parametric on its symbolic identifiers. In the
syntax, the list of set's dimension identifiers are enclosed in parentheses
while its symbols are enclosed in square brackets.

Syntax of affine constraints:

``` {.ebnf}
affine-constraint ::= affine-expr `>=` `0`
                    | affine-expr `==` `0`
affine-constraint-conjunction ::= affine-constraint (`,` affine-constraint)*
```

Integer sets may be defined inline at the point of use, or may be hoisted to the
top of the file and given a name with an integer set definition, and used by
name.

``` {.ebnf}
integer-set-id ::= `#` suffix-id

integer-set-inline
   ::= dim-and-symbol-id-lists `:` '(' affine-constraint-conjunction? ')'

// Declarations of integer sets are at the top of the file.
integer-set-decl ::= integer-set-id `=` integer-set-inline

// Uses of integer sets may use the inline form or the named form.
integer-set ::= integer-set-id | integer-set-inline
```

The dimensionality of an integer set is the number of identifiers appearing in
dimension list of the set. The affine-constraint non-terminals appearing in the
syntax above are only allowed to contain identifiers from dims and symbols. A
set with no constraints is a set that is unbounded along all of the set's
dimensions.

Example:

```mlir {.mlir}
// A example two-dimensional integer set with two symbols.
#set42 = (d0, d1)[s0, s1]
   : d0 >= 0, -d0 + s0 - 1 >= 0, d1 >= 0, -d1 + s1 - 1 >= 0

// Inside an ML Function
if #set42(%i, %j)[%M, %N] {
  ...
}
```

`d0` and `d1` correspond to dimensional identifiers of the set, while `s0` and
`s1` are symbol identifiers.

## Type System {#type-system}

Each SSA value in MLIR has a type defined by the type system below. There are a
number of primitive types (like integers) and also aggregate types for tensors
and memory buffers. MLIR standard types do not include complex numbers, tuples,
structures, arrays, or dictionaries.

MLIR has an open type system (there is no fixed list of types), and types may
have application-specific semantics. For example, MLIR supports a set of
[standard types](#standard-types) as well as
[dialect-specific types](#dialect-specific-types).

``` {.ebnf}
type ::= integer-type
       | index-type
       | float-type
       | vector-type
       | tensor-type
       | memref-type
       | function-type
       | dialect-type
       | type-alias

// MLIR doesn't have a tuple type but functions can return multiple values.
type-list ::= type-list-parens | type
type-list-no-parens ::=  type (`,` type)*
type-list-parens ::= `(` `)`
                   | `(` type-list-no-parens `)`

// This is a common way to refer to an SSA value with a specified type.
ssa-use-and-type ::= ssa-use `:` type

// Non-empty list of names and types.
ssa-use-and-type-list ::= ssa-use-and-type (`,` ssa-use-and-type)*
```

### Type Aliases {#type-aliases}

``` {.ebnf}
type-alias-def ::= '!' alias-name '=' 'type' type
type-alias ::= '!' alias-name
```

MLIR supports defining named aliases for types. A type alias is an identifier
that can be used in the place of the type that it defines. These aliases *must*
be defined before their uses.

Example:

```mlir {.mlir}
!avx.m128 = type vector<4 x f32>

// Using the original type.
"foo"(%x) : vector<4 x f32> -> ()

// Using the type alias.
"foo"(%x) : !avx.m128 -> ()
```

### Builtin Types {#builtin-types}

Builtin types consist of only the types needed for the validity of the IR.

#### Function Type {#function-type}

Syntax:

``` {.ebnf}
function-type ::= type-list-parens `->` type-list
```

MLIR supports first-class functions: the
[`constant` operation](#'constant'-operation) produces the address of a function
as an SSA value. This SSA value may be passed to and returned from functions,
merged across control flow boundaries with [block arguments](#blocks), and
called with the [`call_indirect` instruction](#'call_indirect'-operation).

Function types are also used to indicate the arguments and results of
[operations](#operations).

#### Index Type {#index-type}

Syntax:

``` {.ebnf}
// Target word-sized integer.
index-type ::= `index`
```

The `index` type is a signless integer whose size is equal to the natural
machine word of the target ([rationale](Rationale.md#signless-types)) and is
used by the affine constructs in MLIR. Unlike fixed-size integers. It cannot be
used as an element of vector, tensor or memref type
([rationale](Rationale.md#index-type-disallowed-in-aggregate-types)).

**Rationale:** integers of platform-specific bit widths are practical to express
sizes, dimensionalities and subscripts.

TODO (Index type should not be a builtin).

### Standard Types {#standard-types}

#### Integer Type {#integer-type}

Syntax:

``` {.ebnf}
// Sized integers like i1, i4, i8, i16, i32.
integer-type ::= `i[1-9][0-9]*`
```

MLIR supports arbitrary precision integer types. Integer types are signless, but
have a designated width.

**Rationale:** low precision integers (like `i2`, `i4` etc) are useful for
low-precision inference chips, and arbitrary precision integers are useful for
hardware synthesis (where a 13 bit multiplier is a lot cheaper/smaller than a 16
bit one).

TODO: Need to decide on a representation for quantized integers
([initial thoughts](Rationale.md#quantized-integer-operations)).

#### Floating Point Types {#floating-point-types}

Syntax:

``` {.ebnf}
// Floating point.
float-type ::= `f16` | `bf16` | `f32` | `f64`
```

MLIR supports float types of certain widths that are widely used as indicated
above.

#### Vector Type {#vector-type}

Syntax:

``` {.ebnf}
vector-type ::= `vector` `<` const-dimension-list vector-element-type `>`
vector-element-type ::= float-type | integer-type

const-dimension-list ::= (integer-literal `x`)+
```

The vector type represents a SIMD style vector, used by target-specific
instruction sets like AVX. While the most common use is for 1D vectors (e.g.
vector<16 x f32>) we also support multidimensional registers on targets that
support them (like TPUs).

#### Tensor Type {#tensor-type}

Syntax:

``` {.ebnf}
tensor-type ::= `tensor` `<` dimension-list tensor-memref-element-type `>`
tensor-memref-element-type ::= vector-element-type | vector-type

// memref requires a known rank, but tensor does not.
dimension-list ::= dimension-list-ranked | `*` `x`
dimension-list-ranked ::= (dimension `x`)*
dimension ::= `?` | integer-literal
```

SSA values of tensor type represents aggregate N-dimensional data values, and
have a known element type. It may have an unknown rank (indicated by `*`) or may
have a fixed rank with a list of dimensions. Each dimension may be a static
constant or be dynamically determined (indicated by `?`).

The runtime representation of the MLIR tensor type is intentionally abstracted -
you cannot control layout or get a pointer to the data. For low level buffer
access, MLIR has a [`memref` type](#memref-type). This abstracted runtime
representation holds both the tensor data values as well as information about
the (potentially dynamic) shape of the tensor. The
[`dim` operation](#'dim'-operation) returns the size of a dimension from a value
of tensor type.

Examples:

```mlir {.mlir}
// Tensor with unknown rank.
tensor<* x f32>

// Known rank but unknown dimensions.
tensor<? x ? x ? x ? x f32>

// Partially known dimensions.
tensor<? x ? x 13 x ? x f32>

// Full static shape.
tensor<17 x 4 x 13 x 4 x f32>

// Tensor with rank zero. Represents a scalar.
tensor<f32>
```

#### Memref Type {#memref-type}

Syntax:

``` {.ebnf}
memref-type ::= `memref` `<` dimension-list-ranked tensor-memref-element-type
                (`,` semi-affine-map-composition)? (`,` memory-space)? `>`

semi-affine-map-composition ::= (semi-affine-map `,` )* semi-affine-map
memory-space ::= integer-literal /* | TODO: address-space-id */
```

A `memref` type is a reference to a region of memory (similar to a buffer
pointer, but more powerful). The buffer pointed to by a memref can be allocated,
aliased and deallocated. A memref can be used to read and write data from/to the
memory region which it references. Memref types use the same shape specifier as
tensor types, but do not allow unknown rank.

The memory space of a memref is specified by a target-specific integer index. If
no memory space is specified, then the default memory space (0) is used. The
default space is target specific but always at index 0.

TODO: MLIR will eventually have target-dialects which allow symbolic use of
memory hierarchy names (e.g. HBM, VMEM, ...) but we have not spec'd the details
of that mechanism yet. Until then, this document pretends that it is valid to
refer to these memories by `bare_id`.

The notionally dynamic value of a memref value includes the address of the
buffer allocated, as well as the symbols referred to by the shape, layout map,
and index maps.

Examples of memref static type

```mlir {.mlir}
// Identity index/layout map
#imapA = (d0, d1) -> (d0, d1) size (16, 32)

// Column major layout.
#imapB = (d0, d1, d2) [s0] -> (d2, d1, d0) size (s0, 4, 16)

// The dimension list "16x32" defines the following 2D index space:
//
//   { (i, j) : 0 <= i < 16, 0 <= j < 32 }
//
memref<16x32xf32, #imapA, hbm>
// The dimension list "16x4x?" defines the following 3D index space:
//
//   { (i, j, k) : 0 <= i < 16, 0 <= j < 4, 0 <= k < N }
//
// where N is a symbol which represents the runtime value of the size of
// the third dimension.
memref<16x4x?xf32, #imapB, hbm>
```

Symbol capture example:

```mlir {.mlir}
// Affine map with symbol 's0' used as offset for first dimension.
#imapA = (d0, d1) [s0] -> (d0 + s0, d1)
// Allocate memref and bind the following symbols:
// '%n' is bound to the dynamic second dimension of the memref type.
// '%o' is bound to the symbol 's0' in the affine map of the memref type.
%n = ...
%o = ...
%A = <strong>alloc</strong> (%n)[%o] : <16x?xf32, #imapA>
```

##### Index Space {#index-space}

A memref dimension list defines an index space within which the memref can be
indexed to access data.

##### Index {#index}

Data is accessed through a memref type using a multidimensional index into the
multidimensional index space defined by the memref's dimension list.

Examples

```mlir {.mlir}
// Allocates a memref with 2D index space:
//   { (i, j) : 0 <= i < 16, 0 <= j < 32 }
%A = alloc() : memref<16x32xf32, #imapA, hbm>

// Loads data from memref '%A' using a 2D index: (%i, %j)
%v = load %A[%i, %j] : memref<16x32xf32, #imapA, hbm>
```

##### Index Map {#index-map}

An index map is a one-to-one [semi-affine map](#semi-affine-maps) that
transforms a multidimensional index from one index space to another. For
example, the following figure shows an index map which maps a 2-dimensional
index from a 2x2 index space to a 3x3 index space, using symbols `S0` and `S1`
as offsets.

![Index Map Example](includes/img/index-map.svg)

The number of domain dimensions and range dimensions of an index map can be
different, but must match the number of dimensions of the input and output index
spaces on which the map operates. The index space is always non-negative and
integral. In addition, an index map must specify the size of each of its range
dimensions onto which it maps. Index map symbols must be listed in order with
symbols for dynamic dimension sizes first, followed by other required symbols.

Index map examples:

```mlir {.mlir}
// Index map from [MS, NS] slice index space to larger [M, N]
// matrix index space at slice offset symbols OI, OJ:
// Maps from [MS, NS] -> [M, N]
#imap_slice = (i, j) [M, N, OI, OJ] -> (i + OI , j + OJ) size (M, N)

// Index map from 4-dimensional tiled index space to
// 2-dimensional index space.
// Maps from [M/128, N/128, 128, 128] -> [M, N]
#imap_tiled = (d0, d1, d2, d3) [M, N] -> (128 * d0 + d2, 128 * d1 + d3)
                                         size (M, N)
```

##### Layout Map {#layout-map}

A layout map is a [semi-affine map](#semi-affine-maps) which encodes logical to
physical index space mapping, by mapping input dimensions to their ordering from
most-major (slowest varying) to most-minor (fastest varying). Therefore, an
identity layout map corresponds to a row-major layout.

Layout map examples:

```mlir {.mlir}
// MxN matrix stored in row major layout in memory:
#layout_map_row_major = (i, j) [M, N] -> (i, j) size (M, N)

// MxN matrix stored in column major layout in memory:
#layout_map_col_major = (i, j), [M, N] -> (j, i) size (M, N)
```

##### Affine Map Composition {#affine-map-composition}

A memref specifies a semi-affine map composition as part of its type. A
semi-affine map composition is a composition of semi-affine maps beginning with
zero or more index maps, and ending with a layout map. The composition must be
conformant: the number of dimensions of the range of one map, must match the
number of dimensions of the domain of the next map in the composition.

The semi-affine map composition specified in the memref type, maps from accesses
used to index the memref in load/store instructions to other index spaces (i.e.
logical to physical index mapping). Each of the
[semi-affine maps](#semi-affine-maps) and thus its composition is required to be
one-to-one.

The semi-affine map composition can be used in dependence analysis, memory
access pattern analysis, and for performance optimizations like vectorization,
copy elision and in-place updates. If an affine map composition is not specified
for the memref, the identity affine map is assumed.

## Attributes {#attributes}

Syntax:

``` {.ebnf}
attribute-dict ::= `{` `}`
                 | `{` attribute-entry (`,` attribute-entry)* `}`
attribute-entry ::= bare-id `:` attribute-value
```

Attributes are the mechanism for specifying constant data in MLIR in places
where a variable is never allowed - e.g. the index of a
[`dim` operation](#'dim'-operation), or the stride of a convolution.

Attributes have a name, and their values are represented by the following forms:

``` {.ebnf}
attribute-value ::= bool-literal
                  | integer-literal ( `:` (index-type | integer-type) )?
                  | float-literal ( `:` float-type )?
                  | string-literal
                  | affine-map
                  | type
                  | `[` (attribute-value (`,` attribute-value)*)? `]`
                  | function-id `:` function-type
```

It is possible to attach attributes to instructions and functions, and the set
of expected attributes, their structure, and the definition of that meaning is
contextually dependent on the operation they are attached to.

## Module {#module}

``` {.ebnf}
module ::= module-header-def* function*
```

An MLIR module may optionally have a list of header definitions (e.g. affine
mappings) at the top of the file, but is principally made up of a list of
functions.

TODO: We should allow specifying a "dialect" in the module header. This will
prepopulate a symbol table with known named types and mappings (e.g. for TPU)
and will define the set of operations that are allowed (allowing the verifier to
detect common errors).

## Functions {#functions}

MLIR functions have a signature (including argument and result types) and
associated attributes according to the following grammar:

``` {.ebnf}
function ::= `func` function-signature function-attributes? function-body?

function-signature ::= function-id `(` argument-list `)` (`->` type-list)?
argument-list ::= named-argument (`,` named-argument)* | /*empty*/
argument-list ::= type (`,` type)* | /*empty*/ named-argument ::= ssa-id `:`
type

function-attributes ::= `attributes` attribute-dict
function-body ::= `{` block+ `}`
```

An external function declaration (used when referring to a function declared in
some other module) has no body.  A function definition contains a control
flow graph made up of one or more blocks. While the MLIR textual form provides a
nice inline syntax for function arguments, they are internally represented as
"block arguments" to the first block in the function.

Examples:

```mlir {.mlir}
// External function definitions.
func @abort()
func @scribble(i32, i64, memref<? x 128 x f32, #layout_map0>) -> f64

// A function that returns its argument twice:
func @count(%x: i64) -> (i64, i64)
  attributes {fruit: "banana"} {
  return %x, %x: i64, i64
}
```

#### Blocks {#blocks}

Syntax:

``` {.ebnf}
block           ::= bb-label instruction+
bb-label        ::= bb-id bb-arg-list? `:`
bb-id           ::= caret-id
ssa-id-and-type ::= ssa-id `:` type

// Non-empty list of names and types.
ssa-id-and-type-list ::= ssa-id-and-type (`,` ssa-id-and-type)*

bb-arg-list ::= `(` ssa-id-and-type-list? `)`
```

A [basic block](https://en.wikipedia.org/wiki/Basic_block) is a sequential list
of instructions without control flow (calls are not considered control flow for
this purpose) that are executed from top to bottom. The last instruction in a
block is a [terminator operation](#terminator-operations), which ends the block.

Blocks in MLIR take a list of block arguments, which represent SSA PHI nodes in
a functional notation. The arguments are defined by the block, and values are
provided for these block arguments by branches that go to the block.

Here is a simple example function showing branches, returns, and block
arguments:

```mlir {.mlir}
func @simple(i64, i1) -> i64 {
^bb0(%a: i64, %cond: i1): // Code dominated by ^bb0 may refer to %a
  br_cond %cond, ^bb1, ^bb2

^bb1:
  br ^bb3(%a: i64)    // Branch passes %a as the argument

^bb2:
  %b = addi %a, %a : i64
  br ^bb3(%b: i64)    // Branch passes %b as the argument

// ^bb3 receives an argument, named %c, from predecessors
// and passes it on to bb4 twice.
^bb3(%c: i64):
  br ^bb4(%c, %c : i64, i64)

^bb4(%d : i64, %e : i64):
  %0 = addi %d, %e : i64
  return %0 : i64
}
```

**Context:** The "block argument" representation eliminates a number of special
cases from the IR compared to traditional "PHI nodes are instructions" SSA IRs
(like LLVM). For example, the
[parallel copy semantics](http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.524.5461&rep=rep1&type=pdf)
of SSA is immediately apparent, and function arguments are no longer a special
case: they become arguments to the entry block
[[more rationale](Rationale.md#block-arguments-vs-phi-nodes)].

### Instruction Kinds

MLIR has three kinds of instructions: [dialect-defined operations](#operations),
an affine [`for` instruction](#'for'-instruction), and an affine
[`if` instruction](#'if'-instruction).

``` {.ebnf}
inst ::= operation | for-inst | if-inst
```

#### 'for' instruction {#'for'-instruction}

Syntax:

``` {.ebnf}
for-inst ::= `for` ssa-id `=` lower-bound `to` upper-bound
              (`step` integer-literal)? `{` inst* `}`

lower-bound ::= `max`? affine-map dim-and-symbol-use-list | shorthand-bound
upper-bound ::= `min`? affine-map dim-and-symbol-use-list | shorthand-bound
shorthand-bound ::= ssa-id | `-`? integer-literal
```

The `for` instruction represents an affine loop nest, defining an SSA value for
its induction variable. This SSA value always has type [`index`](#index-type),
which is the size of the machine word.

The `for` instruction executes its body a number of times iterating from a lower
bound to an upper bound by a stride. The stride, represented by `step`, is a
positive constant integer which defaults to "1" if not present. The lower and
upper bounds specify a half-open range: the range includes the lower bound but
does not include the upper bound.

The lower and upper bounds of a `for` instruction are represented as an
application of an affine mapping to a list of SSA values passed to the map. The
[same restrictions](#dimensions-and-symbols) hold for these SSA values as for
all bindings of SSA values to dimensions and symbols.

The affine mappings for the bounds may return multiple results, in which case
the `max`/`min` keywords are required (for the lower/upper bound respectively),
and the bound is the maximum/minimum of the returned values. There is no
semantic ambiguity, but MLIR syntax requires the use of these keywords to make
things more obvious to human readers.

Many upper and lower bounds are simple, so MLIR accepts two custom form
syntaxes: the form that accepts a single 'ssa-id' (e.g. `%N`) is shorthand for
applying that SSA value to a function that maps a single symbol to itself, e.g.,
`()[s]->(s)()[%N]`. The integer literal form (e.g. `-42`) is shorthand for a
nullary mapping function that returns the constant value (e.g. `()->(-42)()`).

Example showing reverse iteration of the inner loop:

```mlir {.mlir}
#map57 = (d0)[s0] -> (s0 - d0 - 1)

func @simple_example(%A: memref<?x?xf32>, %B: memref<?x?xf32>) {
  %N = dim %A, 0 : memref<?x?xf32>
  for %i = 0 to %N step 1 {
    for %j = 0 to %N {   // implicitly steps by 1
      %0 = affine_apply #map57(%j)[%N]
      %tmp = call @F1(%A, %i, %0) : (memref<?x?xf32>, index, index)->(f32)
      call @F2(%tmp, %B, %i, %0) : (f32, memref<?x?xf32>, index, index)->()
    }
  }
  return
}
```

#### 'if' instruction {#'if'-instruction}

Syntax:

``` {.ebnf}
if-inst-head ::= `if` if-inst-cond `{` inst* `}`
               | if-inst-head `else` `if` if-inst-cond `{` inst* `}`
if-inst-cond ::= integer-set dim-and-symbol-use-list

if-inst ::= if-inst-head
          | if-inst-head `else` `{` inst* `}`
```

The `if` instruction restricts execution to a subset of the loop iteration space
defined by an integer set (a conjunction of affine constraints). A single `if`
may have a number of optional `else if` clauses, and may end with an optional
`else` clause.

The condition of the `if` is represented by an [integer set](#integer-sets) (a
conjunction of affine constraints), and the SSA values bound to the dimensions
and symbols in the integer set. The [same restrictions](#dimensions-and-symbols)
hold for these SSA values as for all bindings of SSA values to dimensions and
symbols.

Example:

```mlir {.mlir}
#set = (d0, d1)[s0]: (d0 - 10 >= 0, s0 - d0 - 9 >= 0,
                      d1 - 10 >= 0, s0 - d1 - 9 >= 0)
func @reduced_domain_example(%A, %X, %N) : (memref<10xi32>, i32, i32) {
  for %i = 0 to %N {
     for %j = 0 to %N {
       %0 = affine_apply #map42(%j)
       %tmp = call @S1(%X, %i, %0)
       if #set(%i, %j)[%N] {
          %1 = affine_apply #map43(%i, %j)
          call @S2(%tmp, %A, %i, %1)
       }
    }
  }
  return
}
```

## Operations {#operations}

Syntax:

``` {.ebnf}
operation ::= (ssa-id `=`)? string-literal `(` ssa-use-list? `)`
              (`[` successor-list `]`)? attribute-dict? `:` function-type
successor-list ::= successor (`,` successor)*
```

MLIR represents computation within functions with a uniform concept called
_operations_. Operations in MLIR are fully extensible (there is no fixed list of
operations), and have application-specific semantics. For example, MLIR supports
[target-independent operations](#memory-operations),
[high-level TensorFlow ops](#tensorflow-operations), and
[target-specific machine instructions](#target-specific-operations).

The internal representation of an operation is simple: an operation is
identified by a unique string (e.g. `dim`, `tf.Conv2d`, `x86.repmovsb`,
`ppc.eieio`, etc), can return zero or more results, take zero or more SSA
operands, and may have zero or more attributes. When parsed or printed in the
_generic assembly form_, these are all printed literally, and a function type is
used to indicate the types of the results and operands.

Example:

```mlir {.mlir}
// Invoke a TensorFlow function called tf.scramble with two inputs
// and an attribute "fruit".
%2 = "tf.scramble"(%42, %12){fruit: "banana"} : (f32, i32) -> f32

// Invoke the TPU specific add instruction that takes two vector register
// as input and produces a vector register.
%7 = "tpu.add"(%42, %12)
             : (vector<8x128xf32>, vector<8x128xf32>) -> vector<8x128xf32>
```

[Terminator operations](#terminator-operations) may also have a list of
successors ([blocks](#blocks) and their arguments).

Example:

```mlir {.mlir}
// Branch to ^bb1 or ^bb2 depending on the condition %cond.
// Pass value %v to ^bb2, but not to ^bb1.
"br_cond"(%cond)[^bb1, ^bb2(%v : index)] : (i1) -> ()
```

In addition to the basic syntax above, dialects may register tables of known
operations. This allows those dialects to support _custom assembly form_ for
parsing and printing operations. In the operation sets listed below, we show
both forms.

**Context:** TensorFlow has an open "op" ecosystem, and we directly apply these
ideas to the design of MLIR, but generalize it much further. To make it easy to
reason about IR dumps and manipulate operations in C++, the MLIR compiler
infrastructure uses C++ templates to make working with them convenient and safe.
The details of this are not described in this document.

## Standard Operations {#standard-operations}

TODO: shape, which returns a 1D tensor, and can take an unknown rank tensor as
input.

TODO: rank, which returns an index.

#### Terminator operations {#terminator-operations}

Terminator operations are required at the end of each block. They may contain a
list of successors, i.e. other blocks to which the control flow will proceed.
Currently, all terminator operations must be registered in some known
[dialect](#dialects), unlike regular operations.

##### 'br' terminator operation {#'br'-terminator-operation}

Syntax:

``` {.ebnf}
operation ::= `br` successor
successor ::= bb-id branch-use-list?
branch-use-list ::= `(` ssa-use-list `:` type-list-no-parens `)`
```

The `br` terminator instruction represents an unconditional jump to a target
block. The count and types of operands to the branch must align with the
arguments in the target block.

The MLIR branch instruction is not allowed to target the entry block for a
function.

##### 'cond_br' terminator operation {#'cond_br'-terminator-operation}

Syntax:

``` {.ebnf}
operation ::=
  `cond_br` ssa-use `,` successor `,` successor
```

The `cond_br` terminator instruction represents a conditional branch on a
boolean (1-bit integer) value. If the bit is set, then the first destination is
jumped to; if it is false, the second destination is chosen. The count and types
of operands must align with the arguments in the corresponding target blocks.

The MLIR conditional branch instruction is not allowed to target the entry block
for a function. The two destinations of the conditional branch instruction are
allowed to be the same.

The following example illustrates a CFG function with a conditional branch
instruction that targets the same block:

```mlir {.mlir}
func @select(%a : i32, %b :i32, %flag : i1) -> i32 {
    // Both targets are the same, operands differ
    cond_br %flag, ^bb1(%a : i32), ^bb1(%b : i32)

^bb1(%x : i32) :
    return %x : i32
}
```

##### 'return' terminator operation {#'return'-terminator-operation}

Syntax:

``` {.ebnf}
operation ::= `return` (ssa-use-list `:` type-list-no-parens)?
```

The `return` terminator instruction represents the completion of a function, and
produces the result values. The count and types of the operands must match the
result types of the enclosing function. It is legal for multiple blocks in a
single function to return.

### Core Operations {#core-operations}

#### 'affine_apply' operation {#'affine_apply'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `affine_apply` affine-map dim-and-symbol-use-list
```

The `affine_apply` instruction applies an [affine mapping](#affine-expressions)
to a list of SSA values, yielding a single SSA value. The number of dimension
and symbol arguments to affine_apply must be equal to the respective number of
dimensional and symbolic inputs to the affine mapping; the `affine_apply`
instruction always returns one value. The input operands and result must all
have 'index' type.

Example:

```mlir {.mlir}
#map10 = (d0, d1) -> (floordiv(d0,8) + floordiv(d1,128))
...
%1 = affine_apply #map10 (%s, %t)

// Inline example.
%2 = affine_apply (i)[s0] -> (i+s0) (%42)[%n]
```

#### 'call' operation {#'call'-operation}

Syntax:

``` {.ebnf}
operation ::=
    ssa-id `=` `call` function-id `(` ssa-use-list? `)` `:` function-type
```

The `call` operation represents a direct call to a function. The operands and
result types of the call must match the specified function type. The callee is
encoded as a function attribute named "callee".

Example:

```mlir {.mlir}
// Calling the CFG function my_add.
%31 = call @my_add(%0, %1) : (tensor<16xf32>, tensor<16xf32>) -> tensor<16xf32>
```

#### 'call_indirect' operation {#'call_indirect'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `call_indirect` ssa-use
                `(` ssa-use-list? `)` `:` function-type
```

The `call_indirect` operation represents an indirect call to a value of function
type. Functions are first class types in MLIR, and may be passed as arguments
and merged together with block arguments. The operands and result types of the
call must match the specified function type.

Function values can be created with the
[`constant` operation](#'constant'-operation).

Example:

```mlir {.mlir}
%31 = call_indirect %15(%0, %1)
        : (tensor<16xf32>, tensor<16xf32>) -> tensor<16xf32>
```

#### 'dim' operation {#'dim'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `dim` ssa-id `,` integer-literal `:` type
```

The `dim` operation takes a memref or tensor operand and a dimension index, and
returns an ['index'](#index-type) that is the size of that dimension.

The `dim` operation is represented with a single integer attribute named
`index`, and the type specifies the type of the memref or tensor operand.

Examples:

```mlir {.mlir}
// Always returns 4, can be constant folded:
%x = dim %A, 0 : tensor<4 x ? x f32>

// Returns the dynamic dimension of %A.
%y = dim %A, 1 : tensor<4 x ? x f32>

// Equivalent generic form:
%x = "dim"(%A){index: 0} : (tensor<4 x ? x f32>) -> index
%y = "dim"(%A){index: 1} : (tensor<4 x ? x f32>) -> index
```

#### 'reshape' operation {#'reshape'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa_id `=` `reshape` ssa-use `:` memref-type
    dim-and-symbol-use-list `:` memref-type
```

Reshapes the input to the requested shape. The reshape instruction creates a new
memref, but changes how the total dimension size is factored into individual
dimensions sizes as long as the products of the dimension sizes of both shapes
are the same. For example, a `16x16xf32` memref can be reshaped into a
`16x8x2xf32` one, but not to a `16x4xf32` one.

Example:

```mlir {.mlir}
// Allocate base memref with dynamic 16x?xf32.
#lmapD = (i, j)[S0] -> (i, j) size (16, S0)
%D = alloc <16x?xf32, #lmapD, hbm>(%N)[%N]

// Create memref which reshapes from 16x?xf32 to 16x4x?xf32.
#imapDR = (i, j, k)[S0] -> (i, j * S0 + k) size (16, 4 * S0)
%N4 = affine_apply (S -> floordiv(S,4)) (%N)
%DR = reshape %D : memref<16x?xf32, #lmapD, hbm> (%N4)[%N4] to
      (memref<16x?xf32, #lmapD, hbm> -> memref<16x4x?xf32, #imapDR, hbm>)

```

#### 'view' operation {#'view'-operation}

Syntax:

``` {.ebnf}
operation ::=
  `view` memref-type dim-use-list symbol-use-list? ssa-id `:` memref-type
```

Creates a view of a base memref with a potentially different index space. A view
is only defined when its index map maps to a range that is contained in the base
memref's index space. The element type and memory space of a view matches those
of the operand memref.

The view instruction defines a new memref which aliases the buffer of its
operand memref, but in a new index system, specified by the index map in its
type (and any captured symbols). See the figure below for an example.

![2x2 view of 3x3 base MemRef](includes/img/view-operation.svg "Illustration of a 2x2 view of a 3x3 base memref")

Example:

```mlir {.mlir}
#map_b = (i,j)[s0, s1] -> (i + s0, j) size (16, s1)

// %B is a view of %A with a window of size 4 with offset %0 along the
// first dimension. The SSA value %0 is bound to the offset symbol of
// its index map (#map_b)
%n1 = dim %A, 1 : memref<16x?xf32, #map_a, hbm>
%B = view memref<16x?xf32, #map_a, hbm> -> memref<4x?xf32, #map_b, hbm>
          (%n1) [%0, %n1] %A : memref<16x?xf32, #map_a, hbm>

// A view memref that is a dynamic sized slice along an already dynamic
// sized base memref with the slice size being half the base memref's
// dynamic size and with an offset of %0
#map_c = (i,j)[s0, s1] -> (i + s0, j) size (4, s1)
%s1 = "divi"(%n1, 2) : (i32, i32) -> i32
%C = view memref<16x?xf32, #map_a, hbm> -> memref<4x?xf32, #map_c, hbm>
          (%s1) [%0, %n1] %A : memref<16x?xf32, #map_a, hbm>
```

### Memory Operations {#memory-operations}

#### 'alloc' operation {#'alloc'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `alloc` dim-and-symbol-use-list `:` memref-type
```

Allocates a new memref of specified type. Values required for dynamic dimension
sizes are passed as arguments in parentheses (in the same order in which they
appear in the shape signature of the memref) while the symbols required by the
layout map are passed in the square brackets in lexicographical order. If no
layout maps are specified in the memref, then an identity mapping is used.

The buffer referenced by a memref type is created by the `alloc` instruction,
and destroyed by the `dealloc` instruction.

Example:

```mlir {.mlir}
// Allocating memref for a fully static shape.
%A = alloc() : memref<1024x64xf32, #layout_map0, hbm>

// %M, %N, %x, %y are SSA values of integer type.  M and N are bound to the
// two unknown dimensions of the type and x/y are bound to symbols in
// #layout_map1.
%B = alloc(%M, %N)[%x, %y] : memref<?x?xf32, #layout_map1, vmem>
```

#### 'alloc_static' operation {#'alloc_static'-operation}

Syntax:

``` {.ebnf}
operation ::=
    ssa-id `=` `alloc_static` `(` integer-literal `)` :  memref-type
```

Allocates a new memref of specified type with a fixed base pointer location in
memory. 'alloc_static' does not support types that have dynamic shapes or that
require dynamic symbols in their layout function (use the
[`alloc'`instruction](#'alloc'-operation) in those cases).

Example:

```mlir {.mlir}
%A = alloc_static(0x1232a00) : memref<1024 x 64 x f32, #layout_map0, hbm>
```

The `alloc_static` instruction is used to represent code after buffer allocation
has been performed.

#### 'dealloc' operation {#'dealloc'-operation}

Syntax:

``` {.ebnf}
operation ::= `dealloc` ssa-use `:` memref-type
```

Delineates the end of the lifetime of the memory corresponding to a memref
allocation. It is paired with an [`alloc`](#'alloc'-operation) or
[`alloc_static`](#'alloc_static'-operation) instruction.

Example:

```mlir {.mlir}
dealloc %A : memref<128 x f32, #layout, hbm>
```

#### 'dma_start' operation

Syntax:

``` {.ebnf}
operation ::= `dma_start` ssa-use`[`ssa-use-list`]`,
               ssa-use`[`ssa-use-list`]`, ssa-use,
               ssa-use`[`ssa-use-list`]`
              `:` memref-type, memref-type, memref-type
```

Starts a non-blocking DMA operation that transfers data from a source memref to
a destination memref. The operands include the source and destination memref's
each followed by its indices, size of the data transfer in terms of the number
of elements (of the elemental type of the memref), and a tag memref with its
indices. The tag location is used by a dma_wait operation to check for
completion. The indices of the source memref, destination memref, and the tag
memref have the same restrictions as any load/store instruction in an ML
Function (whenever DMA operations appear in ML Functions). This allows powerful
static analysis and transformations in the presence of such DMAs including
rescheduling, pipelining / overlap with computation, and checking for matching
start/end operations. The source and destination memref need not be of the same
dimensionality, but need to have the same elemental type.

For example, a `dma_start` operation that transfers 32 vector elements from a
memref `%src` at location `[%i, %j]` to memref `%dst` at `[%k, %l]` would be
specified as shown below.

Example:

```mlir {.mlir}
%size = constant 32 : index
%tag = alloc() : memref<1 x i32, (d0) -> (d0), 4>
%idx = constant 0 : index
dma_start %src[%i, %j], %dst[%k, %l], %size, %tag[%idx] :
     memref<40 x 8 x vector<16xf32>, (d0) -> (d0), 0>,
     memref<2 x 4 x vector<16xf32>, (d0) -> (d0), 2>,
     memref<1 x i32>, (d0) -> (d0), 4>
```

#### 'dma_wait' operation

Syntax:

```
operation ::= `dma_wait` ssa-use`[`ssa-use-list`]`, ssa-use `:` memref-type
```

Blocks until the completion of a DMA operation associated with the tag element
specified with a tag memref and its indices. The operands include the tag memref
followed by its indices and the number of elements associated with the DMA being
waited on. The indices of the tag memref have the same restrictions as
load/store indices.

Example:

```mlir {.mlir}
dma_wait %tag[%index], %num_elements : memref<1 x i32, (d0) -> (d0), 4>
```

#### 'extract_element' operation {#'extract_element'-operation}

Syntax:

```
operation ::= ssa-id `=` `extract_element` ssa-use `[` ssa-use-list `]` `:` type
```

The `extract_element` op reads a tensor or vector and returns one element from
it specified by an index list. The output of the 'extract_element' is a new
value with the same type as the elements of the tensor or vector. The arity of
indices matches the rank of the accessed value (i.e., if a tensor is of rank 3,
then 3 indices are required for the extract. The indices should all be of
`affine_int` type.

Examples:

```mlir {.mlir}
%3 = extract_element %v[%1, %2] : vector<4x4xi32>
%4 = extract_element %t[%1, %2] : tensor<4x4xi32>
%5 = extract_element %ut[%1, %2] : tensor<*xi32>
```

#### 'load' operation {#'load'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `load` ssa-use `[` ssa-use-list `]` `:` memref-type
```

The `load` op reads an element from a memref specified by an index list. The
output of load is a new value with the same type as the elements of the memref.
The arity of indices is the rank of the memref (i.e., if the memref loaded from
is of rank 3, then 3 indices are required for the load following the memref
identifier).

In an `if` or `for` body, the indices of a load are restricted to SSA values
bound to surrounding loop induction variables,
[symbols](#dimensions-and-symbols), results of a
[`constant` operation](#'constant'-operation), or the result of an
`affine_apply` operation that can in turn take as arguments all of the
aforementioned SSA values or the recursively result of such an `affine_apply`
operation.

Example:

```mlir {.mlir}
%1 = affine_apply (d0, d1) -> (3*d0) (%i, %j)
%2 = affine_apply (d0, d1) -> (d1+1) (%i, %j)
%12 = load %A[%1, %2] : memref<8x?xi32, #layout, hbm>

// Example of an indirect load (treated as non-affine)
%3 = affine_apply (d0) -> (2*d0 + 1)(%12)
%13 = load %A[%3, %2] : memref<4x?xi32, #layout, hbm>
```

**Context:** The `load` and `store` instructions are specifically crafted to
fully resolve a reference to an element of a memref, and (in polyhedral `if` and
`for` instructions) the compiler can follow use-def chains (e.g. through
[`affine_apply`](#'affine_apply'-operation) operations) to precisely analyze
references at compile-time using polyhedral techniques. This is possible because
of the [restrictions on dimensions and symbols](#dimensions-and-symbols) in
these contexts.

#### 'store' operation {#'store'-operation}

Syntax:

``` {.ebnf}
operation ::= `store` ssa-use `,` ssa-use
    `[` ssa-use-list `]` `:` memref-type
```

Store value to memref location given by indices. The value stored should have
the same type as the elemental type of the memref. The number of arguments
provided within brackets need to match the rank of the memref.

In an ML Function, the indices of a store are restricted to SSA values bound to
surrounding loop induction variables, [symbols](#dimensions-and-symbols),
results of a [`constant` operation](#'constant'-operation), or the result of an
[`affine_apply`](#'affine_apply'-operation) operation that can in turn take as
arguments all of the aforementioned SSA values or the recursively result of such
an `affine_apply` operation.

Example:

```mlir {.mlir}
store %100, %A[%1, 1023] : memref<4x?xf32, #layout, hbm>
```

**Context:** The `load` and `store` instructions are specifically crafted to
fully resolve a reference to an element of a memref, and (in polyhedral `if` and
`for` instructions) the compiler can follow use-def chains (e.g. through
[`affine_apply`](#'affine_apply'-operation) operations) to precisely analyze
references at compile-time using polyhedral techniques. This is possible because
of the [restrictions on dimensions and symbols](#dimensions-and-symbols) in
these contexts.

#### 'tensor_load' operation {#'tensor_load'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `tensor_load` ssa-use-and-type
```

Create a tensor from a memref, making an independent copy of the element data.
The result value is a tensor whose shape and element type match the memref
operand.

Example:

```mlir {.mlir}
// Produces a value of tensor<4x?xf32> type.
%12 = tensor_load %10 : memref<4x?xf32, #layout, hbm>
```

#### 'tensor_store' operation {#'tensor_store'-operation}

Syntax:

``` {.ebnf}
operation ::= `tensor_store` ssa-use `,` ssa-use `:` memref-type
```

Stores the contents of a tensor into a memref. The first operand is a value of
tensor type, the second operand is a value of memref type. The shapes and
element types of these must match, and are specified by the memref type.

Example:

```mlir {.mlir}
%9 = dim %8, 1 : tensor<4x?xf32>
%10 = alloc(%9) : memref<4x?xf32, #layout, hbm>
tensor_store %8, %10 : memref<4x?xf32, #layout, hbm>
```

### Arithmetic Operations {#arithmetic-operations}

Basic arithmetic in MLIR is specified by standard operations described in this
section.

TODO: "sub" etc. Let's not get excited about filling this out yet, we can define
these on demand. We should be highly informed by and learn from the operations
supported by HLO and LLVM.

#### 'addi' operation {#'addi'-operation}

Examples:

```mlir {.mlir}
// Scalar addition.
%a = addi %b, %c : i64

// SIMD vector element-wise addition, e.g. for Intel SSE.
%f = addi %g, %h : vector<4xi32>

// Tensor element-wise addition, analogous to HLO's add operation.
%x = addi %y, %z : tensor<4x?xi8>
```

The `addi` operation takes two operands and returns one result, each of these is
required to be the same type. This type may be an integer scalar type, a vector
whose element type is integer, or a tensor of integers. It has no standard
attributes.

#### 'addf' operation {#'addf'-operation}

Examples:

```mlir {.mlir}
// Scalar addition.
%a = addf %b, %c : f64

// SIMD vector addition, e.g. for Intel SSE.
%f = addf %g, %h : vector<4xf32>

// Tensor addition, analogous to HLO's add operation.
%x = addf %y, %z : tensor<4x?xbf16>
```

The `addf` operation takes two operands and returns one result, each of these is
required to be the same type. This type may be a floating point scalar type, a
vector whose element type is a floating point type, or a floating point tensor.

It has no standard attributes.

TODO: In the distant future, this will accept
optional attributes for fast math, contraction, rounding mode, and other
controls.

#### 'cmpi' operation {#'cmpi'-operation}

Examples:

```mlir {.mlir}
// Custom form of scalar "signed less than" comparison.
%x = cmpi "slt", %lhs, %rhs : i32

// Generic form of the same operation.
%x = "cmpi"(%lhs, %rhs){predicate: 2} : (i32, i32) -> i1

// Custom form of vector equality comparison.
%x = cmpi "eq", %lhs, %rhs : vector<4xi64>

// Generic form of the same operation.
%x = "cmpi"(%lhs, %rhs){predicate: 0}
    : (vector<4xi64>, vector<4xi64> -> vector<4xi1>
```

The `cmpi` operation is a generic comparison for integer-like types. Its two
arguments can be integers, vectors or tensors thereof as long as their types
match. The operation produces an i1 for the former case, a vector or a tensor of
i1 with the same shape as inputs in the other cases.

Its first argument is an attribute that defines which type of comparison is
performed. The following comparisons are supported:

-   equal (mnemonic: `"eq"`; integer value: `0`)
-   not equal (mnemonic: `"ne"`; integer value: `1`)
-   signed less than (mnemonic: `"slt"`; integer value: `2`)
-   signed less than or equal (mnemonic: `"slt"`; integer value: `3`)
-   signed greater than (mnemonic: `"sgt"`; integer value: `4`)
-   signed greater than or equal (mnemonic: `"sge"`; integer value: `5`)
-   unsigned less than (mnemonic: `"ult"`; integer value: `6`)
-   unsigned less than or equal (mnemonic: `"ult"`; integer value: `7`)
-   unsigned greater than (mnemonic: `"ugt"`; integer value: `8`)
-   unsigned greater than or equal (mnemonic: `"uge"`; integer value: `9`)

The result is `1` if the comparison is true and `0` otherwise. For vector or
tensor operands, the comparison is performed elementwise and the element of the
result indicates whether the comparison is true for the operand elements with
the same indices as those of the result.

Note: while the custom assembly form uses strings, the actual underlying
attribute has integer type (or rather enum class in C++ code) as seen from the
generic assembly form. String literals are used to improve readability of the IR
by humans.

This operation only applies to integer-like operands, but not floats. The main
reason being that comparison operations have diverging sets of attributes:
integers require sign specification while floats require various floating
point-related particularities, e.g., `-ffast-math` behavior, IEEE754 compliance,
etc ([rationale](Rationale.md#splitting-floating-point-vs-integer-operations)).
The type of comparison is specified as attribute to avoid introducing ten
similar operations, taking into account that they are often implemented using
the same operation downstream ([rationale](Rationale.md#cmpi-predicate)). The
separation between signed and unsigned order comparisons is necessary because of
integers being signless. The comparison operation must know how to interpret
values with the foremost bit being set: negatives in two's complement or large
positives ([rationale](Rationale.md#sign-in-cmpi)).

#### 'constant' operation {#'constant'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `constant` attribute-value `:` type
```

The `constant` operation produces an SSA value equal to some constant specified
by an attribute. This is the way that MLIR uses to form simple integer and
floating point constants, as well as more exotic things like references to
functions and (TODO!) tensor/vector constants.

The `constant` operation is represented with a single attribute named "value".
The type specifies the result type of the operation.

Examples:

```mlir {.mlir}
// Integer constant
%1 = constant 42 : i32

// Reference to function @myfn.
%3 = constant @myfn : (tensor<16xf32>, f32) -> tensor<16xf32>

// Equivalent generic forms
%1 = "constant"(){value: 42} : i32
%3 = "constant"(){value: @myfn}
   : () -> (tensor<16xf32>, f32) -> tensor<16xf32>

```

MLIR does not allow direct references to functions in SSA operands because we
anticipate the desire to multithread the compiler, and disallowing SSA values to
directly reference a function simplifies this
([rationale](Rationale.md#multithreading-the-compiler)).

#### 'divis' operation {#'divis'-operation}

Signed integer division. Rounds towards zero. Treats the leading bit as sign,
i.e. `6 / -2 = -3`.

Note: the semantics of division by zero or signed division overflow (minimum
value divided by -1) is TBD; do NOT assume any specific behavior.

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `divis` ssa-use, ssa-use `:` type
```

Examples:

```mlir {.mlir}
// Scalar signed integer division.
%a = divis %b, %c : i64

// SIMD vector element-wise division.
%f = divis %g, %h : vector<4xi32>

// Tensor element-wise integer division.
%x = divis %y, %z : tensor<4x?xi8>
```

The `divis` operation takes two operands and returns one result, each of these
is required to be the same type. This type may be an integer scalar type, a
vector whose element type is integer, or a tensor of integers. It has no
standard attributes.

#### 'diviu' operation {#'diviu'-operation}

Unsigned integer division. Rounds towards zero. Treats the leading bit as the
most significant, i.e. for `i16` given two's complement representation, `6 /
-2 = 6 / (2^16 - 2) = 0`.

Note: the semantics of division by zero is TBD; do NOT assume any specific
behavior.

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `diviu` ssa-use, ssa-use `:` type
```

Examples:

```mlir {.mlir}
// Scalar unsigned integer division.
%a = diviu %b, %c : i64

// SIMD vector element-wise division.
%f = diviu %g, %h : vector<4xi32>

// Tensor element-wise integer division.
%x = diviu %y, %z : tensor<4x?xi8>
```

The `diviu` operation takes two operands and returns one result, each of these
is required to be the same type. This type may be an integer scalar type, a
vector whose element type is integer, or a tensor of integers. It has no
standard attributes.

#### 'memref_cast' operation

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `memref_cast` ssa-use `:` type `to` type
```

Examples:

```mlir {.mlir}
// Discard static dimension information.
%3 = memref_cast %2 : memref<4x?xf32> to memref<?x?xf32>

// Convert to a type with more known dimensions.
%4 = memref_cast %3 : memref<?x?xf32> to memref<4x?xf32>
```

Convert a memref from one type to an equivalent type without changing any data
elements. The source and destination types must both be memref types with the
same element type, same mappings, same address space, and same rank, yet the
source and destination types may not be the same. The operation is invalid if
converting to a mismatching constant dimension.

#### 'mulf' operation {#'mulf'-operation}

Examples:

```mlir {.mlir}
// Scalar multiplication.
%a = mulf %b, %c : f64

// SIMD pointwise vector multiplication, e.g. for Intel SSE.
%f = mulf %g, %h : vector<4xf32>

// Tensor pointwise multiplication, analogous to HLO's pointwise multiply operation.
%x = mulf %y, %z : tensor<4x?xbf16>
```

The `mulf` operation takes two operands and returns one result, each of these is
required to be the same type. This type may be a floating point scalar type, a
vector whose element type is a floating point type, or a floating point tensor.

It has no standard attributes.

TODO: In the distant future, this will accept
optional attributes for fast math, contraction, rounding mode, and other
controls.

#### 'remis' operation {#'remis'-operation}

Signed integer division remainder. Treats the leading bit as sign, i.e. `6 %
-2 = 0`.

Note: the semantics of division by zero is TBD; do NOT assume any specific
behavior.

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `remis` ssa-use, ssa-use `:` type
```

Examples:

```mlir {.mlir}
// Scalar signed integer division remainder.
%a = remis %b, %c : i64

// SIMD vector element-wise division remainder.
%f = remis %g, %h : vector<4xi32>

// Tensor element-wise integer division remainder.
%x = remis %y, %z : tensor<4x?xi8>
```

The `remis` operation takes two operands and returns one result, each of these
is required to be the same type. This type may be an integer scalar type, a
vector whose element type is integer, or a tensor of integers. It has no
standard attributes.

#### 'remiu' operation {#'remiu'-operation}

Unsigned integer division remainder. Treats the leading bit as the most
significant, i.e. for `i16`, `6 % -2 = 6 % (2^16 - 2) = 6`.

Note: the semantics of division by zero is TBD; do NOT assume any specific
behavior.

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `remiu` ssa-use, ssa-use `:` type
```

Examples:

```mlir {.mlir}
// Scalar unsigned integer division remainder.
%a = remiu %b, %c : i64

// SIMD vector element-wise division remainder.
%f = remiu %g, %h : vector<4xi32>

// Tensor element-wise integer division remainder.
%x = remiu %y, %z : tensor<4x?xi8>
```

The `remiu` operation takes two operands and returns one result, each of these
is required to be the same type. This type may be an integer scalar type, a
vector whose element type is integer, or a tensor of integers. It has no
standard attributes.

#### 'select' operation {#'select'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `select` ssa-use, ssa-use, ssa-use `:` type
```

Examples:

```mlir {.mlir}
// Custom form of scalar selection.
%x = select %cond, %true, %false : i32

// Generic form of the same operation.
%x = "select"(%cond, %true, %false) : (i1, i32, i32) -> i32

// Vector selection is element-wise
%vx = "select"(%vcond, %vtrue, %vfalse)
    : (vector<42xi1>, vector<42xf32>, vector<42xf32>) -> vector<42xf32>
```

The `select` operation chooses one value based on a binary condition supplied as
its first operand. If the value of the first operand is `1`, the second operand
is chosen, otherwise the third operand is chosen. The second and the third
operand must have the same type.

The operation applies to vectors and tensors elementwise given the _shape_ of
all operands is identical. The choice is made for each element individually
based on the value at the same position as the element in the condition operand.

The `select` operation combined with [`cmpi`](#'cmpi'-operation) can be used to
implement `min` and `max` with signed or unsigned comparison semantics.

#### 'tensor_cast' operation {#'tensor_cast'-operation}

Syntax:

``` {.ebnf}
operation ::= ssa-id `=` `tensor_cast` ssa-use `:` type `to` type
```

Examples:

```mlir {.mlir}
// Convert from unknown rank to rank 2 with unknown dimension sizes.
%2 = "tensor_cast"(%1) : (tensor<*xf32>) -> tensor<?x?xf32>
%2 = tensor_cast %1 : tensor<*xf32> to tensor<?x?xf32>

// Convert to a type with more known dimensions.
%3 = "tensor_cast"(%2) : (tensor<?x?xf32>) -> tensor<4x?xf32>

// Discard static dimension and rank information.
%4 = "tensor_cast"(%3) : (tensor<4x?xf32>) -> tensor<?x?xf32>
%5 = "tensor_cast"(%4) : (tensor<?x?xf32>) -> tensor<*xf32>
```

Convert a tensor from one type to an equivalent type without changing any data
elements. The source and destination types must both be tensor types with the
same element type, and the source and destination types may not be the same.
They must either have the same rank, or one may be an unknown rank. The
operation is invalid if converting to a mismatching constant dimension.

## Dialects {#dialects}

MLIR supports multiple dialects containing a set of operations and types defined
together, potentially outside of the main tree. Dialects are produced and
consumed by certain passes. MLIR can be converted between different dialects by
a conversion pass.

Currently, MLIR supports the following dialects:

*   [Standard dialect](#standard-operations)
*   [SuperVector dialect](Dialects/SuperVector.md)
*   [TensorFlow dialect](#tensorflow-operations)

### TensorFlow operations {#tensorflow-operations}

MLIR operations can represent arbitrary TensorFlow operations with a reversible
mapping. Switch and merge nodes are represented with the MLIR control flow
graph. TensorFlow dataflow operations are mapped over to MLIR operations whose
name gets a "tf." prefix.

The normal dtypes supported by TensorFlow are mapped onto a Tensor type with an
unknown rank. The resource and variant dtypes are mapped onto our resource and
variant type specifically (TODO: Specify this). Attributes get mapped over
directly, with type attributes represented as strings.

Examples:

```mlir {.mlir}
// TensorFlow Add operation.
%a = "tf.Add"(%b, %c)
  : (tensor<*xf32>,tensor<*xf32>) -> tensor<*xf32>

// TensorFlow Add operation with partially inferred shapes.
%d = "tf.Add"(%e, %f)
  : (tensor<?x42x?xf32>,tensor<?x42x?xf32>) -> tensor<?x42x?xf32>

// TensorFlow Conv2d operation.
%y = "tf.Conv2d"(%input, %filter)
          {strides: [1,1,2,1], padding: "SAME", dilations: [2,1,1,1]}
   : (tensor<*xf32>, tensor<*xf32>) -> tensor<*xf32>
```

### Target specific operations {#target-specific-operations}

We expect to expose many target-specific (such as TPU-specific) operations
directly through to MLIR.

Example:

```mlir {.mlir}
// TPU vector add instruction
%f = "tpu.vaddf32"(%a, %b)
             : (vector<8x128xf32>, vector<8x128xf32>) -> vector<8x128xf32>
```

In addition to the LLO backend, some targets go through LLVM. LLVM has a rich
set of intrinsics for certain target-independent operations (e.g. addition with
overflow check) as well as providing access to target-specific operations for
the targets it supports (e.g. vector permutation instructions). LLVM intrinsics
start with an "llvm." name.

Example:

```mlir {.mlir}
// LLVM: %x = call {i16, i1} @llvm.sadd.with.overflow.i16(i16 %a, i16 %b)
%x = "llvm.sadd.with.overflow.i16"(%a, %b) : (i16, i16) -> (i16, i1)
```

These operations only work when targeting LLVM as a backend (e.g. for CPUs and
GPUs), and are required to align with the LLVM definition of these intrinsics.

### Dialect specific types {#dialect-specific-types}

Similarly to operations, dialects may define custom extensions to the type
system. These extensions fit within the same type system as described in the
[type system overview](#type-system).

``` {.ebnf}
dialect-type ::= '!' dialect-namespace '<' '"' type-specific-data '"' '>'
```

Example:

```mlir {.mlir}
// LLVM type that wraps around llvm IR types.
!llvm<"i32*">

// Tensor flow string type.
!tf<"string">
```

### TensorFlow types {#tensorflow-types}

The TensorFlow dialect in MLIR defines several extended types:

``` {.ebnf}
// TensorFlow specific types (TODO: the rest ref data types)
type-specific-data ::= `control` | `resource` | `variant` | `string`
               `complex64` | `complex128` | `f32ref`
```

`control` is used in TensorFlow graphs to represent
[control dependence edges](https://docs.google.com/document/d/1Iey7MfrAlBWd0nrHNdnVKvIKRoo8XHsWG5g5pi1iDV4/edit?ts=5b5a0a9f#heading=h.1dv5wuya469j).
