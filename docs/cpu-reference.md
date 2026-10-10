# Allegrex CPU reference

Allegrex is the PSP's custom 32-bit MIPS CPU; a generic MIPS decoder is not enough. Use the MIPS manual for baseline semantics, Allegrex references for differences, and PSP hardware tests to settle uncertain behavior.

## Sources

Read in this order (links checked 2026-09-30):

1. [MIPS32 Architecture for Programmers, Volume II, rev. 0.95](https://courses.cs.washington.edu/courses/cse378/documentation/MIPS32_Architecture_For_Programmers_Volume_II.pdf): encodings, pseudocode, and exceptions. A baseline, not a list of what Allegrex supports.
2. [Allegrex instructions in the PSP VFPU docs](https://davidgfnet.github.io/psp-vfpu-docs/#allegrex-instructions) ([source and hardware tests](https://github.com/pspdev/vfpu-docs)): encoding differences for `CLZ`, `CLO`, and multiply-accumulate, plus additions such as `BITREV`, `WSBW`, `MIN`, and `MAX`.
3. [PRXTool encodings](https://github.com/pspdev/prxtool/blob/master/disasm.C): instruction values and masks for checking decoding, not semantics.
4. [PPSSPP CPU overview](https://dev.ppsspp.org/docs/psp-hardware/cpu/) and [dispatch tables](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/MIPSTables.cpp): an implementation reference, not a hardware specification.

When sources disagree:

- Record the exact encoding and inputs, compare against PSP hardware results where available, and keep the result as a regression test.
- When relying on upstream code, link the exact commit.
- Inspect the compiled PRX too: startup code and libraries can need instructions the C source does not show.

## Instruction formats

```text
R: opcode[31:26] rs[25:21] rt[20:16] rd[15:11] shamt[10:6] funct[5:0]
I: opcode[31:26] rs[25:21] rt[20:16] immediate[15:0]
J: opcode[31:26] target[25:0]
```

- Opcode zero (`SPECIAL`) selects by `funct`; other groups (`REGIMM`, `SPECIAL3`, `COP1`) use further fields.
- Arithmetic and comparison immediates are sign-extended, including `SLTIU`. `ANDI`, `ORI`, and `XORI` zero-extend.
- Example: `0x2409FFFF` is `ADDIU $t1, $zero, -1`.

## Execution model

- Each thread owns a plain `CpuState`: registers, HI/LO, FPU registers and FCR31, both instruction addresses, and the link bit.
- One shared `Cpu` holds the memory reference and runs `step(state)` on the selected thread.
- `$zero` reads as zero and ignores writes, even if a saved state was edited.
- A failing instruction commits nothing, including pending delay-slot control flow, so the host can retry it.
- Faults are reported to the host; guest exception registers and handlers are not implemented.
- A `SYSCALL` returns its code after committing control flow; see [runtime and syscall handling](/docs/runtime.md).

## Branches and calls

- Supported: `J`, `JAL`, `JR`, `JALR`, `BEQ`, `BNE`, `BLEZ`, `BGTZ`, `BLTZ`, `BGEZ`, their branch-likely and branch-and-link variants, and the scalar FPU branches.
- Ordinary branches always run the delay slot; branch-likely skips it when untaken.
- Branch offsets count words from `PC + 4`. `J`/`JAL` keep the upper four bits of `PC + 4`.
- Links save `PC + 8`, visible inside the delay slot. Branch-and-link writes the link even when untaken.
- `JALR` reads its target before writing the link register.
- A misaligned jump target faults at fetch, after the delay slot runs.
- Unsupported: control transfers inside delay slots and VFPU branches.

## Integer operations

- Arithmetic, logic, comparisons, shifts and rotates, `SEB`, `SEH`, `BITREV`, `MIN`, `MAX`, `CLZ`, `CLO`, `WSBH`, `WSBW`, `EXT`, `INS`, `MOVZ`, `MOVN`, and HI/LO multiply, divide, and multiply-accumulate.
- HI and LO start at zero; products and accumulations wrap modulo 2^64.
- Signed division truncates toward zero.
- Division by zero and signed overflow follow [`cpu_div.expected`](/third_party/pspautotests/tests/cpu/cpu_alu/cpu_div.expected). Unsigned division by zero gives `0xFFFF` when the numerator fits in 16 bits, else `0xFFFFFFFF`.
- Invalid bitfield ranges and rotate selectors fault without advancing.

## Loads and stores

- Halfword and word accesses require natural alignment. Signed loads sign-extend; unsigned loads zero-extend.
- `LWL`, `LWR`, `SWL`, and `SWR` merge bytes of the containing aligned word, little-endian (MIPS manual pp. 133–140 and 211–214).
- `LL` loads and sets the link bit. `SC` stores only if the bit is set, then writes 1 or 0 to its register.
- Allegrex specifics, from [`llsc.c`](/third_party/pspautotests/tests/cpu/lsu/llsc.c) at [1885ee4](https://github.com/hrydgard/pspautotests/blob/1885ee4ed34a03477066b249667ff90813d6b7e0/tests/cpu/lsu/llsc.c):
  - Ordinary loads and stores keep the link bit.
  - `SC` may target a different address.
  - A successful `SC` keeps the bit set.
- A syscall clears the link bit, and so does delivery of a vblank interrupt.

## Scalar FPU

State and transfers:

- 32 raw binary32 registers; transfers preserve every bit, including NaN payloads and signed zero.
- FCR31 starts at `0x00000E00`. `CFC1` reads FCR0 as `0x00003351` and other control registers as zero.
- `CTC1` writes only FCR31, masked to `0x0181FFFF`. Setting the unimplemented-operation cause, or a cause whose exception is enabled, faults.
- Values follow [`fcr.expected`](/third_party/pspautotests/tests/cpu/fpu/fcr.expected), cross-checked against PPSSPP's [interpreter](https://github.com/hrydgard/ppsspp/blob/feb6caa3c470a890e2ad2c24b95045f8f309f7b3/Core/MIPS/Interpreter.cpp) and [thread setup](https://github.com/hrydgard/ppsspp/blob/feb6caa3c470a890e2ad2c24b95045f8f309f7b3/Core/HLE/sceKernelThread.cpp).

Comparisons and branches:

- `C.cond.S` evaluates unordered, equal, and less-than on raw encodings, unaffected by host rounding or flushing.
- Quiet compares accept both NaN encodings, including libc's `0x7FBFFFFF`. Signaling compares with a NaN raise invalid operation.
- A compare reaches `BC1F`/`BC1T`/`BC1FL`/`BC1TL` one instruction later; `CFC1` and a following branch after `CTC1` see the change immediately ([`fpu_branch_hazard`](/third_party/pspautotests/tests/cpu/fpu/fpu_branch_hazard.c)).

Arithmetic and conversions:

- `ADD.S`, `SUB.S`, `MUL.S`, `DIV.S`, `SQRT.S`, `CVT.S.W`, and `CVT.W.S` use FCR31's rounding mode. `ROUND`, `TRUNC`, `CEIL`, and `FLOOR` use their fixed modes.
- Float-to-int conversion saturates out-of-range values and infinities. NaNs give `INT32_MAX`. Saturation raises invalid operation.
- NaN operands propagate the first NaN with its quiet bit set. Other invalid results are `0x7FC00000`.
- FCR31.FS flushes subnormal results to signed zero and raises underflow and inexact.
- `MOV.S`, `ABS.S`, and `NEG.S` change bits only and leave exception status alone.
- Results follow [`roundmode`](/third_party/pspautotests/tests/cpu/fpu/roundmode.expected), [`fpu_nan`](/third_party/pspautotests/tests/cpu/fpu/fpu_nan.expected), and [`fpu`](/third_party/pspautotests/tests/cpu/fpu/fpu.expected).

Exception status:

- Each operation replaces the cause bits (12–16) and accumulates the sticky flags (2–6).
- An enabled exception (enables at 7–11) faults before changing any state, so the instruction can be retried. Guest exception entry is unsupported.

Host requirements:

- [`floating_point.cpp`](/src/cpu/floating_point.cpp) owns FCR31's layout, compares, `CTC1` validation, and host evaluation.
- Host evaluation runs in a scoped environment with guest rounding and masked traps, and restores the host's environment and `errno`.
- It requires IEEE binary32 without excess precision and [Clang strict floating-point mode](https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior). Other hosts would need a software floating-point evaluator.

## Tests

- [CPU tests](/src/cpu/tests) cover single instructions without a loader or runtime.
- [Execution tests](/src/runtime/tests/execution_test.cpp) run the bundled CPU and FPU hardware tests at two load addresses and compare every output byte. The `lsu` comparison adds the final blank line the guest prints but its expectation file omits.
