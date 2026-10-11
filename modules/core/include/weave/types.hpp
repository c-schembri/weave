#pragma once

#include <climits>

namespace weave {

/// Signed 8-bit integer.
typedef signed char i8;
/// Signed 16-bit integer.
typedef signed short i16;
/// Signed 32-bit integer.
typedef signed int i32;
/// Signed 64-bit integer.
typedef signed long long i64;

/// Unsigned 8-bit integer.
typedef unsigned char u8;
/// Unsigned 16-bit integer.
typedef unsigned short u16;
/// Unsigned 32-bit integer.
typedef unsigned int u32;
/// Unsigned 64-bit integer.
typedef unsigned long long u64;

/// 8-bit integer-backed boolean storage: zero is false, nonzero true; not a bitmask or bool replacement.
typedef unsigned char b8;
/// 16-bit integer-backed boolean storage: zero is false, nonzero true.
typedef unsigned short b16;
/// 32-bit integer-backed boolean storage: zero is false, nonzero true.
typedef unsigned int b32;
/// 64-bit integer-backed boolean storage: zero is false, nonzero true.
typedef unsigned long long b64;

/// 32-bit floating-point storage.
typedef float f32;
/// 64-bit floating-point storage.
typedef double f64;

static_assert(CHAR_BIT == 8);
static_assert(sizeof(i8) == 1 && sizeof(i16) == 2 && sizeof(i32) == 4 && sizeof(i64) == 8);
static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
static_assert(sizeof(b8) == 1 && sizeof(b16) == 2 && sizeof(b32) == 4 && sizeof(b64) == 8);
static_assert(sizeof(f32) == 4 && sizeof(f64) == 8);

} // namespace weave
