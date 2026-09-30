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

- [cpu_alu source](../third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.c) and [expected output](../third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.expected) define the next executable-test target.
- [CPU unit tests](../src/cpu/tests/cpu_test.cpp) verify individual instruction effects and execution order without needing a loader or PSP runtime.
- Inspect the compiled PRX as well as the C source: compiler output, startup code, and linked libraries can require additional instructions.

If sources disagree, record the exact instruction encoding and inputs, compare against PSP hardware results where available, and preserve the result in a regression test.

## Proposed CPU changes for branching

Keep the current interpreter and opcode switches. The next implementation should introduce current and next instruction addresses so the delay-slot instruction executes before a branch target. Handle the untaken branch-likely case explicitly because it skips the delay slot. Preserve the current contract that a rejected instruction does not advance execution.

Read jump operands before writing link registers, including when `JALR` uses the same register for both. Add tests for taken and untaken branches, backward loops, delay-slot effects, and calls returning after the delay slot.

Extract instruction-field accessors only when they remove duplication or gain another caller. A shared decoder becomes useful when tracing or disassembly needs the same instruction identification. Add HI/LO state with the multiply/divide instructions that need it.
