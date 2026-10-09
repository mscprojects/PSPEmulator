# PSP emulator implementation plan

Status: SDL display and controller input are complete and verified. The GE triangle milestone is in progress and checkpointed at the user's request on 2026-10-09. Its initial implementation builds, but the triangle milestone is not complete. Resume with the GE verification work below.

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

## GE triangle milestone: checkpoint and remaining work

Render a small SDK-linked RGB triangle through guest GU calls, GE display lists, and software rasterization into VRAM, then present it through the existing display and SDL frontend. Start with `GU_TRANSFORM_2D`: the SDK `gu/ortho` example also requires matrix transforms and rotation, which belong to a later milestone.

### Initial implementation in this checkpoint

- `src/runtime/ge.*` adds incremental command processing, queued lists, stall updates, list/draw synchronization, basic address state and JUMP, FINISH/END, finish callback registration, and a separate GE command limit using the configured instruction-budget value.
- The draft rasterizer writes unindexed, untextured screen-space triangles and GU color-clear sprites into RGBA 8888 VRAM. It handles stride, region/scissor clipping, smooth/flat shading, winding, and VRAM address mirroring. Unsupported active features are rejected before drawing.
- Runtime advances one GE command alongside CPU work and advances the guest clock during runnable GE work when CPU threads are waiting. Kernel support includes GE waits/wakeups, vblank waits, guest finish callback entry/return, and the event-flag creation/deletion needed by GU initialization and termination.
- `homebrew/triangle/main.c` and `Makefile` build a project-owned sample against the pinned SDK. It clears the framebuffer, draws a red/green/blue triangle, submits finish ID 7, waits for synchronization and the actual guest finish callback, swaps buffers, and waits for Home to invoke its exit callback. `just homebrew` now builds it as `build-homebrew/triangle/triangle.prx` alongside the existing SDK examples.
- The checkpoint has no dedicated GE tests and no committed triangle PRX fixture yet. The only existing test change wires the new GE dependency into the syscall-dispatcher setup.

Checkpoint verification: `just homebrew`, `just build`, and `just format-check` passed. All 185 existing Debug tests passed. A headless run with a 2,000,000-instruction limit reached that limit without an earlier reported GE fault; the sample is interactive and waits for Home, so this does not establish successful completion, callback delivery, or correct pixels. Release, clang-tidy, sanitizers, and a desktop triangle check have not been run for this draft.

### Remaining implementation and verification

1. [ ] Add focused GE tests, then fix issues they expose before treating the implementation as complete.
   - Test a list stalled at its start, queued/running/stalled/done status, FIFO ordering, stall resumption, and reuse of completed list slots. Blocking list sync must wait for its own END; draw sync must wait for all queued lists. Validate IDs, callback slots, guest pointers, and unsupported submission arguments.
   - Verify FINISH executes real guest instructions with the finish ID and common argument. Test masked and nested interrupt deferral, restoration of the interrupted thread's full CPU state and stack, ignored callback return values, and preservation of the existing sleeping exit-callback behavior.
   - Check VRAM offsets and aliases, 512-pixel stride, color byte order, region/scissor clipping, smooth and flat colors, reversed winding, degenerate triangles, and shared-edge coverage. Verify RGB clears preserve alpha and alpha-enabled clears replace it. Restrict clear sprites to the SDK's integer format if fractional clear coverage cannot be justified yet.
   - Check unsupported vertex formats, indices, 3D transforms, enabled rendering features, framebuffer formats, and primitive counts. Invalid coordinates and unmapped vertex/framebuffer ranges must fail before any VRAM changes.
   - Check malformed lists and unknown commands report the GE program counter and command word. Bound cyclic JUMP lists, including execution with every CPU thread waiting. Verify independent CPU/GE budget enforcement and vblank handoffs during GE waits.
   - Cover vblank wait timing and return values, plus the limited event-flag creation/deletion used by GU.
2. [ ] Add execution tests for the SDK-linked triangle at load addresses `0x08800000` and `0x08900000`.
   - Advance to display events and compare the entire visible framebuffer with an independently derived pixel expectation. Check the finish callback actually runs; the fixture's `finishes` gate provides a guest-visible condition.
   - Request Home both before and after rendering, verify guest exit and the final image, and preserve instruction-limit fault behavior.
   - Store the verified triangle PRX through Git LFS, with source location, pinned toolchain provenance, license notices, checksum, and reproducible rebuild instructions matching the existing fixtures.
3. [ ] Check the actual SDL triangle window, Home callback exit, final-frame retention, Escape, and window close. Add an offscreen frontend test where it verifies behavior beyond the existing controller/window coverage.
4. [ ] Document the GE implementation in `docs/runtime.md` and the triangle build/run command in `README.md`. Describe supported commands and formats, asynchronous lists and synchronization, guest callbacks, timing/budgets, unsupported features, and the provisional rasterization rules. Update the `justfile` homebrew comment to include the project-owned sample. Mark this milestone complete in this plan only after verification.
5. [ ] Run `just ci` and fix formatting, clang-tidy, Debug/Release, and address/undefined-behavior sanitizer failures with leak detection enabled. Verify SDK submodules remain unchanged, the PRX rebuild is identical, and the staged LFS pointer and stored object agree. Commit the completed milestone separately from this checkpoint.

### Scope and known limitations

- Start with one unindexed, untextured three-vertex screen-space triangle per primitive and SDK color-clear sprites. Textures, depth/stencil operations, blending, general 3D transforms, VFPU, SIGNAL/CALL/RET, list cancellation/head insertion, and saved GE contexts remain outside this milestone. Event flags currently provide only creation/deletion, not general waiting/signaling.
- Raster coverage currently uses fixed subpixel coordinates, pixel-center edge tests, a top-left rule, and integer barycentric color interpolation. These rules are provisional, not established as bit-exact PSP behavior; hardware probes are needed before claiming that accuracy.
- Finish interrupts are delivered through an available running guest thread. Callback scheduling while all threads are waiting, masking/nesting, and restoration need targeted verification. Completion of the submitting list wakes sync waiters; guest termination currently does not drain unfinished lists automatically.
- No new user scope decision is pending. The next implementation step is focused GE/kernel tests and corrections, followed by triangle execution and pixel tests. The user requested this checkpoint before that work continues.

### Resume references and commands

- Completed display and controller commits are `f08e0d1` and `057caf8`; the GE checkpoint follows the latter on the current branch.
- SDK contracts are in `third_party/pspsdk/src/ge/pspge.h`, `sceGe_user.S`, and `src/gu/guInternal.h`. Read `sceGuInit`, `sceGuStart`, `sceGuFinishId`, `sceGuDrawArray`, `sceGuGetMemory`, and `sceGuClear` for emitted command streams, stall handling, callbacks, and vertex layouts.
- The pinned PSPDEV installation is `~/.local/opt/pspdev/v20261001`, with SDK revision `6f15c154c902963864ea5c4538cc94febf8a2dad`. Preserve the existing toolchain pin and unmodified submodule sources.
- Primary implementation references consulted for framebuffer mirroring and raster behavior were [PPSSPP GPU state](https://github.com/hrydgard/ppsspp/blob/master/GPU/GPUState.h) and [software rasterizer](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/Rasterizer.cpp). Their hardware details need separate probes before adopting exact raster expectations.
- Rebuild with `just homebrew` and `just build`; inspect with `./build/pspemu build-homebrew/triangle/triangle.prx --window`. Home requests guest exit; Escape closes the frontend. Run the complete required checks with `just ci` when the milestone is ready.
- Temporary checkpoint logs are under `~/Temp/psp-triangle/`; they are local scratch files, not committed artifacts.
