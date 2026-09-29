# PSPEmulator

An early PSP emulator project. The current executable is a placeholder; it does not load or run PSP programs yet.

## Build

Requires a C++23 compiler, CMake 3.20 or newer, fmt, and GoogleTest. The `ci` recipe also requires clang-format and clang-tidy. `just` runs the common commands:

```sh
just build
just test
just ci
```

To build without `just`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

`just format` formats C++ files under `src/` with `clang-format`. `just ci` checks formatting, builds and runs the tests, runs clang-tidy, then builds and tests with address and undefined behavior sanitizers.

The first core component is a single contiguous `psp::Memory` region. It starts zeroed, reads and writes 8-, 16-, and 32-bit values in little-endian order, permits unaligned access, and throws `std::out_of_range` when an access crosses its boundaries. The CPU will later decide which accesses require alignment checks.

Project tests live beside the code they cover, under each component's `tests/` directory. The upstream PSP tests live in `third_party/pspautotests`. After cloning, initialize them with `git submodule update --init --recursive`.
