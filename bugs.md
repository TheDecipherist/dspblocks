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
removed. Branch `work/portable-pi`.

**Status:** open.
