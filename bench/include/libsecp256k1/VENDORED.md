# Vendored libsecp256k1

Upstream: https://github.com/bitcoin-core/secp256k1
Commit: `1b4181f` ("Merge bitcoin-core/secp256k1#1978", master, cloned
2026-10-29). License: MIT, see `COPYING`.

## What was copied

- `include/secp256k1.h`, `include/secp256k1_preallocated.h` (public API)
- `src/secp256k1.c`, `src/precomputed_ecmult.c`,
  `src/precomputed_ecmult_gen.c` (the only translation units built)
- `src/*.h` (the library's internal headers), except test/bench-only
  headers (`bench.h`, `tests_common.h`, `testrand.h`, `testrand_impl.h`,
  `testutil.h`, `unit_test.h`)

NOT copied: `src/modules/` (no module is enabled, see below),
`src/asm/` (ARM-only), tests, benchmarks, examples, tools, `sage/`.

## How it is built

The bench tool copies this directory flat into the build root, so the
library lands as `include/` + `src/` next to `app.c` / `app.cpp`.
The build compiles the three translation units above with:

- `-DSECP256K1_WIDEMUL_INT128` — 64-bit field arithmetic (5x52 limbs)
  and 4x64 scalars, the library's standard fast path on x86-64 with
  `__int128`. No other configuration define is needed: the window size
  defaults to 15 in `src/ecmult.h`, and the default callbacks are
  defined inside the library unless `USE_EXTERNAL_DEFAULT_CALLBACKS`
  is set (it is not).
- No `ENABLE_MODULE_*` define is set, so no module code is compiled;
  the benchmark only uses the core API (`ec_pubkey_create`,
  `ec_pubkey_tweak_mul`, `ec_pubkey_serialize`).

This version of the library has no generated `libsecp256k1-config.h`
(the old autotools config header is gone); configuration is entirely
via the defines above, taken from the library's own `CMakeLists.txt`
and header defaults — no crypto code was hand-written here.

## Regenerating

Re-clone upstream, check out the commit above (or a newer one), and
re-copy the same file sets. If upstream renames files, update
`app_libsecp.rsp` / `app_libsecp_cpp.rsp` (C/C++ builds) accordingly.
