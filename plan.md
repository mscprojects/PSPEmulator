# SDL display implementation plan

Status: SDL display and controller input complete on 2026-10-09. Both milestones are implemented and verified.

## REQ

- Run the unmodified PSPSDK screen Hello World sample as a PRX and show pixels drawn by its guest code in an SDL3 window.
- Use the existing deterministic guest clock: one microsecond per instruction, with periodic vblank at 60000/1001 Hz. CPU cycle estimates and clock-frequency emulation are deferred until a concrete program requires better timing.
- Interleave emulation and SDL on one main thread. Return control at emulated vblank events, rather than using a fixed instruction batch as the normal display boundary.
- Preserve headless execution, captured console output, fault reporting, and the overall instruction budget.
- Start with the sample's 480 × 272 LCD mode and 32-bit RGBA 8888 framebuffer format. Present it in a 960 × 544 window with sharp scaling and preserved aspect ratio when resized.
- Keep the final frame visible after guest execution ends. Closing the window or pressing Escape ends the frontend, including during guest execution.
- Store the compiled screen Hello World PRX through Git LFS, with source/toolchain provenance and rebuild instructions.
- GE command processing, other pixel formats, guest interrupt handlers, controller input, callbacks, and accurate LCD scanout are outside this milestone. Unsupported services and formats must fail clearly.

## Approach

### Display state and guest services

Add an SDL-independent display component to the emulator core. It owns the selected framebuffer and pending framebuffer changes, including address, stride, and pixel format. VRAM and its uncached alias are already mapped. Capture the visible pixels using the guest stride, accounting for the SDK's 512-pixel row width; the displayed image is only 480 pixels wide. Validate guest parameters and address ranges before using them.

Implement the services needed by `third_party/pspsdk/src/samples/template/elf_template/main.c`, compiled as a PRX:

- `sceGeEdramGetAddr` (`sceGe_user`, NID `0xE47E40E4`).
- `sceDisplaySetMode` (`sceDisplay`, NID `0x0E20F177`).
- `sceDisplaySetFrameBuf` (`sceDisplay`, NID `0x289D82FE`).

Apply next-frame framebuffer selections at vblank. If immediate selection is supported, document its approximation of the SDK's next-hsync behavior. Displayed pixels should be opaque; framebuffer alpha is not transparency against the host window. Additional display APIs should be added only if this sample's execution requires them.

### Execution and vblank

Expose incremental execution that returns at the next vblank or guest termination. Keep `execute_prx()` as the headless convenience path that runs through successive events to completion. Guest faults retain their instruction context.

When all guest threads are delayed, advance guest time to the earlier of the next vblank and thread wakeup. Preserve delay results, interrupt masking, link-bit invalidation, and existing scheduling behavior. Display handoffs occur even when the guest has no active framebuffer, does not wait for vblank, or has masked interrupts; guest interrupt delivery is a separate operation.

At each vblank, activate any pending framebuffer selection and capture the displayed pixels before guest execution continues. Also make the final displayed contents available when a program exits between vblanks. Allow an earlier frontend handoff if needed for responsiveness; it must not alter guest time or generate an extra frame.

### SDL frontend and pacing

Add `--window` to `pspemu`, retaining the existing headless default and `--max-instructions` option. SDL belongs in the frontend; the core and its tests should not require SDL initialization or a desktop session. SDL3 3.4.2 development files were verified installed.

The frontend processes events, runs to the next guest display event, uploads captured pixels into an SDL texture, and presents them. Pace execution using a monotonic host clock with deadlines derived from a fixed starting point at 60000/1001 Hz. Continue processing window events while waiting. The monitor's refresh rate must not determine guest timing. If execution falls behind, preserve guest work and display events; intermediate host presentations may be omitted. After guest termination, retain the final image and continue handling window events.

## Milestones and verification

1. [x] Add display state and service dispatch. Test framebuffer validation, stride handling, color byte order, opacity, address aliases, and activation at vblank.
2. [x] Add execution that yields at vblank. Test running guests, idle periods spanning several vblanks, termination between display events, masked interrupts, and preservation of existing execution results and instruction limits.
3. [x] Build the unmodified SDK screen sample using the pinned PSPDEV setup, add its PRX through Git LFS, and extend the homebrew recipe and fixture documentation. Execute it at two load addresses and verify successful termination and rendered text against an independently established pixel expectation.
4. [x] Add the SDL frontend and CLI option. Verify the actual rendered window, final-frame retention, resizing, Escape/close handling, and closure during active execution. Use an isolated offscreen or virtual-display smoke check where practical. Run `just ci` for formatting, clang-tidy, Debug/Release tests, and address/undefined-behavior sanitizers. Verify the staged LFS pointer and object integrity before committing the completed feature.

## Resume context

- The baseline is commit `16aea40`, which added partition-memory freeing and the console Hello World fixture. Its `just ci` run passed all 157 tests in each tested build configuration.
- The display sample is `third_party/pspsdk/src/samples/template/elf_template/main.c`; it calls `pspDebugScreenInit()` and `pspDebugScreenPrintf("Hello World\n")` and returns.
- SDK display initialization is in `third_party/pspsdk/src/debug/scr_printf.c`; display API contracts and NIDs are in `third_party/pspsdk/src/display/pspdisplay.h` and `sceDisplay.S`.
- Relevant implementation files are `src/runtime/runtime.*`, `kernel.*`, `syscall_dispatcher.*`, `execution.*`, `src/main.cpp`, `CMakeLists.txt`, and `justfile`.
- `.gitattributes` already tracks `*.prx` with Git LFS. The existing fixture notices cover the same pinned SDK, Newlib, and GCC runtime revisions.
- No blocking scope decisions remain. Resolve routine interface choices during implementation without broadening this milestone.

## Completion and verification

- `just ci` passed formatting and clang-tidy, with all 171 tests passing in each Debug, Release, and address/undefined-behavior sanitizer configuration. Leak detection remains enabled; isolated SDL tests use offscreen video, software rendering, and CPU framebuffer surfaces.
- The unmodified screen sample executes at both load addresses and matches the independently established full-frame MSX-font expectation. Its compiled PRX, pinned provenance, license notices, and rebuild instructions are included.
- Actual CLI windows were checked on the desktop for rendered text, resizing, final-frame retention, window close, Escape, console output, and exit status. Offscreen tests also verify closure during active execution. CLI checks cover both option orders, invalid arguments, headless output, and windowed fault context.
- The staged LFS pointer and stored object match SHA-256 `9458a58f4fec26f497670d901b5289e08a1cd962f3e73ad3daabda9bcdef7f0c` and size 158598 bytes; Git LFS integrity checks passed.

## Controller input milestone

Run the unmodified SDK `controller/basic` sample with keyboard buttons and left analog input, deterministic vblank sampling, blocking positive reads, callback creation and exit registration, and callback-enabled sleep. Home requests a guest exit; Escape and window close stop the frontend. Preserve the sample's guest drawing and execute its callback through the CPU interpreter.

- [x] Add SDL-independent input, cycle-0 sampling, digital/analog modes, and one-sample positive reads. Pending reads yield and resume at a guest sampling event.
- [x] Add callback ownership, notification delivery, guest callback entry/return, and restoration of the sleeping owner. Select threads by PSP priority at instruction boundaries so rendering cannot starve the higher-priority callback thread; keep FIFO ties and defer time slicing.
- [x] Connect keyboard presses/releases, focus loss, and Home to incremental execution. Store the unmodified PRX in Git LFS and test neutral/changed analog values, all buttons, and guest callback exit at two load addresses.
- [x] Verify the actual desktop window, run formatting/clang-tidy/Debug/Release/sanitizers, inspect LFS integrity, and commit the milestone.

Current limits: sampling cycle 0, count-1 positive reads, and one latest unread sample. Other controller APIs and general callback notification/wakeup APIs remain unsupported. The keyboard frontend leaves the right stick neutral; the core API and fixture tests cover both sticks. Runtime and keyboard contracts are documented in `docs/runtime.md` and `README.md`.

Controller verification: `just ci` passed formatting and clang-tidy, with all 185 tests passing in Debug, Release, and address/undefined-behavior sanitizer builds. A desktop SDL window was checked for changed analog coordinates, button text, release to neutral, Home callback exit, final-frame retention after further input, and Escape closure. The PRX rebuilt identically from the unmodified pinned source. Its staged LFS pointer and stored object match SHA-256 `c4f5c88809321de3b545d48fc7598b0e5c7fe115cc7c609372b98137d35ae165` and size 160230 bytes; LFS integrity checks passed.
