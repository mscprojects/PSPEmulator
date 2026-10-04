# Runtime and syscall handling

`execute_prx()` owns an independent execution through `detail::Runtime`. `Runtime` initializes the program, steps the selected CPU state, enforces the instruction budget, adds guest instruction context to faults, and returns captured output and termination status. Other components own the state and behavior of their services; none receives a `Runtime&`.

## Ownership

- `Cpu` executes one Allegrex instruction against guest memory and the supplied `CpuState`.
- `GuestAllocator` owns the monotonic arena shared by partition allocations and thread stacks, plus partition block addresses.
- `Kernel` owns the shared object-ID counter, `Threads`, and `Synchronization`. It assigns partition IDs before delegating allocation to `GuestAllocator`. The kernel cannot be copied or moved because its components refer to its ID counter.
- `Threads` owns saved CPU states, metadata, the ready queue, and execution termination. It creates stacks through `GuestAllocator` and copies startup arguments into guest memory.
- `Synchronization` owns mutex identities and semaphore counts. Guest mutex work areas remain authoritative for ownership and recursive counts. The caller supplies the current thread ID explicitly.
- `GuestIo` owns captured output and the autotest emulator device protocol. It exposes no host filesystem or display.
- `SyscallDispatcher` owns import bindings and translates guest registers into named operations on those components. Small standard-stream and UTC timezone handlers remain here.

## Syscall flow

The dispatcher patches each function import stub with `JR $ra` followed by `SYSCALL code`. It assigns the encoded code locally and maps it to the import's library and NID. The code is neither the NID nor a syscall result.

`Cpu::step()` returns `std::optional<Syscall>`. Ordinary instructions return no event. A syscall event contains the encoded code and the syscall instruction address. The CPU commits instruction control flow before returning the event, including a pending delay-slot return target. It does not call PSP service implementations.

The runtime passes the event and current CPU state to the dispatcher. Integer arguments occupy registers 4–11 (`a0`–`a3`, `t0`–`t3`). The dispatcher supplies the service result in register 2 (`v0`); a 64-bit system-time result also uses register 3 (`v1`). It completes service handling before the runtime steps the next instruction.

## Guest structures

[`guest_structures.hpp`](../src/runtime/guest_structures.hpp) defines named guest layouts based on the [PSPSDK thread header](https://github.com/pspdev/pspsdk/blob/master/src/user/pspthreadman.h). Each numeric field is a four-byte little-endian `GuestWord`, including guest pointers. The layouts contain no native pointers or host-sized integers. Compile-time size and offset checks guard the ABI. Complete structures are converted to byte arrays with `std::bit_cast` and written through `Memory::write_bytes()`, which validates the whole range before modifying guest memory. No preliminary read-and-discard is needed for output validation.

`sceKernelReferThreadStatus` is imported from `ThreadManForUser` with NID `0x17C1684E`. Register `a0` supplies the thread ID; zero selects the current thread in this implementation. Register `a1` points to a `SceKernelThreadInfo` buffer whose initial `size` must be 104. `Threads::status()` builds the named fields and the dispatcher writes the complete structure, returning zero in `v0`. Invalid IDs or buffers and unsupported structure sizes produce host exceptions. Scheduling counters and wait fields remain zero. These are the current runtime's limits, rather than a complete hardware contract; the bundled [`refer.c`](../third_party/pspautotests/tests/threads/threads/refer.c) probes additional sizes and behaviors.

## Scheduling limits

`Threads::select_next()` runs between CPU steps. The current thread continues until it exits or reaches the runtime's return sentinel after its return delay slot. The next ready thread is then selected. A game-exit service ends execution immediately. Starting a thread enqueues it without preempting the current thread.

Blocking waits, callbacks, timeouts, and timer preemption remain unsupported. Current syscall handlers complete synchronously or throw. Adding blocking waits will require pending wait records, wake-up rules, and deferred return-register writes: a blocked syscall receives its result when the wait completes. Thread switching must happen after `Cpu::step()` and syscall dispatch have unwound, so references to the previous thread state remain valid throughout the instruction.

## Verification

The [execution tests](../src/runtime/tests/execution_test.cpp) compare every output byte of `cpu_alu`, `cpu_branch2`, and `cpu_div` against the bundled expectations at two load addresses. They also cover register argument handling, startup arguments, faults, instruction budgets, and synchronization services through real guest instructions. [Kernel tests](../src/runtime/tests/kernel_test.cpp) cover ownership and scheduling boundaries; [structure tests](../src/runtime/tests/guest_structures_test.cpp) check PSP field offsets, byte order, preservation of reserved fields, and rejection of incomplete output buffers without partial writes.
