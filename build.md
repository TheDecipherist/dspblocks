# dspblocks build log

The project kept no build log before 2026-11-12; this file starts it (the kit's
`build.md`, at the root, one line per piece of making, a report at the end of each).

- 2026-11-12, `work/portable-pi`: replace every `M_PI` (POSIX, absent on MSVC) with one
  C++17 constant `dsp::kPi` in `include/dsp/Constants.h`, same literal glibc gives
  `M_PI`, every value bit-identical. Bug: bugs.md, 2026-11-12. No CLAUDE.md, so the
  trunk needs no yes; landed by the kit's `land-branch` (read from claude-packs, not
  installed here).

### Report, 2026-11-12, `work/portable-pi`

**Commits:** 1d03fd1 (start build.md and bugs.md, log the bug), e25c4c3 (`dsp::kPi` in
`include/dsp/Constants.h`; all 69 `M_PI` tokens in include/, tests/, tools/ and probes/
replaced, the `#define M_PI` blocks in JpFilter.h and JpOsc.h removed), and this report.
There is no src/. A search of include, tests, tools and probes finds no `M_PI`.

**Bit-identity.** The tests are behavioural and mostly print values to a few places, so
they were not relied on. Instead everything was built and run before (a pristine
`origin/main` worktree) and after, with GCC 13 in two configurations, CMake's default
(no optimisation) and Release (-O3): the three test binaries, `dsp_reverb_ab` and the
six piano probes. In both configurations, before and after:
- every test and probe output is byte-identical (only `reverb_ab`'s line naming its
  output folder differs, which is the folder name);
- all 8 WAVs `dsp_reverb_ab` renders are byte-identical;
- the disassembly of all 10 binaries is identical (addresses masked), so the code that
  computes every value is the same machine code;
- the pass/fail results are identical.

**Pre-existing reds, the same before and after:** `dsp_tests` fails one check, "ladder
with stage saturation OFF is unchanged" (golden value to 1e-6, this machine misses it),
in both configurations; the piano probes are red as todo.md says; `dsp_fdn_measure`
does not build (missing `<string>`). Both new faults are logged open in bugs.md. The
tests the change reaches are all three ctest binaries; `dsp_delay_designer_tests` and
`dsp_superfilter_tests` pass. Landed with `dsp_tests` red, because it is red on main
already, its output is unchanged byte for byte, and fixing it was outside this order.

**MSVC:** cl 19.44.35228 x64 (VS 2022 Build Tools, vcvars64.bat via cmd.exe from WSL),
`/std:c++17 /EHsc /permissive- /W3`, one translation unit including every header plus
one per header. `origin/main`: 17 of the 25 one-header units and the all-headers unit fail,
every error `C2065 'M_PI': undeclared identifier` or its knock-on (`C2737`, the
const it left uninitialised). Branch: all 27 units (including Constants.h) compile with
no errors and no warnings. GCC builds every target it built before.

**Left as found:** `Oversampler.h` keeps its own class-member `kPi` (same literal, never
used `M_PI`); folding it into `dsp::kPi` would be a change this order did not allow.
Consumers on GCC/Clang that relied on JpFilter.h/JpOsc.h for `M_PI` still get it from
<cmath>; on MSVC nothing did, since every other header already failed there.
