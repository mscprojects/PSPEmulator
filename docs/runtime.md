# Runtime and syscall handling

## Execution API

- `Execution` runs one independent guest program; `execute_prx()` runs it headless to completion.
- `advance()` returns at the next vblank or at termination. After termination it keeps returning `Finished` without advancing time.
- `pixels()` returns the packed `kLcdWidth` × `kLcdHeight` RGBA frame at the last event.
- `output()` returns captured console bytes at any time; `result()` is available after `Finished`.
- `run_to_completion()` throws when the guest can only be woken by host input (every live thread waits for a callback notification or GE completion with nothing pending). Otherwise it would loop over idle vblanks forever.
- Guest faults, unsupported imports, and budget exhaustion throw with the guest PC and instruction word.
- The startup thread gets a 64 KiB stack, GP, priority `0x20`, and the NUL-separated `ExecutionOptions::arguments` in `a0`/`a1`.
- The guest returns its exit code from the last thread's `v0`, or zero after `sceKernelExitGame`.

## Components

Each component has one header and one source file. None receives a `Runtime&`.

- `Runtime`: owns the components, steps the selected thread, enforces the instruction budget, and is the only code that advances guest time.
- `Cpu`: executes one Allegrex instruction on a supplied `CpuState`; see [CPU reference](/docs/cpu-reference.md).
- `Memory`: RAM, VRAM, and address aliases; see [guest memory](/docs/memory.md).
- `Kernel`: threads, scheduling, guest clock, vblank timing, interrupts, callbacks, sync objects, and partition block IDs.
- `AddressArena`: free guest address ranges shared by thread stacks and partition blocks.
- `SyscallDispatcher`: binds imports and maps guest registers to component calls.
- `Display`: active and pending framebuffers and the captured frame.
- `Controller`: host input, sampling mode, and pending reads.
- `Ge`: display lists, GE callbacks, and drawing-state validation.
- `rasterizer.hpp`: draws triangles and clears into a validated framebuffer; never reads GE registers.
- `GuestIo`: captured output and the autotest emulator device.
- `guest_structures.hpp`: little-endian guest layouts with compile-time size and offset checks.
- `lcd.hpp`: panel size and 60000/1001 Hz refresh, shared by the runtime and the SDL frontend.

## Syscalls

- Import stubs are patched to `JR $ra` followed by `SYSCALL code`. The code is a local index, not the NID.
- `Cpu::step()` commits control flow, then returns the syscall event; it never calls services.
- Arguments come from `a0`–`a7` (registers 4–11). Results go to `v0`; 64-bit time results also use `v1`.
- Blocking services leave `v0` untouched; the wakeup supplies the result.
- Calling an unsupported import throws with its library name and NID; an unbound syscall code also throws.

## Guest time and scheduling

- The clock counts guest microseconds from zero and never reads host time.
- One CPU instruction or one GPU-only GE command advances it by one microsecond. This rate is provisional, not PSP timing.
- When no thread is ready, time jumps to the earlier of the next wakeup and the next vblank. Idle time does not consume the instruction budget.
- Ready threads run by lowest PSP priority, FIFO among equals. A higher-priority ready thread preempts at the next instruction boundary.
- Thread switches happen only between instructions, never inside `Cpu::step()` or dispatch.
- Equal deadlines wake in delay order.
- When a delay ends on a vblank edge, the vblank handoff happens first; the thread wakes before the next guest instruction.
- Execution ends when no running, ready, or waiting thread remains, or when the guest calls `sceKernelExitGame`.
- Unsupported: equal-priority time slicing, timeouts, blocking mutex and semaphore waits, and short-delay hardware granularity ([`delayzero`](/third_party/pspautotests/tests/threads/scheduling/delayzero.c)).

Services (`ThreadManForUser`):

- `sceKernelGetSystemTimeLow` (`0x369ED59D`): low 32 bits of guest time.
- `sceKernelGetSystemTimeWide` (`0x82BC5777`): full 64-bit time in `v0`/`v1`.
- `sceKernelDelayThread` (`0xCEADEB47`): waits `a0` microseconds and returns zero. A zero delay yields to ready threads of equal priority.
- `sceKernelReferThreadStatus` (`0x17C1684E`): writes a 104-byte `SceKernelThreadInfo` for thread `a0` (zero selects the current thread). Wait types: sleep 1, delay 2, controller and GE 4 (event-flag approximation), vblank 12. Scheduling counters stay zero.

## Threads, synchronization, and I/O

Threads (`ThreadManForUser`):

- `sceKernelCreateThread` (`0x446D8DE6`): name, entry, priority, stack size (at least 512 bytes), and attributes; the stack comes from the allocation arena.
- `sceKernelStartThread` (`0xF475845D`): copies `a1` bytes from `a2` to the top of the new stack and makes the thread ready.
- `sceKernelExitThread` (`0xAA73C935`), `sceKernelGetThreadId` (`0x293B45B8`), `sceKernelGetThreadCurrentPriority` (`0x94AA61EE`).
- Unsupported: thread deletion, priority changes, and stack reclamation.

Synchronization (needed by Newlib startup in `cpu_div`):

- `sceKernelCreateLwMutex` (`0x19CFF145`) and `sceKernelDeleteLwMutex` (`0x60107536`): the guest work area stays authoritative for owner and count.
- `sceKernelLockLwMutex` (`0xBEA46419`) and `sceKernelUnlockLwMutex` (`0x15B6446B`), in `Kernel_Library`: uncontended and recursive locks only.
- `sceKernelCreateSema` (`0xD6DA4BA1`), `sceKernelDeleteSema` (`0x28B6489C`), `sceKernelWaitSema` (`0x4E3A1105`), and `sceKernelSignalSema` (`0x3F53E640`): waits must be satisfiable immediately and have no timeout.
- Contended locks, unsatisfiable waits, and timeouts throw instead of blocking.

I/O and system parameters:

- `sceIoWrite` (`IoFileMgrForUser`, `0x42EC03AC`): captures writes to stdout and stderr; other descriptors throw.
- `sceIoDopen` (`0xB29DDF9C`): no guest filesystem; returns `ENOENT` (`0x80010002`).
- `sceIoDevctl` (`0x54F5FB11`): the autotest `emulator:` device. Command 1 reports no display, 2 captures output, and 3 is a probe.
- `StdioForUser` `0x172D316E`, `0xA6BAB2E9`, and `0xF78BA90A` return descriptors 0, 1, and 2.
- `sceUtilityGetSystemParamInt` (`sceUtility`, `0xA5DA2406`): IDs 6 and 7 return 0 (UTC, no daylight saving); other IDs throw.
- The runtime never touches host files; guest libc does all formatting.

## Interrupts

- Vblank is the only interrupt source (interrupt 30). Edge n occurs at n × 1001000/60 µs and is seen at the first whole microsecond at or after it.
- Edges while interrupts are masked coalesce into one pending interrupt, delivered at the next instruction boundary once enabled and a thread is running.
- Delivery is handled on the host: it clears the link bit (as the [`llsc` test](/third_party/pspautotests/tests/cpu/lsu/llsc.c) observes) and preserves all other CPU state, including a pending delay slot.
- Unsupported: guest interrupt handlers, CP0 exception vectors and `ERET`, handler timing, and interrupt-driven preemption.

Services (`Kernel_Library`):

- `sceKernelCpuSuspendIntr` (`0x092968F4`): disables delivery and returns the previous enable flag.
- `sceKernelCpuResumeIntr` (`0x5F10D406`) and `sceKernelCpuResumeIntrWithSync` (`0x3B84732D`): restore bit 0 of `a0`; delivery waits for the next instruction boundary.
- `sceKernelIsCpuIntrEnable` (`0xB55249D2`): current enable flag.
- `sceKernelIsCpuIntrSuspended` (`0x47A0B729`): one when the whole `a0` word is zero, following [`suspended.expected`](/third_party/pspautotests/tests/intr/suspended.expected).

## Partition memory

- The arena spans from the end of the loaded image to the end of RAM, with 256-byte alignment. VRAM is outside it.
- The return sentinel, thread stacks, and partition blocks share the arena.
- `sceKernelAllocPartitionMemory` (`0x237DBD4F`): user partition 2, low (0) or high (1) placement.
- `sceKernelGetBlockHeadAddr` (`0x9D9A5BA1`): block start address.
- `sceKernelFreePartitionMemory` (`0xB6D61D02`): returns the range and merges neighbors; memory contents stay unchanged.
- `sceKernelTotalFreeMemSize` (`0xF919F628`) and `sceKernelMaxFreeMemSize` (`0xA291F107`): total and largest free range.
- Allocation failures and invalid IDs throw.
- Unsupported: fixed-address allocation, other partitions, and stack reclamation.

## Display

- `sceGeEdramGetAddr` (`0xE47E40E4`): returns `0x04000000`.
- `sceDisplaySetMode` (`0x0E20F177`): accepts only LCD mode 0 at 480 × 272.
- `sceDisplaySetFrameBuf` (`0x289D82FE`): RGBA 8888 only; 16-byte aligned address, power-of-two stride of at least 480, and a full stride × 272 × 4 range in one mapping. Address zero disables output.
  - Sync 1 applies at the next vblank; the latest pending selection wins.
  - Sync 0 applies immediately, approximating next-hsync until scanout is modeled.
- `sceDisplayWaitVblankStart` (`0x984C27E7`): waits for the next vblank edge, even when called on one, and returns zero.
- Frames are captured lazily on the first `pixels()` call after an event. Alpha is always 255; a disabled display is black.
- Termination between vblanks shows the active framebuffer and ignores a pending selection.
- Unsupported: other display services and pixel formats, and scanout timing.

## Controller and exit callbacks

- `sceCtrlSetSamplingCycle` (`0x6A2774F3`): only cycle 0 (vblank sampling).
- `sceCtrlSetSamplingMode` (`0x1F4011E6`): digital 0 (neutral sticks) or analog 1.
- `sceCtrlReadBufferPositive` (`0x1F803938`): count 1 into an aligned 16-byte buffer. Returns a fresh sample, or waits for the next vblank sample. Readers are served FIFO, one sample each.
- Samples are taken at every vblank, regardless of interrupt masking, with the low 32 bits of guest time as timestamp.
- `sceKernelCreateCallback` (`0xE81CAF8F`), `sceKernelRegisterExitCallback` (`0x4AC57943`), and `sceKernelSleepThreadCB` (`0x82826F70`) support the standard exit-callback pattern.
- `Execution::request_exit()` notifies the exit callback; an early request waits for registration. It never stops execution by itself.
- The callback runs as guest code on its sleeping owner's stack with the notification count, zero, and its common pointer. A nonzero return deletes it, as in [PPSSPP](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelThread.cpp).
- Unsupported: historical buffers, peeks, negative reads, latches, other sampling periods, nested callback sleep, and other callback APIs.

## GE

- `sceGeSetCallback` (`0xA4FC06A4`) and `sceGeUnsetCallback` (`0x05DB22CE`): up to 16 callbacks. SIGNAL is unsupported.
- `sceGeListEnQueue` (`0xAB49E76A`): 64 list slots executed FIFO. Stall zero means no stall; callback -1 means none. Non-null list arguments are unsupported.
- `sceGeListUpdateStallAddr` (`0xE0D68148`): a list stops before the command at its stall address and blocks later lists.
- `sceGeListSync` (`0x03444EB4`) and `sceGeDrawSync` (`0xB287BD61`): mode 1 polls (done 0, queued 1, running 2, stalled 3); mode 0 waits for END.
- One command runs alongside each CPU instruction. The GE command budget equals `max_instructions` but is counted separately.
- Commands: NOP, VADDR, IADDR, BASE, OFFSET_ADDR, JUMP, PRIM, FINISH, END, zero-block CLUT_LOAD, and the state registers a standard GE reset writes. Other commands fail.
- FINISH runs the guest finish callback when interrupts are enabled and no callback is active. It disables interrupts and preemption until it returns, then restores the interrupted thread and its wait.
- Drawing: one untextured 2D triangle or one integer clear sprite per PRIM, RGBA 8888, clipped to region and scissor. Unsupported state fails before any VRAM write.
- Rasterization rules are provisional, pending hardware probes: pixel-center coverage, top-left edges, integer barycentric colors, and flat color from the last vertex.
- `sceKernelCreateEventFlag` (`0x55C20A00`) and `sceKernelDeleteEventFlag` (`0xEF9E4C70`): create and delete only, as graphics initialization needs.
- Unsupported: textures, depth and stencil, blending, 3D transforms, VFPU, SIGNAL/CALL/RET, list cancellation, and saved GE contexts.

## Sources

- Bundled [pspautotests](/third_party/pspautotests/tests) expectations for observed hardware behavior.
- [PPSSPP](https://github.com/hrydgard/ppsspp) as an implementation reference where hardware results are missing.

## Tests

- Unit tests live next to each component under `src/*/tests/`.
- Execution tests compare bundled hardware-test output and homebrew fixture frames byte for byte at two load addresses; see the [fixtures](/src/runtime/tests/fixtures/README.md).
