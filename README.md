# PSPEmulator

An early PSP emulator project with a headless interpreter. It can load and execute the bundled `cpu_alu` PRX and reproduce its complete expected output. It does not yet run games.

## Build

Requires a C++23 compiler, CMake 3.20 or newer, fmt, and GoogleTest. The `ci` recipe also requires clang++, clang-format, and clang-tidy. `just` runs the common commands:

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

`just format` formats C++ files under `src/` with `clang-format`. `just ci` checks formatting, tests with GCC and Clang, runs clang-tidy on project source and headers, then builds and tests with address and undefined behavior sanitizers.

The first core component is a single contiguous `psp::Memory` region. It accepts `psp::GuestAddress` values, starts zeroed, reads and writes 8-, 16-, and 32-bit values in little-endian order, permits unaligned access, and throws `std::out_of_range` when an access crosses its boundaries. The CPU will later decide which accesses require alignment checks.

Project tests live beside the code they cover, under each component's `tests/` directory. Upstream PSP tests and the `strong_type` library are pinned in `third_party/` as submodules. After cloning, initialize them with `git submodule update --init --recursive`.

## Execute a PRX

```sh
./build/pspemu third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx
./build/pspemu program.prx --max-instructions 1000000
```

`pspemu` writes captured guest output to stdout. Execution faults and budget exhaustion report the guest PC and instruction on stderr and return a nonzero status. The default budget is 50 million instructions.

Tests and the CLI share `psp::execute_prx()` from `runtime/execution.hpp`. It accepts a self-contained `ParsedPrx` and execution options, initializes guest memory, stack, GP, and a NUL-separated PSP argument block, and returns output, termination status, and the instruction count. It throws on unsupported services, CPU faults, and budget exhaustion. Each invocation owns independent runtime state.

The runtime runs threads cooperatively until they return or exit. It supports startup thread creation, user-partition allocation, standard stream identifiers, console writes, and the PSP autotest emulator device protocol. Formatting runs inside the PRX's libc; the host captures the bytes it emits. Guest directories are unavailable, and the runtime does not access host files. Scheduling, allocation reclamation, and the PSP kernel are incomplete; unsupported calls fail when invoked. Uncalled imports can remain unsupported. There is no graphics, audio, FPU, or VFPU support yet.

The integration test executes the unmodified bundled `cpu_alu.prx` from its ELF entry point at two load addresses, verifies successful termination, and compares every output byte with `cpu_alu.expected`.

## CPU documentation

See [the Allegrex CPU reference guide](docs/cpu-reference.md) for instruction manuals, PSP-specific encoding notes, implementation references, and supported branching behavior.
