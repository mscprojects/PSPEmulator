# PSPEmulator

An early PSP emulator project with an interpreter and an optional SDL3 display. It can load and execute the bundled `cpu_alu`, `cpu_branch2`, `cpu_div`, `lsu`, `llsc`, `fpu_branch`, `fpu_branch_hazard`, `fpu`, `roundmode`, `rounding`, `fpu_nan`, and `fcr` PRXs and reproduce their hardware-tested output. It also runs the unmodified PSPSDK console and screen Hello World PRXs through startup and cleanup, and its controller sample with keyboard input and a guest exit callback. It does not yet run games.

## Build

Requires Clang with C++23 support, CMake 3.20 or newer, fmt, SDL3 3.4.2 development files, and GoogleTest. Scalar FPU execution requires IEEE binary32 host evaluation and floating-point environment support; CMake enables Clang’s strict floating-point mode. The `just` recipes use clang++ and nproc; `ci` also requires clang-format and clang-tidy. `just` runs the common commands:

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

Project tests live beside the code they cover, under each component's `tests/` directory. Upstream PSP tests, PSPSDK, and the `strong_type` library are pinned in `third_party/` as submodules. After cloning, initialize them with `git submodule update --init --recursive`. The SDK homebrew test fixtures are stored in Git LFS; install Git LFS and run `git lfs pull` to download them if they were not fetched during cloning. Their [provenance and rebuild instructions](src/runtime/tests/fixtures/README.md) are included beside the PRXs.

## Build homebrew PRXs

The optional homebrew recipes require Ubuntu 24.04 x86_64, GNU Make, curl, tar, and sha256sum. `just setup-pspdev` installs the [PSPDEV v20261001 Ubuntu release](https://github.com/pspdev/pspdev/releases/tag/v20261001) under `~/.local/opt/pspdev/v20261001/`. It downloads the archive into `${XDG_CACHE_HOME:-$HOME/.cache}/pspdev/v20261001/`, verifies its pinned SHA-256, and checks that the compiler runs before installing it. Subsequent setup runs reuse the installation. The version, download URL, and checksum are pinned in `justfile`; the PSPSDK submodule matches the SDK revision recorded in that release's `build.txt`.

```sh
just setup-pspdev
just homebrew
```

`just homebrew` also runs setup, then builds the unmodified SDK [Hello World PRX template](third_party/pspsdk/src/samples/template/prx_template/main.c), [screen Hello World template](third_party/pspsdk/src/samples/template/elf_template/main.c), and [controller sample](third_party/pspsdk/src/samples/controller/basic/main.c). Outputs are `build-homebrew/template/prx_template/template.prx`, `build-homebrew/template/elf_template/template.prx`, and `build-homebrew/controller/basic/controller_basic.prx`. Sources and Makefiles are copied into the ignored build directory so building leaves the submodule clean. These recipes set `PSPDEV` and `PATH` only for their own commands; the normal emulator build and `just ci` do not require the PSP toolchain.

The Hello World template runs successfully and prints `Hello World` before exiting. The screen template displays `Hello World` in an SDL window. The controller sample displays button presses and analog coordinates and exits through its callback when Home is pressed.

## Execute a PRX

```sh
./build/pspemu third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx
./build/pspemu third_party/pspautotests/tests/cpu/fpu/roundmode.prx
./build/pspemu src/runtime/tests/fixtures/hello_world.prx
./build/pspemu src/runtime/tests/fixtures/screen_hello_world.prx --window
./build/pspemu src/runtime/tests/fixtures/controller_basic.prx --window
./build/pspemu program.prx --max-instructions 1000000
```

`pspemu` writes captured guest output to stdout. Execution faults and budget exhaustion report the guest PC and instruction on stderr and return a nonzero status. The default budget is 50 million instructions.

Headless execution is the default. `--window` opens a resizable 960 × 544 SDL3 window with sharp scaling and a preserved 480:272 aspect ratio. The frontend presents the guest's RGBA 8888 framebuffer, treating its alpha as opaque. It retains the final image after guest termination; press Escape or close the window to exit. These controls also stop active execution. Options may appear in either order after the PRX path. Closing an active guest returns zero and preserves console output captured so far; after guest termination the guest exit code is retained. Faults still report guest instruction context and exit immediately.

Keyboard controls use physical key positions:

- Arrow keys: D-pad; WASD: left analog stick, with opposite directions cancelling to neutral.
- I/J/K/L: Triangle/Square/Cross/Circle; Q/E: left/right trigger.
- Enter: Start; Backspace: Select.
- Home: request exit through the guest's registered callback. The final frame stays open until Escape or window close.

Key release and focus loss clear held input. The right analog stick stays neutral in the keyboard frontend. Controller sampling uses guest vblank time, supports digital and analog modes, and accepts one positive-read sample per call. The core keeps the latest unread sample; a read without fresh data waits for a future sample. Nonzero sampling cycles, multi-sample reads, and other controller services are unsupported.

Guest display timing uses the deterministic instruction clock and periodic vblank at 60000/1001 Hz. SDL and emulation run on the main thread, with handoffs at vblank, including when threads sleep or interrupts are masked. Host pacing uses fixed-origin monotonic deadlines independently of monitor refresh. When behind, guest work and display events are retained while intermediate host presentations may be skipped. The core and its tests never initialize SDL; separate frontend tests use SDL's offscreen video driver, software renderer, and CPU framebuffer surfaces without a desktop session or host GPU drivers.

Tests and the CLI share `psp::Execution` from `runtime/execution.hpp`; the headless `psp::execute_prx()` convenience function runs its incremental interface to completion. The convenience function accepts a self-contained `ParsedPrx` and execution options, initializes guest memory, stack, GP, and a NUL-separated PSP argument block, and returns output, termination status, and the instruction count. It throws on unsupported services, CPU faults, and budget exhaustion. Each invocation owns independent runtime state. Each guest thread owns a `CpuState` containing its integer and scalar floating-point registers, FCR31, HI/LO, and instruction addresses. The runtime shares one `Cpu` interpreter bound to guest memory and executes the selected thread with `cpu.step(state)`.

`Runtime` coordinates execution; `Display` owns framebuffer selections and captured pixels; `Controller` samples input and completes controller reads; `Kernel` owns threads, synchronization, and the shared allocation arena, `GuestIo` captures output, and `SyscallDispatcher` binds imports and translates the guest register ABI. The CPU reports a named `Syscall` event after committing instruction control flow. Guest output structures use named fields and fixed little-endian layouts. See [runtime and syscall handling](docs/runtime.md) for ownership, service inputs and outputs, and scheduling limits.

The runtime selects ready threads by PSP priority, with FIFO ties. A higher-priority ready thread preempts at an instruction boundary; equal-priority threads run until they wait, return, or exit. It supports startup thread creation and status queries, deterministic microsecond time queries and thread delays, periodic HLE vblank interrupts and interrupt masking, user-partition allocation and freeing, lightweight mutex creation/deletion and uncontended recursive locking, semaphore creation/deletion and immediately satisfiable waits/signals, standard stream identifiers, console writes, and the PSP autotest emulator device protocol. Partition blocks and thread stacks share an allocator with 256-byte alignment, low/high placement, reuse and merging of freed blocks, and separate total/largest-free queries. Guest timezone queries return UTC with daylight saving disabled. Formatting runs inside the PRX's libc; the host captures the bytes it emits. Guest directories are unavailable, and the runtime does not access host files. Blocking synchronization, general callback services, equal-priority time slicing, thread-stack reclamation, and the PSP kernel are incomplete; unsupported calls and parameters fail when invoked. Thread status requires the 104-byte structure and reports delayed threads as waiting; scheduling counters remain zero. Guest time advances one microsecond per instruction, a provisional rate, and advances through vblank boundaries while threads wait. Callback creation, exit registration, and callback-enabled sleep support the controller sample: Home queues a notification, runs the guest function on its owner thread, and restores that thread to sleep on return. Uncalled imports can remain unsupported. The CPU also supports `LL`/`SC` with syscall invalidation; periodic vblank delivery also clears the link bit, and bundled `llsc.prx` now matches its hardware output. Guest interrupt handlers and exception vectors remain unsupported. Scalar FPU transfers, comparisons, branches, arithmetic, square root, and integer conversions are supported, including comparison-to-branch latency, four rounding modes, flush-to-zero, NaN handling, and FCR31 exception flags. Enabled FPU exceptions report host faults before committing the instruction; guest exception entry remains unsupported. Display supports LCD 480 × 272 and RGBA 8888 framebuffers; GE command processing, other pixel formats, accurate LCD scanout, audio, and VFPU remain unsupported.

The integration tests execute the unmodified bundled `cpu_alu.prx`, `cpu_branch2.prx`, `cpu_div.prx`, `lsu.prx`, `llsc.prx`, `fpu_branch.prx`, `fpu_branch_hazard.prx`, `fpu.prx`, `roundmode.prx`, `rounding.prx`, `fpu_nan.prx`, and `fcr.prx` from their ELF entry points at two load addresses, verify successful termination, and compare every output byte. The LSU expectation explicitly includes the final blank line emitted by the guest test but omitted from its bundled `.expected` file; the other expectations match their bundled files directly. The SDK Hello World PRX is also tested at two load addresses, requiring exactly `Hello World\n` and successful termination within one million instructions.

The screen Hello World fixture is also tested at two load addresses against an independently established full-frame pixel expectation. The controller fixture is tested at two load addresses against frozen font expectations for neutral axes, changed axes, and every button, followed by a guest callback exit. SDL tests verify rendered pixels, sharp scaling, resizing, final-frame retention, keyboard mapping and release, focus loss, Home callback exit, and Escape/close handling during execution.

## CPU documentation

See [the Allegrex CPU reference guide](docs/cpu-reference.md) for instruction manuals, PSP-specific encoding notes, implementation references, and supported branching behavior.
