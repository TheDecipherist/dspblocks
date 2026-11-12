# dspblocks bug log

The project kept no bug log before 2026-11-12; this file starts it.

## 2026-11-12: the headers do not compile with MSVC: `M_PI` is not standard C++

**Seen:** building BANDWARP with Visual Studio 2022 on 2026-11-12:
`'M_PI': undeclared identifier` in `include/dsp/Drive.h`.

**Cause:** `M_PI` is POSIX, not ISO C++. glibc's `<cmath>` defines it (g++ always
defines `_GNU_SOURCE`) and so does Clang on Unix, so GCC and Clang never saw it; MSVC
defines it only under `_USE_MATH_DEFINES`. Used about 45 times across `include/dsp/`
(Pitch, Modulation, Delay, Sampler, DelayDesigner, EarlyPatterns, Drive, JpFilter,
Measure, JpOsc, Filters) and in tests, tools and probes. `JpFilter.h` and `JpOsc.h`
each `#define M_PI` themselves when missing; every other header does not, so any unit
including them fails on Windows.

**Fix:** one constant, `dsp::kPi = 3.14159265358979323846` (exactly glibc's `M_PI`
literal, so every computed value is bit-identical) in `include/dsp/Constants.h`; every
`M_PI` uses it with each expression's type kept; the two local `#define M_PI` blocks
removed. Branch `work/portable-pi`, commit e25c4c3.

**Status:** fixed 2026-11-12. All 26 headers compile with MSVC 19.44 (VS 2022); every
test, probe and render output is byte-identical to before (build.md, 2026-11-12 report).

## 2026-11-12: `dsp_tests` is red on main: "ladder with stage saturation OFF is unchanged"

**Seen:** while taking the baseline for the `M_PI` fix, on untouched `origin/main`
(14e6670), GCC on WSL2, both the default and Release configurations:
`FAIL ladder with stage saturation OFF is unchanged (got 143.142, wanted 143.142)`.
The check (`tests/test_blocks.cpp`, the ladder fingerprint) wants 143.141899516 within
1e-6; this machine's build lands outside that, though it prints the same to three places.

**Cause:** not diagnosed. It is not the `M_PI` fix: the binaries before and after that
fix are the same machine code and print the same bytes.

**Status:** open. Not fixed on `work/portable-pi`, whose order was to change nothing else.

## 2026-11-12: `tools/fdn_measure.cpp` does not build

**Seen:** `cmake --build build --target dsp_fdn_measure` on `origin/main` (14e6670):
`'string' is not a member of 'std'` at line 66: the file uses `std::string` without
`#include <string>`.

**Status:** open. Not fixed on `work/portable-pi` (nothing else changes there).
