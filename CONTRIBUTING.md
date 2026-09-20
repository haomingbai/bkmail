# Contributing to bkmail

Thanks for your interest in bkmail. This document covers the day-to-day
workflow; the authoritative coding rules live in
[docs/code_style.md](docs/code_style.md) — read it before opening a pull
request.

## Build

bkmail requires CMake ≥ 3.20 and a C++20 compiler. Dependencies (bexec,
bnio, googletest) are resolved automatically — the default `AUTO` provider
fetches them during configure, so a plain build just works:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Useful options: `BKMAIL_BUILD_TESTS` (default on for top-level builds),
`BKMAIL_BUILD_EXAMPLES`, `BKMAIL_ENABLE_TSAN`, `BKMAIL_ENABLE_COVERAGE`.

## Test

```sh
ctest --test-dir build --output-on-failure
```

All tests must pass before submitting. Add tests for every behavior change;
tests are GoogleTest binaries under `tests/`, discovered via
`gtest_discover_tests`.

## Format

Code is formatted with `clang-format` using the repository root
`.clang-format` (`BasedOnStyle: Google`). Format everything under
`include/`, `src/`, `tests/`, and `examples/` before committing:

```sh
find include src tests examples -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) \
  -exec clang-format -i {} +
```

To verify without modifying files, replace `-i` with `--dry-run --Werror`.
CI runs exactly this check and reports any diff in the job log; the job is
continue-on-error, so a formatting diff does not fail the build — fix it
anyway before review.

## Pull requests

- Keep changes focused; one logical change per PR.
- Follow [docs/code_style.md](docs/code_style.md): file banners, naming,
  include order, one-type-per-file, allocator support.
- Ensure the CI matrix (Linux/macOS × Debug/Release × gcc/clang, with gcc
  excluded on macOS, plus a separate ASan+UBSan job) is green. The format
  check reports diffs but does not fail the build.
- Write commit messages in English, imperative mood, explaining *why*.
