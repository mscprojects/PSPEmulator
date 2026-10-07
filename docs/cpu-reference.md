# Allegrex CPU reference

Use the MIPS instruction manual for baseline instruction semantics, Allegrex-specific references for differences, and PSP hardware tests to resolve uncertain behavior. Allegrex is the PSP's custom 32-bit MIPS CPU; a generic MIPS decoder alone is insufficient.

## Reading order

1. [MIPS32 Architecture for Programmers, Volume II: The MIPS32 Instruction Set, revision 0.95](https://courses.cs.washington.edu/courses/cse378/documentation/MIPS32_Architecture_For_Programmers_Volume_II.pdf). This is a MIPS Technologies manual hosted by the University of Washington. Instruction entries describe encodings, operands, operation pseudocode, restrictions, and exceptions. Start with `BEQ`, `BEQL`, `J`, `JAL`, `JR`, and `JALR` for branching and calls. Treat it as a baseline reference, rather than a list of instructions Allegrex necessarily supports.
2. [Allegrex instructions in the PSP VFPU documentation](https://davidgfnet.github.io/psp-vfpu-docs/#allegrex-instructions). Despite the document's title, this section covers integer instructions too. It identifies encoding differences for `CLZ`, `CLO`, and multiply-accumulate instructions, plus additions such as `BITREV`, `WSBW`, `MIN`, and `MAX`. The [source project](https://github.com/pspdev/vfpu-docs) includes hardware validation tests.
3. [PRXTool instruction encodings](https://github.com/pspdev/prxtool/blob/master/disasm.C). Its instruction values and masks are useful for checking Allegrex decoding. A disassembler identifies instructions; it does not specify their execution semantics.
4. [PPSSPP CPU overview](https://dev.ppsspp.org/docs/psp-hardware/cpu/) and [instruction dispatch tables](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/MIPSTables.cpp). Follow the tables into the interpreter handlers under `Core/MIPS` when checking behavior. These are implementation references, not hardware specifications.

Links were checked on 2026-09-30. This guide contains project notes and links; it does not include copies of the upstream manuals or source code. When recording a behavior discovered in upstream code, link the exact commit and add a focused test here.

## Instruction formats

Instructions are 32-bit words. Bit 31 is the most significant bit; bit 0 is the least significant. `Memory::read_u32` assembles the little-endian bytes into that word before decoding.

The common integer layouts are:

```text
R: opcode[31:26] rs[25:21] rt[20:16] rd[15:11] shamt[10:6] funct[5:0]
I: opcode[31:26] rs[25:21] rt[20:16] immediate[15:0]
J: opcode[31:26] target[25:0]
```

`rs`, `rt`, and `rd` are five-bit register fields, selecting registers 0 through 31. Their roles depend on the instruction: `ADDU` reads `rs` and `rt` and writes `rd`; `ADDIU` reads `rs` and writes `rt`; `SW` uses `rs` as the address base and reads `rt` as the value to store. `shamt` is the fixed shift amount.

The six-bit primary opcode can identify an operation or a decoding group. Opcode zero is the MIPS `SPECIAL` group, where `funct` selects operations such as `SLL`, `ADDU`, and `JR`. Other groups can use additional fields to select the operation. Floating-point and VFPU instructions need their own layouts.

For example, `0x2409FFFF` encodes `ADDIU $t1, $zero, -1`: opcode `0x09`, `rs = 0`, `rt = 9`, and immediate `0xFFFF`. Arithmetic and comparison immediates are sign-extended, including `SLTIU`; the logical immediates in `ANDI`, `ORI`, and `XORI` are zero-extended. `LUI` places its immediate in the upper half of the result.

These layouts follow the MIPS manual and PRXTool encodings linked above. Check Allegrex-specific encodings before adding instructions.

## Verification in this repository

- [cpu_alu source](../third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.c) and [expected output](../third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.expected) define the executable integration-test target.
- [CPU unit tests](../src/cpu/tests/cpu_test.cpp) verify individual instruction effects and execution order without needing a loader or PSP runtime.
- Inspect the compiled PRX as well as the C source: compiler output, startup code, and linked libraries can require additional instructions.

If sources disagree, record the exact instruction encoding and inputs, compare against PSP hardware results where available, and preserve the result in a regression test.

## Branching and calls

The interpreter implements `J`, `JAL`, `JR`, `JALR`, `BEQ`, `BNE`, `BLEZ`, `BGTZ`, `BLTZ`, and `BGEZ`, plus integer branch-likely and branch-and-link variants. Each opcode states its condition directly. Ordinary branches execute the delay-slot instruction whether taken or untaken; branch-likely instructions skip it when untaken. Scalar FPU branches are supported as described below; VFPU branches remain unsupported.

Each thread’s `CpuState` keeps its registers, HI/LO, current instruction address, and next instruction address. A shared `Cpu` interpreter holds the memory reference and accepts the selected state for each instruction. The state is a plain struct with public fields; copying it also preserves a pending delay-slot target. The interpreter clears the `$zero` slot before reading operands and discards instruction writes to it, even if a caller edited that slot in a saved state. Instruction alignment is checked when fetching. `step(state)` stages their successors locally and commits them only after successful execution, preserving the pending target when a delay-slot instruction fails. Exceptions are reported to the host; guest exception registers and exception handlers are not implemented. Control transfers inside delay slots are outside this milestone's supported behavior.

Branch offsets count words relative to `PC + 4`; `J` and `JAL` combine their 26-bit word target with the upper four bits of `PC + 4`. Link instructions save `PC + 8`, which is already visible to the delay-slot instruction. Branch-and-link instructions write the link even when untaken. `JALR` reads its target before writing the destination register. A misaligned register jump faults when fetching the target, after the delay slot executes.

[CPU tests](../src/cpu/tests/cpu_test.cpp) cover taken and untaken conditions for every integer branch variant, signed boundaries, a backward loop, function calls and returns, `JALR` register aliasing, jump region boundaries, skipped invalid delay slots, and retry after a failed delay slot. The [PPSSPP interpreter](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/Interpreter.cpp) is a cross-check for branch and link behavior.

## Integer ALU support for cpu_alu

The interpreter implements the integer operations exercised by `cpu_alu.c`: basic arithmetic and logic, comparisons, fixed and variable shifts, rotations, `SEB`, `SEH`, `BITREV`, `MIN`, `MAX`, `CLZ`, `CLO`, `WSBH`, `WSBW`, `EXT`, `INS`, and HI/LO multiply, divide, and multiply-accumulate operations. `MOVZ`, `MOVN`, and `SRLV` are also implemented for compiled integer code.

HI and LO start at zero and are accessible through `MFHI`, `MFLO`, `MTHI`, and `MTLO`. Products and accumulations preserve the 64-bit result modulo 2^64, including signed multiplication. Signed division truncates toward zero. Division-by-zero and signed overflow results follow the bundled [cpu_div hardware expectations](../third_party/pspautotests/tests/cpu/cpu_alu/cpu_div.expected); notably, unsigned division by zero gives a quotient of `0xFFFF` when the numerator fits in 16 bits, otherwise `0xFFFFFFFF`.

Tests cover each added instruction, boundary shift counts, signedness, full-width bitfields, register aliasing, HI/LO carry and wraparound, division edge cases, and writes to `$zero`. Invalid bitfield ranges and unsupported rotate selectors are rejected before advancing execution.

The [runtime integration tests](../src/runtime/tests/execution_test.cpp) execute the bundled `cpu_alu.prx`, `cpu_branch2.prx`, and `cpu_div.prx` from their entry points at two load addresses, verify successful termination, and compare their entire output with the respective `.expected` files. The runtime supplies the startup and I/O services used by these paths; guest libc performs the formatting. Scalar FPU operations are described below. VFPU operations remain unsupported.

## Unaligned word loads and stores

The interpreter implements `LWL`, `LWR`, `SWL`, and `SWR` using the little-endian byte layouts in the MIPS instruction manual linked above (pages 133–140 and 211–214). Each instruction computes its address with a signed 16-bit offset, accesses the containing aligned word, and merges selected bytes while preserving the rest. A complementary left/right pair loads or stores an unaligned word spanning two aligned words, in either instruction order. Load results are available to the next instruction; writes to `$zero` are discarded after performing the memory access.

CPU tests cover every byte offset, negative offsets, left/right pairs in both orders, base/destination register aliasing, `$zero`, and faults without modifying registers, memory, or pending control flow. The runtime integration test executes the bundled `lsu.prx` at two load addresses and compares the complete output with `lsu.expected` plus the final blank line emitted by `lsu.c` but omitted from that expectation file. The emulator preserves the guest output as written.

## Linked word loads and conditional stores

`LL` loads an aligned word and sets `CpuState::load_linked`. `SC` stores its register value only when that bit is set, then replaces the same register with one for success or zero for failure. The address is calculated before modifying the register, including when the base and value register are the same. A syscall clears the bit before returning its event to the runtime. The runtime also clears it when the HLE kernel delivers a pending vblank interrupt between instructions.

The [bundled PSP hardware test](../third_party/pspautotests/tests/cpu/lsu/llsc.c) and [expected output](../third_party/pspautotests/tests/cpu/lsu/llsc.expected), pinned at [1885ee4](https://github.com/hrydgard/pspautotests/blob/1885ee4ed34a03477066b249667ff90813d6b7e0/tests/cpu/lsu/llsc.c), define the Allegrex-specific behavior: ordinary loads and stores leave the bit set, `SC` can target a different address, and successful `SC` does not clear the bit. CPU tests cover those cases, repeated failures, syscall invalidation and rearming, signed offsets, register aliasing, `$zero`, saved-state copies, and faults that preserve the uncommitted instruction state.

The runtime supports time queries, thread delays, and periodic HLE vblank interrupts. The unmodified bundled `llsc.prx` now matches its entire hardware expectation at two load addresses, including its long interrupt spin. The HLE interrupt path preserves registers and pending branch control flow while clearing the link bit. Guest exception-vector entry and guest interrupt handlers remain unimplemented; host-reported CPU faults retain the existing retry behavior rather than simulating exception entry. See [periodic interrupts](runtime.md#periodic-interrupts) for timing, masking, and current limits.

## Scalar FPU

Each `CpuState` owns 32 raw binary32 register words, FCR31 initialized to the PSP thread startup value `0x00000E00`, and the condition currently visible to FPU branches. Copying a saved state preserves all three. `MFC1`, `MTC1`, `MOV.S`, `LWC1`, and `SWC1` preserve every payload bit, including NaNs and signed zero. `$f0` is writable; the integer `$zero` rules still apply. Loads and stores use the integer memory alignment and fault contracts.

`CFC1` reads FCR0 as `0x00003351`, FCR31 as saved, and other control registers as zero. Only FCR31 is writable through `CTC1`; its supported bits are masked with `0x0181FFFF`. The initial values and write/readback cases follow the bundled [FCR hardware expectation](../third_party/pspautotests/tests/cpu/fpu/fcr.expected). Control-register encodings and the writable mask were cross-checked against the [PPSSPP interpreter](https://github.com/hrydgard/ppsspp/blob/feb6caa3c470a890e2ad2c24b95045f8f309f7b3/Core/MIPS/Interpreter.cpp); PSP thread initialization was checked in its [thread implementation](https://github.com/hrydgard/ppsspp/blob/feb6caa3c470a890e2ad2c24b95045f8f309f7b3/Core/HLE/sceKernelThread.cpp). Setting the unimplemented-operation exception bit or a cause whose enable is set reports a host fault. Guest exception entry remains unsupported.

Single-precision comparisons implement the unordered, equal, and less-than predicates and update condition bit 23 and exception status while preserving rounding, enables, and FS. Comparisons use binary32 encodings directly so host floating-point rounding and flushing cannot affect signed zeros, subnormals, infinities, or quiet NaNs. Quiet comparisons accept both NaN encodings, including the libc `0x7FBFFFFF` value used by the bundled `fpu.prx`. A signaling predicate with a NaN raises invalid operation; disabled exceptions update FCR31, and enabled exceptions report a host fault before modifying state.

`BC1F`, `BC1T`, `BC1FL`, and `BC1TL` use the existing branch-address and delay-slot rules. A comparison becomes visible to branches after one intervening instruction; `CFC1` sees its result immediately. `CTC1` is immediately visible to a following branch. This distinction follows the bundled [branch-hazard source](../third_party/pspautotests/tests/cpu/fpu/fpu_branch_hazard.c) and [hardware expectation](../third_party/pspautotests/tests/cpu/fpu/fpu_branch_hazard.expected). The delayed branch condition is committed only when an instruction completes successfully, preserving retry behavior after a fault.

[Focused FPU tests](../src/cpu/tests/cpu_fpu_test.cpp) cover raw transfers, control-register readbacks, comparison predicates, branch outcomes and delay slots, saved-state isolation and comparison latency, and memory faults. The runtime executes unmodified `fpu_branch.prx` and `fpu_branch_hazard.prx` at two load addresses and compares every output byte with their bundled expectations. The remaining FPU integration targets are described below.

### Arithmetic, conversions, and exception status

`ADD.S`, `SUB.S`, `MUL.S`, `DIV.S`, and `SQRT.S` evaluate binary32 operands under the rounding mode in FCR31 bits 0–1: nearest with ties to even, toward zero, toward positive infinity, and toward negative infinity. `CVT.S.W` converts signed 32-bit integer bits to binary32 under that mode. `CVT.W.S` follows it too; `ROUND.W.S`, `TRUNC.W.S`, `CEIL.W.S`, and `FLOOR.W.S` each use their fixed mode. Float-to-int results saturate to `INT32_MAX` or `INT32_MIN` for out-of-range inputs and infinities; NaNs saturate to `INT32_MAX` regardless of sign. Saturation raises invalid operation, and an inexact valid conversion raises inexact. `ABS.S` and `NEG.S`, like `MOV.S`, change bits without changing exception status.

Arithmetic with a NaN raises invalid operation and propagates the first NaN operand's sign and payload, setting its quiet bit. Invalid operations without a NaN operand create the positive canonical `0x7FC00000` NaN. FCR31.FS flushes subnormal arithmetic results to signed zero and raises underflow and inexact. These result contracts follow the bundled [roundmode expectation](../third_party/pspautotests/tests/cpu/fpu/roundmode.expected), [NaN expectation](../third_party/pspautotests/tests/cpu/fpu/fpu_nan.expected), and [FPU expectation](../third_party/pspautotests/tests/cpu/fpu/fpu.expected).

Arithmetic, conversions, and comparisons replace the five cause bits at 12–16 and accumulate the sticky flags at 2–6 for inexact, underflow, overflow, divide by zero, and invalid operation. If an exception's enable at 7–11 is set, execution reports a host fault with guest instruction context. The instruction leaves its destination, FCR31, branch-visible condition, and pending control flow unchanged, allowing a retry after the caller disables the exception. This host-fault contract does not simulate PSP exception-vector entry. The bundled `fcr.prx` verifies disabled-exception flag results; focused tests cover enabled exceptions and retry.

[`floating_point.cpp`](../src/cpu/floating_point.cpp) confines host floating-point evaluation to a scoped environment with guest rounding and masked host traps. It restores the caller's rounding, flags, flushing state, and `errno` on return. Host exception flags supply the ordinary arithmetic status; PSP NaN, saturation, and FS behavior are handled explicitly. This requires IEEE binary32 evaluation without excess precision and working host floating-point environment support. CMake enables [Clang strict floating-point mode](https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior). The [C++ floating-point environment specification](https://eel.is/c++draft/cfenv) describes the per-host-thread environment. Supporting a host without these facilities would require a software floating-point evaluator.

Integration tests execute unmodified `fpu`, `roundmode`, `rounding`, `fpu_nan`, and `fcr` PRXs at both load addresses and compare every output byte. Together with the branch and hazard PRXs, these cover all seven bundled scalar FPU tests. Focused tests also verify destination aliasing, reserved encodings, sticky flags and cause replacement, host environment isolation, and fault retry. Optimized Clang runs validate that rounding remains effective under optimization.

## Runtime boundary

`Cpu::step(CpuState &)` returns an optional `Syscall` event containing its encoded code and instruction address after committing instruction control flow. In an import stub, `JR $ra` schedules the return and its delay-slot `SYSCALL` reports a service to the runtime. `SyscallDispatcher` resolves the code to a library name and NID, translates register arguments into named service operations, and supplies results in guest registers. Execution resumes at the committed return target. CPU instruction semantics do not depend on PSP service implementations. See [runtime and syscall handling](runtime.md) for component ownership, guest structures, and scheduling behavior.

Byte and halfword loads and stores (`LB`, `LBU`, `LH`, `LHU`, `SB`, `SH`) support the compiled startup and libc code. Signed loads extend their sign bits; unsigned loads zero-extend. Halfword and word accesses require their respective alignments. Memory bounds are checked before a register or memory write.

Service identifiers and signatures were checked against the [PSPSDK import tables](https://github.com/pspdev/pspsdk/tree/master/src/user) and PPSSPP's [thread](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelThread.cpp), [memory](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelMemory.cpp), and [I/O](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceIo.cpp) dispatch tables. The bundled autotest common code defines the emulator device commands for probing, headless display detection, and output capture.

The `cpu_div` libc startup also uses lightweight mutexes, semaphores, thread status, and timezone queries. Work-area and status layouts and synchronization signatures follow the [PSPSDK thread header](https://github.com/pspdev/pspsdk/blob/master/src/user/pspthreadman.h); timezone identifiers follow the [system parameter header](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_sysparam.h), checked on 2026-10-04. The runtime maintains mutex identity, ownership and recursive counts, and semaphore counts with their maximum. Only uncontended mutex locks and immediately satisfiable semaphore waits with no timeout are supported; blocking waits and callbacks require scheduler support. UTC and disabled daylight saving are deterministic runtime settings. Focused guest-program tests verify mutex initialization/deletion, recursive lock counts and underflow, and semaphore consumption/replenishment and blocking rejection.
