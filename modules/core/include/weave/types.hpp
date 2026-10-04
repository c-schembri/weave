#pragma once

#include <climits>

namespace weave {

typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long long i64;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef unsigned char b8;
typedef unsigned short b16;
typedef unsigned int b32;
typedef unsigned long long b64;

typedef float f32;
typedef double f64;

static_assert(CHAR_BIT == 8);
static_assert(sizeof(i8) == 1 && sizeof(i16) == 2 && sizeof(i32) == 4 && sizeof(i64) == 8);
static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
static_assert(sizeof(b8) == 1 && sizeof(b16) == 2 && sizeof(b32) == 4 && sizeof(b64) == 8);
static_assert(sizeof(f32) == 4 && sizeof(f64) == 8);

} // namespace weave
