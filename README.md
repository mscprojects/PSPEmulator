# PSPEmulator

An early PSP emulator project with a headless interpreter. It can load and execute the bundled `cpu_alu`, `cpu_branch2`, `cpu_div`, `lsu`, `llsc`, `fpu_branch`, `fpu_branch_hazard`, `fpu`, `roundmode`, `rounding`, `fpu_nan`, and `fcr` PRXs and reproduce their hardware-tested output. It does not yet run games.

## Build

Requires Clang with C++23 support, CMake 3.20 or newer, fmt, and GoogleTest. Scalar FPU execution requires IEEE binary32 host evaluation and floating-point environment support; CMake enables Clang’s strict floating-point mode. The `just` recipes use clang++ and nproc; `ci` also requires clang-format and clang-tidy. `just` runs the common commands:

```sh
just build
just test
just ci
```

To build without `just`:

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel "$(nproc)"
```

`just test` runs the complete test suite, including all bundled PRX hardware-output comparisons.

`just format` and `just format-check` invoke CMake’s `format` and `format-check` targets for C++ files under `src/`, using `clang-format`. `just tidy` runs clang-tidy through CMake’s native `CMAKE_CXX_CLANG_TIDY` integration during a clean parallel build in `build-tidy`, using the repository’s `.clang-tidy` configuration.

`just ci` checks formatting, tests Debug and Release builds with Clang, runs `tidy`, then builds and tests a `RelWithDebInfo` build with address and undefined behavior sanitizers. Debug builds use `-O1` for faster PRX execution while retaining debug symbols and assertions. The sanitizer build retains debug symbols and frame pointers while enabling optimization. Builds and test runs use the CPU count reported by `nproc` for parallel workers.

`psp::Memory` owns zeroed regions and explicit aliases sharing their backing bytes. PRX execution maps configured RAM with its PSP address views and 2 MiB of VRAM at `0x04000000`, also accessible at `0x44000000`. Reads and writes remain little-endian; the CPU applies instruction-specific alignment checks. Nonempty accesses must fit within one mapping and invalid accesses throw `std::out_of_range`. See [guest memory](docs/memory.md) for the layout, ownership, and current limits.

Project tests live beside the code they cover, under each component's `tests/` directory. Upstream PSP tests and the `strong_type` library are pinned in `third_party/` as submodules. After cloning, initialize them with `git submodule update --init --recursive`.

## Execute a PRX

```sh
./build/pspemu third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx
./build/pspemu third_party/pspautotests/tests/cpu/fpu/roundmode.prx
./build/pspemu program.prx --max-instructions 1000000
```

`pspemu` writes captured guest output to stdout. Execution faults and budget exhaustion report the guest PC and instruction on stderr and return a nonzero status. The default budget is 50 million instructions.

Tests and the CLI share `psp::execute_prx()` from `runtime/execution.hpp`. It accepts a self-contained `ParsedPrx` and execution options, initializes guest memory, stack, GP, and a NUL-separated PSP argument block, and returns output, termination status, and the instruction count. It throws on unsupported services, CPU faults, and budget exhaustion. Each invocation owns independent runtime state. Each guest thread owns a `CpuState` containing its integer and scalar floating-point registers, FCR31, HI/LO, and instruction addresses. The runtime shares one `Cpu` interpreter bound to guest memory and executes the selected thread with `cpu.step(state)`.

`Runtime` coordinates execution; `Kernel` owns threads, synchronization, and the shared allocation arena, `GuestIo` captures output, and `SyscallDispatcher` binds imports and translates the guest register ABI. The CPU reports a named `Syscall` event after committing instruction control flow. Guest output structures use named fields and fixed little-endian layouts. See [runtime and syscall handling](docs/runtime.md) for ownership, service inputs and outputs, and scheduling limits.

The runtime runs threads cooperatively until they delay, return, or exit. It supports startup thread creation and status queries, deterministic microsecond time queries and thread delays, periodic HLE vblank interrupts and interrupt masking, user-partition allocation, lightweight mutex creation/deletion and uncontended recursive locking, semaphore creation/deletion and immediately satisfiable waits/signals, standard stream identifiers, console writes, and the PSP autotest emulator device protocol. Guest timezone queries return UTC with daylight saving disabled. Formatting runs inside the PRX's libc; the host captures the bytes it emits. Guest directories are unavailable, and the runtime does not access host files. Blocking synchronization, callbacks, scheduling, allocation reclamation, and the PSP kernel are incomplete; unsupported calls and parameters fail when invoked. Thread status requires the 104-byte structure and reports delayed threads as waiting; scheduling counters remain zero. Guest time advances one microsecond per instruction, a provisional rate, and jumps to the next wakeup when all threads are waiting. Uncalled imports can remain unsupported. The CPU also supports `LL`/`SC` with syscall invalidation; periodic vblank delivery also clears the link bit, and bundled `llsc.prx` now matches its hardware output. Guest interrupt handlers and exception vectors remain unsupported. Scalar FPU transfers, comparisons, branches, arithmetic, square root, and integer conversions are supported, including comparison-to-branch latency, four rounding modes, flush-to-zero, NaN handling, and FCR31 exception flags. Enabled FPU exceptions report host faults before committing the instruction; guest exception entry remains unsupported. There is no graphics, audio, or VFPU support yet.

The integration tests execute the unmodified bundled `cpu_alu.prx`, `cpu_branch2.prx`, `cpu_div.prx`, `lsu.prx`, `llsc.prx`, `fpu_branch.prx`, `fpu_branch_hazard.prx`, `fpu.prx`, `roundmode.prx`, `rounding.prx`, `fpu_nan.prx`, and `fcr.prx` from their ELF entry points at two load addresses, verify successful termination, and compare every output byte. The LSU expectation explicitly includes the final blank line emitted by the guest test but omitted from its bundled `.expected` file; the other expectations match their bundled files directly.

## CPU documentation

See [the Allegrex CPU reference guide](docs/cpu-reference.md) for instruction manuals, PSP-specific encoding notes, implementation references, and supported branching behavior.
