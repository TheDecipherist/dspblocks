// Numeric constants shared by every block.
//
// The <cmath> pi macro is POSIX, not C++: glibc and Clang on Unix define it, MSVC does
// not unless `_USE_MATH_DEFINES` is set before the first <cmath>, so a header that names
// it fails to compile on Windows (bugs.md, 2026-11-12). The project is C++17, so
// `std::numbers::pi` is not available either. This is the one place pi is written down;
// every block uses it.
//
// The literal is exactly the one glibc gives that macro, so every value computed from
// it is bit-identical to what the macro produced. Being a `double`, it keeps every
// expression's type: `(float) kPi` is the same float conversion of the same double.
#pragma once

namespace dsp {

inline constexpr double kPi = 3.14159265358979323846;

} // namespace dsp
