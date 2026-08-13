# tinycc (vendored snapshot)

A pinned copy of TinyCC's `mob` branch, used as the runtime C99 JIT for
texture scripts (`documentation/plans/texture-scripts.md`, milestone T3).
Copied source like `extern/glfw` — **not** a submodule.

- Upstream: https://github.com/TinyCC/tinycc (mirror of repo.or.cz/tinycc.git)
- Branch: `mob` (0.9.27 predates the arm64/Mach-O backends; the mob branch is
  where upstream development lives)
- Pinned commit: `2ba12e83b3599ca8f5d50c179fe5138fe956f0c9` (2026-08-09)
- License: LGPL-2.1 (`COPYING`; `RELICENSING` tracks upstream's MIT effort) —
  compatible with the engine's GPL-2.0-or-later.

## What differs from upstream

- **Excluded**: `tests/`, `examples/`, `win32/` (tcc's own Windows headers +
  import libs), `configure`, `Makefile`, docs/build machinery. The engine
  embeds libtcc for in-memory JIT only (`tcc_compile_string` →
  `tcc_relocate` → `tcc_get_symbol`); it never installs tcc, compiles to
  files, or ships tcc's headers/libs. Generated texture-script C99 includes
  nothing and gets its libc/math symbols via `tcc_add_symbol`.
- **Added, ours**: this file, `config.h` (upstream generates it with
  `./configure`; ours pins `TCC_VERSION` and leaves the rest to the in-tree
  defaults — `tcc.h` autodetects the native target/OS when no
  `TCC_TARGET_*` is predefined).
- **Added, generated**: `tccdefs_.h` — `include/tccdefs.h` converted to C
  strings, normally produced by the Makefile. Regenerate after touching
  `include/tccdefs.h`:

  ```
  cc -DC2STR conftest.c -o c2str && ./c2str include/tccdefs.h tccdefs_.h
  ```

  (`conftest.c`'s converter is a pure text transform; the output is
  host-independent.)

## How it builds

`extern/CMakeLists.txt` defines the `tinycc` static library for native
non-WASM builds: the ONE_SOURCE model — compile `libtcc.c` alone with
`-DONE_SOURCE=1` (it `#include`s the lexer/codegen/target files itself),
backtrace/bounds-check off. No WASM tcc: precompiled kernels already work
there.

## Updating the snapshot

Clone upstream `mob`, note the new commit, re-copy the file set above
(everything top-level plus `include/` and `lib/`, minus the exclusions),
regenerate `tccdefs_.h`, update the pin here, and re-run `test_tcc_jit`
on every platform CI covers.
