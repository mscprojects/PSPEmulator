# Guest memory

## Memory

- `Memory` owns zeroed byte regions and the address mappings that reach them.
- `map_region(base, size)` adds independent storage; `map_alias(alias, region_base)` maps an existing region at a second address.
- Mappings reject empty, overlapping, and wrapping ranges.
- Copying `Memory` copies all bytes and keeps aliases inside the copy, so executions never share memory.

## PSP layout

`create_psp_memory()` builds this layout; `prepare_prx()` loads the PRX into it.

- RAM at the load address, normally `0x08800000`, 24 MiB by default.
- RAM views with address bit 30 (`0x48800000`, uncached), bit 31 (`0x88800000`, kernel), and both (`0xC8800000`).
- 2 MiB VRAM at `0x04000000`, with its uncached view at `0x44000000`.
- RAM must fit below `0x40000000` and must not overlap VRAM.
- The allocation arena spans from the loaded image's end to the end of RAM; VRAM is outside it.
- Mapping VRAM does not make it valid storage for PRX metadata.

## Access rules

- Values are little-endian. `Memory` allows unaligned access; the CPU applies instruction alignment rules.
- Every nonempty access must fit inside one mapping, even when another mapping is adjacent. Invalid accesses throw `std::out_of_range`.
- Failed writes and caller-buffer reads change nothing. Zero-length operations are no-ops.
- Strings stop at the first NUL and need not have their full maximum length mapped.
- Cached and uncached views share bytes, so they are always coherent.
- Not modeled: caches, privilege checks, scratchpad, hardware registers, and the extra VRAM mirrors probed by [`gpu/transfer/mirrors`](../third_party/pspautotests/tests/gpu/transfer/mirrors.cpp).

## Sources

- [PSPSDK GE header](../third_party/pspsdk/src/ge/pspge.h): 2 MiB EDRAM.
- [`setframebuf` test](../third_party/pspautotests/tests/display/setframebuf.cpp): uses cached and uncached VRAM pointers.
- [PPSSPP memory map](https://github.com/hrydgard/ppsspp/blob/master/Core/MemMap.cpp): address-view layout reference.
