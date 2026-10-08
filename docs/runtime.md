# Runtime and syscall handling

`execute_prx()` owns an independent execution through `detail::Runtime`. `Runtime` initializes the program, steps the selected CPU state, enforces the instruction budget, adds guest instruction context to faults, and returns captured output and termination status. Other components own the state and behavior of their services; none receives a `Runtime&`.

## Ownership

Each component has one header and one implementation file. Related kernel services stay together in `kernel.hpp` and `kernel.cpp`; file size alone is not a reason to split them.

- `Memory` owns RAM and VRAM backing bytes and shared address aliases, initialized by `prepare_prx()`. See [guest memory](memory.md) for the execution layout and access rules.
- `Cpu` executes one Allegrex instruction against guest memory and the supplied `CpuState`.
- `Kernel` owns saved CPU states, thread creation metadata, the ready queue, guest time, delay deadlines, interrupt masking and pending events, execution termination, mutex identities, semaphore counts, partition block ranges, and the shared object-ID counter. Each thread has an explicit created, started, waiting, or finished lifecycle; started threads are reported as running or ready according to the selected thread ID. Thread stacks and partition blocks share address-ordered free ranges with low and high allocation directions. Allocation state and block IDs have the same owner. Guest mutex work areas remain authoritative for ownership and recursive counts; mutex operations use the kernel's current thread ID.
- `GuestIo` owns captured output and the autotest emulator device protocol. It exposes no host filesystem or display.
- `SyscallDispatcher` owns import bindings and translates guest registers into named operations on those components. Small standard-stream and UTC timezone handlers remain here.

## Syscall flow

The dispatcher patches each function import stub with `JR $ra` followed by `SYSCALL code`. It assigns the encoded code locally and maps it to the import's library and NID. The code is neither the NID nor a syscall result.

`Cpu::step()` returns `std::optional<Syscall>`. Ordinary instructions return no event. A syscall event contains the encoded code and the syscall instruction address. The CPU commits instruction control flow before returning the event, including a pending delay-slot return target. It does not call PSP service implementations.

The runtime passes the event and current CPU state to the dispatcher. Integer arguments occupy registers 4–11 (`a0`–`a7` in the PSP MIPS32 EABI). The dispatcher supplies the service result in register 2 (`v0`); a 64-bit system-time result also uses register 3 (`v1`). It completes service handling before the runtime steps the next instruction.

## Guest structures

[`guest_structures.hpp`](../src/runtime/guest_structures.hpp) defines named guest layouts based on the [PSPSDK thread header](https://github.com/pspdev/pspsdk/blob/master/src/user/pspthreadman.h). Each numeric field is a four-byte little-endian `GuestWord`, including guest pointers. The layouts contain no native pointers or host-sized integers. Compile-time size and offset checks guard the ABI. Complete structures are converted to byte arrays with `std::bit_cast` and written through `Memory::write_bytes()`, which validates the whole range before modifying guest memory. No preliminary read-and-discard is needed for output validation. Structure reads use `Memory::read_into()` to copy directly into caller-owned storage after checking the full guest range, without allocating an intermediate byte vector.

`sceKernelReferThreadStatus` is imported from `ThreadManForUser` with NID `0x17C1684E`. Register `a0` supplies the thread ID; zero selects the current thread in this implementation. Register `a1` points to a `SceKernelThreadInfo` buffer whose initial `size` must be 104. `Kernel::thread_status()` builds the named fields and the dispatcher writes the complete structure, returning zero in `v0`. Invalid IDs or buffers and unsupported structure sizes produce host exceptions. Scheduling counters remain zero; delivering an interrupt without switching threads does not increment a preemption counter. A delayed thread reports waiting status 4 and delay wait type 2 with no wait object ID; its wait fields return to zero when the delay expires. These are the current runtime's limits, rather than a complete hardware contract; the bundled [`refer.c`](../third_party/pspautotests/tests/threads/threads/refer.c) probes additional sizes and behaviors.

## Partition memory

The allocation arena begins after the loaded image and ends at the configured RAM limit. Its bounds and allocation sizes use 256-byte alignment. A return sentinel and thread stacks reserve space in the same arena as partition blocks. VRAM remains separate.

`sceKernelAllocPartitionMemory` (`SysMemUserForUser`, NID `0x237DBD4F`) supports user partition 2 and low/high allocation types 0/1. Low allocations use the lowest suitable free range; high allocations use the highest suitable range. Each block ID records the complete aligned allocation. `sceKernelGetBlockHeadAddr` (`0x9D9A5BA1`) returns its start address.

`sceKernelFreePartitionMemory` (`0xB6D61D02`) returns a block's range to the arena, merges adjacent free ranges, removes its ID, and returns zero. It leaves the underlying RAM mapped and its bytes unchanged. `sceKernelTotalFreeMemSize` (`0xF919F628`) returns the sum of free ranges; `sceKernelMaxFreeMemSize` (`0xA291F107`) returns the largest contiguous range. Allocation failures and invalid or already-freed IDs still produce host exceptions, consistent with the runtime's existing service limits. Fixed-address allocations, additional partitions, and thread deletion/stack reclamation remain unsupported. Definitions come from the pinned [PSPSDK system-memory header](../third_party/pspsdk/src/user/pspsysmem.h).

## Guest time and scheduling

`Kernel` owns a 64-bit clock measured in guest microseconds, starting at zero. After each completed CPU instruction, `Runtime` advances it by one microsecond before dispatching a syscall. This is a provisional deterministic rate, not PSP instruction timing; host wall-clock time is never used. Idle time advances to the earliest pending delay deadline without executing instructions or consuming the instruction budget.

The [PSPSDK thread header](https://github.com/pspdev/pspsdk/blob/master/src/user/pspthreadman.h) defines the signatures and microsecond units; its [import table](https://github.com/pspdev/pspsdk/blob/master/src/user/ThreadManForUser.S) supplies the NIDs. These services are imported from `ThreadManForUser`:

- `sceKernelGetSystemTimeLow` (`0x369ED59D`) takes no arguments and returns the low 32 bits of guest time in `v0`, wrapping at 2³² microseconds.
- `sceKernelGetSystemTimeWide` (`0x82BC5777`) takes no arguments and returns the same clock in `v0` (low word) and `v1` (high word), including elapsed idle time.
- `sceKernelDelayThread` (`0xCEADEB47`) takes an unsigned microsecond delay in `a0`. The kernel records the current time plus that delay and marks the caller waiting. Dispatch leaves `v0` untouched; wakeup supplies zero for success before the caller resumes at its already-committed syscall return address. A zero delay yields to already-ready threads without advancing time. This simplified zero-delay behavior and exact deadlines do not reproduce the hardware's short-delay granularity, probed by the bundled [`delayzero` test](../third_party/pspautotests/tests/threads/scheduling/delayzero.c).

`Kernel::select_next_thread()` runs between CPU steps. Expired delays enter the ready queue in deadline order, with equal deadlines preserving insertion order. The current thread continues until it delays, exits, or reaches the runtime's return sentinel after its return delay slot. The next ready thread is selected in FIFO order. If no thread is ready and a delay is pending, the clock advances to its deadline and wakes it. Execution ends only when no running, ready, or delayed thread remains, or a game-exit service ends it explicitly. Starting a thread and waking a delay do not preempt the current thread.

Synchronization waits, callbacks, timeout cancellation, priority scheduling, and timer preemption remain unsupported. Thread switching happens after `Cpu::step()` and syscall dispatch have unwound, so references to the previous thread state remain valid throughout the instruction. A delay wakeup does not clear the load-link bit by itself; pending interrupt delivery is a separate operation.

## Periodic interrupts

The initial interrupt source is LCD vblank, interrupt 30 in the [PSPSDK interrupt header](https://github.com/pspdev/pspsdk/blob/master/src/user/pspintrman.h). The [display header](https://github.com/pspdev/pspsdk/blob/master/src/display/pspdisplay.h) specifies approximately 59.94005995 Hz, also recorded by the bundled [`display.expected`](../third_party/pspautotests/tests/display/display.expected). The runtime uses 60000/1001 Hz, with phase zero at guest clock startup. The nth edge occurs at n × 1001000/60 microseconds and becomes visible at the first integer microsecond at or after that edge. Counting crossed edges from the 64-bit clock avoids rounding drift and handles large idle jumps without replaying every frame.

`Kernel::advance_time()` marks a vblank interrupt pending when time crosses an edge. Multiple unhandled edges coalesce into one pending event. After thread selection and before the next CPU instruction, `Runtime` calls `Kernel::deliver_pending_interrupt()`. Masked interrupts remain pending. A waiting thread does not receive an interrupt; delivery waits until a thread is running. Idle clock advancement uses the same event-generation path as CPU execution.

The HLE kernel handles interrupt entry and return immediately on the host. It clears the running thread's Allegrex link bit, as observed by the bundled [`llsc` hardware test](../third_party/pspautotests/tests/cpu/lsu/llsc.c), and preserves all registers, HI/LO, and both instruction addresses. A pending branch target and its unexecuted delay slot survive delivery. This models the observable effect of the kernel interrupt path for current programs. Guest subinterrupt handlers, CP0 exception vectors and `ERET`, interrupt-handler execution time, other interrupt sources, display scanout, and interrupt-driven thread preemption remain unimplemented.

Interrupt-control services belong to `Kernel_Library`; their NIDs come from the [PSPSDK import table](https://github.com/pspdev/pspsdk/blob/master/src/user/Kernel_Library.S):

- `sceKernelCpuSuspendIntr` (`0x092968F4`) takes no arguments. It disables delivery and returns the previous enable flag, zero or one, in `v0`. A nested call returns zero.
- `sceKernelCpuResumeIntr` (`0x5F10D406`) and `sceKernelCpuResumeIntrWithSync` (`0x3B84732D`) take saved flags in `a0`, restoring the enable bit from bit zero. They have void PSP signatures; this runtime writes zero to `v0`. Resuming does not deliver an interrupt inside syscall dispatch; delivery occurs at the next instruction boundary.
- `sceKernelIsCpuIntrEnable` (`0xB55249D2`) takes no arguments and returns the current enable state in `v0`.
- `sceKernelIsCpuIntrSuspended` (`0x47A0B729`) takes flags in `a0` and returns one when the entire flags word is zero, otherwise zero. The bundled [`suspended.expected`](../third_party/pspautotests/tests/intr/suspended.expected) supplies this behavior, including nonzero values other than one; the PSPSDK comment describes the return value differently.

## Verification

The [execution tests](../src/runtime/tests/execution_test.cpp) compare every output byte of `cpu_alu`, `cpu_branch2`, `cpu_div`, `lsu`, `llsc`, `fpu_branch`, `fpu_branch_hazard`, `fpu`, `roundmode`, `rounding`, `fpu_nan`, and `fcr` at two load addresses. The LSU comparison adds the final blank line emitted by the guest source but omitted from its bundled expectation; the other outputs match their bundled expectations directly. The unmodified SDK Hello World [fixture](../src/runtime/tests/fixtures/README.md) must print exactly `Hello World\n` and exit successfully at two load addresses; it includes Newlib startup and heap cleanup. Guest-instruction tests also verify partition-free return values, distinct total/largest-free queries under fragmentation, register argument handling, startup arguments, faults, instruction budgets, synchronization services, time queries, and a delay that runs another guest thread before resuming the caller. [Kernel tests](../src/runtime/tests/kernel_test.cpp) cover block reuse, low/high placement, merging in every free order, invalid frees, aligned sizes and address boundaries, shared stack/partition ownership, delay deadlines, deferred results, idle time, clock overflow, periodic interrupt deadlines, masking, link-bit invalidation, branch delay-slot preservation, and scheduling boundaries; [structure tests](../src/runtime/tests/guest_structures_test.cpp) check PSP field offsets, byte order, preservation of reserved fields, and rejection of incomplete output buffers without partial writes.
