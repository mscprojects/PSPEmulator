# PSPEmulator

An early PSP emulator: an Allegrex interpreter, a high-level PSP runtime, and an optional SDL3 window. It does not run games yet.

What runs today:

- Bundled hardware tests `cpu_alu`, `cpu_branch2`, `cpu_div`, `lsu`, `llsc`, `fpu_branch`, `fpu_branch_hazard`, `fpu`, `roundmode`, `rounding`, `fpu_nan`, and `fcr`, matching their hardware output byte for byte.
- Bundled GPU tests `gpu/filtering/precisionnearest2d` and `precisionlinear2d`: textured 2D sprites with nearest and bilinear filtering, clamp addressing, flips, and subpixel offsets, matching the full hardware output.
- Bundled GPU test `gpu/commands/blend`: all six 2D sprite blend operations and their source/destination factors, with ALWAYS/REPLACE stencil, matching the full hardware output.
- Unmodified homebrew samples: console Hello World, screen Hello World, and a basic controller sample with its exit callback.
- A project-owned sample that draws an RGB triangle through GE display lists and software rasterization.

## Build and test

Requirements:

- Clang with C++23, CMake 3.20+, fmt, SDL3 3.4.2 development files, and GoogleTest.
- A host with IEEE binary32 evaluation and floating-point environment support; CMake enables Clang's strict floating-point mode.
- `just`, plus clang-format and clang-tidy for `just ci`.
- Git submodules (`git submodule update --init --recursive`) and Git LFS for the homebrew fixtures (`git lfs pull`).

```sh
just build          # Debug build (-O1)
just build release  # Release build
just test           # Debug tests, including hardware-output comparisons
just test release   # the same tests after optimization
just ci             # format check, Debug and Release tests, clang-tidy, sanitizers
```

- `configure`, `build`, and `test` accept a preset and default to `debug`.
- Available presets: `debug`, `release`, `tidy`, and `sanitizers`, defined in [CMakePresets.json](/CMakePresets.json).
- Each preset uses its own directory under `build/<preset>`.
- `build` configures automatically; `test` and `run` build automatically.

Without `just`, select the same CMake presets explicitly:

```sh
cmake --preset release
cmake --build --preset release --parallel "$(nproc)"
ctest --preset release --parallel "$(nproc)"
```

Other recipes:

- `just format` / `just format-check`: clang-format over `src/`.
- `just tidy`: clean clang-tidy build using the `tidy` preset.
- `just sanitizers`: build and test with address and undefined behavior sanitizers.

## Run a PRX

```sh
just run release src/runtime/tests/fixtures/triangle.prx --window
just run debug src/runtime/tests/fixtures/controller_basic.prx --window --max-instructions 500000000
just run release third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx
just run release third_party/pspautotests/tests/gpu/filtering/precisionnearest2d.prx
just run release third_party/pspautotests/tests/gpu/filtering/precisionlinear2d.prx
just run release third_party/pspautotests/tests/gpu/commands/blend.prx --max-instructions 1000000000
just run debug program.prx --max-instructions 1000000
```

- `run` requires a preset followed by a PRX path; remaining arguments go to the emulator. Relative PRX paths are resolved from the repository root.
- Headless by default. Guest output goes to stdout, including output captured before a fault.
- Faults and budget exhaustion report the guest PC and instruction on stderr. A guest that only host input could wake also fails. Both exit nonzero.
- `--max-instructions` defaults to 50 million. GE commands have a separate limit of the same size.
- The unoptimized `blend.prx` test repeatedly fills and copies whole framebuffers; use the larger instruction budget shown above.
- The guest's `argv[0]` is the PRX file name without host directories.
- `--window` opens a resizable 960 × 544 window with sharp scaling and letterboxing, paced at the PSP's 60000/1001 Hz.
- The final frame stays visible after the guest exits. Escape or closing the window quits, also during execution.

Keyboard controls (physical key positions):

- Arrow keys: D-pad. WASD: left analog stick; opposite keys cancel.
- I/J/K/L: Triangle/Square/Cross/Circle. Q/E: left/right trigger.
- Enter: Start. Backspace: Select.
- Home: request exit through the guest's exit callback.
- Releasing keys or losing focus clears held input. The right stick stays neutral.

## Build homebrew PRXs

Optional; needs Ubuntu 24.04 x86_64, GNU Make, curl, tar, and sha256sum.

```sh
just setup-pspdev   # pinned PSPDEV v20261001 into ~/.local/opt/pspdev, checksum-verified
just homebrew       # homebrew fixtures into build-homebrew/
```

- Version, URL, and checksum are pinned in `justfile`; the sample-source submodule matches that release.
- Sources are copied into `build-homebrew/`, so the submodule stays clean.
- The normal build and `just ci` do not need the PSP toolchain.
- Fixture provenance and rebuild steps: [fixtures README](/src/runtime/tests/fixtures/README.md).

## Documentation

- [Runtime and syscall handling](/docs/runtime.md): components, services, scheduling, display, controller, and GE.
- [Allegrex CPU reference](/docs/cpu-reference.md): instruction sources and CPU behavior.
- [Guest memory](/docs/memory.md): address layout and access rules.
- Tests live next to their code under `src/*/tests/`.
