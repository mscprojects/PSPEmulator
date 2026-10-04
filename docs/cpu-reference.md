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

The interpreter implements `J`, `JAL`, `JR`, `JALR`, `BEQ`, `BNE`, `BLEZ`, `BGTZ`, `BLTZ`, and `BGEZ`, plus integer branch-likely and branch-and-link variants. Each opcode states its condition directly. Ordinary branches execute the delay-slot instruction whether taken or untaken; branch-likely instructions skip it when untaken. FPU and VFPU branches await their respective register state and instruction support.

Each thread’s `CpuState` keeps its registers, HI/LO, current instruction address, and next instruction address. A shared `Cpu` interpreter holds the memory reference and accepts the selected state for each instruction. The state is a plain struct with public fields; copying it also preserves a pending delay-slot target. The interpreter clears the `$zero` slot before reading operands and discards instruction writes to it, even if a caller edited that slot in a saved state. Instruction alignment is checked when fetching. `step(state)` stages their successors locally and commits them only after successful execution, preserving the pending target when a delay-slot instruction fails. Exceptions are reported to the host; guest exception registers and exception handlers are not implemented. Control transfers inside delay slots are outside this milestone's supported behavior.

Branch offsets count words relative to `PC + 4`; `J` and `JAL` combine their 26-bit word target with the upper four bits of `PC + 4`. Link instructions save `PC + 8`, which is already visible to the delay-slot instruction. Branch-and-link instructions write the link even when untaken. `JALR` reads its target before writing the destination register. A misaligned register jump faults when fetching the target, after the delay slot executes.

[CPU tests](../src/cpu/tests/cpu_test.cpp) cover taken and untaken conditions for every integer branch variant, signed boundaries, a backward loop, function calls and returns, `JALR` register aliasing, jump region boundaries, skipped invalid delay slots, and retry after a failed delay slot. The [PPSSPP interpreter](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/Interpreter.cpp) is a cross-check for branch and link behavior.

## Integer ALU support for cpu_alu

The interpreter implements the integer operations exercised by `cpu_alu.c`: basic arithmetic and logic, comparisons, fixed and variable shifts, rotations, `SEB`, `SEH`, `BITREV`, `MIN`, `MAX`, `CLZ`, `CLO`, `WSBH`, `WSBW`, `EXT`, `INS`, and HI/LO multiply, divide, and multiply-accumulate operations. `MOVZ`, `MOVN`, and `SRLV` are also implemented for compiled integer code.

HI and LO start at zero and are accessible through `MFHI`, `MFLO`, `MTHI`, and `MTLO`. Products and accumulations preserve the 64-bit result modulo 2^64, including signed multiplication. Signed division truncates toward zero. Division-by-zero and signed overflow results follow the bundled [cpu_div hardware expectations](../third_party/pspautotests/tests/cpu/cpu_alu/cpu_div.expected); notably, unsigned division by zero gives a quotient of `0xFFFF` when the numerator fits in 16 bits, otherwise `0xFFFFFFFF`.

Tests cover each added instruction, boundary shift counts, signedness, full-width bitfields, register aliasing, HI/LO carry and wraparound, division edge cases, and writes to `$zero`. Invalid bitfield ranges and unsupported rotate selectors are rejected before advancing execution.

The [runtime integration tests](../src/runtime/tests/execution_test.cpp) execute the bundled `cpu_alu.prx`, `cpu_branch2.prx`, and `cpu_div.prx` from their entry points at two load addresses, verify successful termination, and compare their entire output with the respective `.expected` files. The runtime supplies the startup and I/O services used by these paths; guest libc performs the formatting. Other instruction families, including FPU and VFPU operations, remain unsupported.

## Runtime boundary

`Cpu::step(CpuState &)` returns an optional syscall code after committing instruction control flow. In an import stub, `JR $ra` schedules the return and its delay-slot `SYSCALL` reports a service to the runtime. The runtime dispatches it by library name and NID, supplies results in guest registers, and resumes execution at the committed return target. CPU instruction semantics do not depend on PSP service implementations.

Byte and halfword loads and stores (`LB`, `LBU`, `LH`, `LHU`, `SB`, `SH`) support the compiled startup and libc code. Signed loads extend their sign bits; unsigned loads zero-extend. Halfword and word accesses require their respective alignments. Memory bounds are checked before a register or memory write.

Service identifiers and signatures were checked against the [PSPSDK import tables](https://github.com/pspdev/pspsdk/tree/master/src/user) and PPSSPP's [thread](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelThread.cpp), [memory](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelMemory.cpp), and [I/O](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceIo.cpp) dispatch tables. The bundled autotest common code defines the emulator device commands for probing, headless display detection, and output capture.

The `cpu_div` libc startup also uses lightweight mutexes, semaphores, thread status, and timezone queries. Work-area and status layouts and synchronization signatures follow the [PSPSDK thread header](https://github.com/pspdev/pspsdk/blob/master/src/user/pspthreadman.h); timezone identifiers follow the [system parameter header](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_sysparam.h), checked on 2026-10-04. The runtime maintains mutex identity, ownership and recursive counts, and semaphore counts with their maximum. Only uncontended mutex locks and immediately satisfiable semaphore waits with no timeout are supported; blocking waits and callbacks require scheduler support. UTC and disabled daylight saving are deterministic runtime settings. Focused guest-program tests verify mutex initialization/deletion, recursive lock counts and underflow, and semaphore consumption/replenishment and blocking rejection.
