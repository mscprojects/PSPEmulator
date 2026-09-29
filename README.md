# PSPEmulator

An early PSP emulator project. The current executable is a placeholder; it does not load or run PSP programs yet.

## Build

Requires a C++20 compiler and CMake 3.20 or newer. `just` runs the common commands:

```sh
just build
just format-check
```

To build without `just`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

`just format` formats project C++ files with `clang-format`. Formatting commands leave the upstream test submodule untouched. No project tests are registered yet.

The upstream PSP tests live in `tests/pspautotests`. After cloning, initialize them with `git submodule update --init --recursive`.
