# dspblocks build log

The project kept no build log before 2026-11-12; this file starts it (the kit's
`build.md`, at the root, one line per piece of making, a report at the end of each).

- 2026-11-12, `work/portable-pi`: replace every `M_PI` (POSIX, absent on MSVC) with one
  C++17 constant `dsp::kPi` in `include/dsp/Constants.h`, same literal glibc gives
  `M_PI`, every value bit-identical. Bug: bugs.md, 2026-11-12. No CLAUDE.md, so the
  trunk needs no yes; landed by the kit's `land-branch` (read from claude-packs, not
  installed here).
