# Guest memory

`Memory` owns zeroed byte regions and explicit address mappings. `map_region(base, size)` adds independent storage. `map_alias(alias_base, region_base)` maps an existing region through a second address; the source must be an existing mapping start and may itself be an alias. Both operations reject empty, overlapping, or wrapping address ranges. Aliases store region indices, so adding a region does not invalidate them. Copying `Memory` copies its bytes and keeps aliases within the copy; separate emulator executions share no memory.

## PRX execution layout

`create_psp_memory()` maps the configured RAM region and these views; `prepare_prx()` places the PRX image in that layout:

- RAM at the configured canonical load address, normally `0x08800000`, with the configured size, normally 24 MiB.
- The same RAM with address bit 30 set: normally `0x48800000`, the uncached user view.
- The same RAM with bit 31 set: normally `0x88800000`, the kernel view.
- The same RAM with both bits set: normally `0xC8800000`, the uncached kernel view.
- Independent 2 MiB VRAM storage at `0x04000000` through `0x041FFFFF`.
- The same VRAM at `0x44000000` through `0x441FFFFF`, its uncached view.

RAM size continues to describe RAM alone. `prepare_prx()` reports the end of the loaded image; the kernel's stack and partition allocation arena spans from there to the end of RAM, and VRAM is outside it. The configured canonical RAM region must fit below `0x40000000` and must not overlap VRAM or its views. Relocation and module validation still require PRX records to belong to loaded segments; mapping VRAM does not make it a valid location for arbitrary module metadata.

The [PSPSDK GE header](https://github.com/pspdev/pspsdk/blob/master/src/ge/pspge.h) describes the baseline 2 MiB EDRAM size. The bundled [`setframebuf` test](../third_party/pspautotests/tests/display/setframebuf.cpp) uses both primary and uncached VRAM pointers. Address-view layout was checked against [PPSSPP's explicit memory mappings](https://github.com/hrydgard/ppsspp/blob/master/Core/MemMap.cpp). These sources serve different purposes: SDK declarations describe public interfaces, hardware tests probe behavior, and emulator mappings provide an implementation reference.

## Access rules and limits

The CPU and syscall services use the same existing scalar, byte-range, caller-buffer, and string APIs. Scalar values remain little-endian; `Memory` permits unaligned access and the CPU applies instruction-specific alignment checks. An address is resolved through an explicit mapping, without masking arbitrary addresses into valid memory. Every nonempty operation must fit within one mapped region, even when another region or alias is adjacent. Failed writes and caller-buffer reads leave the destination unchanged. Zero-length byte operations remain no-ops regardless of address. Strings stop at their first NUL and need not have the full maximum length mapped.

Cached and uncached views are coherent because they share bytes; cache lines, cache maintenance, kernel privilege checks, scratchpad, hardware registers, and specialized extra VRAM mirrors are not modeled. The extra VRAM mirrors have translation behavior probed by [`gpu/transfer/mirrors`](../third_party/pspautotests/tests/gpu/transfer/mirrors.cpp), so they are not mapped as ordinary aliases. This stage supplies pixel storage; display services, framebuffer presentation, and GPU command execution remain separate work.

## Verification

[Memory tests](../src/memory/tests/memory_test.cpp) cover shared scalar/range/string access, independent regions, alias chains, overlap and wrap rejection, boundaries, copy ownership, and preservation after faults. They also verify the PSP RAM and VRAM layout, first and last VRAM bytes, unmapped addresses, and rejected RAM placements. [Loader tests](../src/loader/tests/prx_test.cpp) check that a loaded image appears in that layout and report its end address. [Execution tests](../src/runtime/tests/execution_test.cpp) run guest instructions that store through primary VRAM and load through its uncached view, alongside the existing unmodified CPU PRX output comparisons.
