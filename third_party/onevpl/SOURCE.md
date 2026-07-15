# Vendored oneVPL — trimmed copy

This is a **trimmed** in-tree copy of Intel's oneVPL (libvpl) dispatcher, vendored
so AetherFlow's Windows backend builds out of the box (`add_subdirectory(third_party/onevpl)`
in the root `CMakeLists.txt`). Upstream version: see [`version.txt`](version.txt).
Upstream project: https://github.com/intel/libvpl (MIT, see `LICENSE`).

## What was removed (AetherFlow reorg, 2026-06-11)

To keep the repository lean, the parts AetherFlow does **not** build were
removed (~560 files). None are referenced by the build that produces the `VPL`
library AetherFlow links:

| Removed | Why safe |
|---|---|
| `examples/` | Only built when `BUILD_EXAMPLES=ON` (default OFF). Its one unconditional `add_subdirectory(examples)` was removed from `CMakeLists.txt`. |
| `libvpl/test/` (incl. bundled googletest) | Only added under `if(BUILD_TESTS)`; `BUILD_TESTS` defaults **OFF**. |
| `doc/`, `.github/`, `script/` | Referenced by no CMake path (docs / CI / dev scripts). |
| root lint/CI/meta files (`.clang-format`, `.pylintrc`, `bandit.yaml`, `CHANGELOG.md`, `CONTRIBUTING.md`, `README.md`, `INSTALL.md`, `SECURITY.md`, …) | Upstream repo hygiene, not needed to consume oneVPL as a library. |

## What was kept (build-required)

`api/`, `libvpl/` (dispatcher core, minus `test/`), `cmake/`, `env/`,
`CMakeLists.txt`, `version.txt`, `LICENSE`, `third-party-programs.txt`.

## Constraint

`BUILD_TESTS` must stay **OFF** (the default) — `libvpl/test/` is gone. To restore
the full upstream tree, re-fetch the `version.txt` tag from the upstream repo.
