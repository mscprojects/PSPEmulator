# SDK homebrew fixtures

Prebuilt PRXs that let the test suite run without the PSP toolchain.

## Provenance

- Built by `just homebrew` with the pinned PSPDEV `v20261001` Ubuntu x86_64 release.
- PSPSDK revision `6f15c154c902963864ea5c4538cc94febf8a2dad`.
- GCC 15.2.0 (`1a33997924916ff5a6f61b64179ff9c8921f46c6`) and Newlib 4.5.0 (`9e0a073634ad73e8e088f2e071c55a9fe5d39709`), as recorded in the release's `build.txt`.
- Stored in Git LFS; run `git lfs pull` if a file is only a pointer.

## Fixtures

- `hello_world.prx`: unmodified SDK [PRX template](../../../../third_party/pspsdk/src/samples/template/prx_template/main.c).
  - SHA-256 `c39b124b2cd85014bed9082b8f197c6f93b32ac7f5e38bf859ce1bcc98a31d78`.
  - Must print exactly `Hello World\n` and exit with code zero within one million instructions.
  - GCC turns its `printf` into `puts`, so it exercises Newlib stream output and cleanup, not formatting.
- `screen_hello_world.prx`: unmodified SDK [screen ELF template](../../../../third_party/pspsdk/src/samples/template/elf_template/main.c), built as a PRX.
  - SHA-256 `9458a58f4fec26f497670d901b5289e08a1cd962f3e73ad3daabda9bcdef7f0c`.
  - Must exit within two million instructions and match a full-frame bitmap derived from the SDK [MSX font](../../../../third_party/pspsdk/src/debug/font.c) and [7-pixel advance](../../../../third_party/pspsdk/src/debug/scr_printf.c), independent of emulator output.
- `controller_basic.prx`: unmodified SDK [controller sample](../../../../third_party/pspsdk/src/samples/controller/basic/main.c).
  - SHA-256 `c4f5c88809321de3b545d48fc7598b0e5c7fe115cc7c609372b98137d35ae165`.
  - Must show neutral and changed stick values and every button using frozen font rows, then exit through its guest exit callback, including after an early exit request.
  - The sample's `Cicle pressed` spelling is preserved.
- `triangle.prx`: project-owned [triangle sample](../../../../homebrew/triangle/main.c) with its [Makefile](../../../../homebrew/triangle/Makefile).
  - SHA-256 `00db51e3ce2bb692a9af53f76fbbb13cc44987cffa45e472a4e32d612a2cc7ed`.
  - Draws red, green, and blue vertices at (240,40), (80,232), and (400,232) and submits FINISH ID 7.
  - Must match an analytic scanline expectation and exit only after its guest finish callback, with Home before and after rendering.
  - The expectation checks the emulator's provisional raster rules; it is not a hardware capture.
- Every fixture runs at load addresses `0x08800000` and `0x08900000`.

## Rebuild

```sh
just homebrew
cp build-homebrew/template/prx_template/template.prx src/runtime/tests/fixtures/hello_world.prx
cp build-homebrew/template/elf_template/template.prx src/runtime/tests/fixtures/screen_hello_world.prx
cp build-homebrew/controller/basic/controller_basic.prx src/runtime/tests/fixtures/controller_basic.prx
cp build-homebrew/triangle/triangle.prx src/runtime/tests/fixtures/triangle.prx
sha256sum src/runtime/tests/fixtures/*.prx
just test
```

- Update the checksums above when replacing a binary.
- Interactive fixtures can also be checked with `./build/pspemu <fixture> --window`.

## Licenses

- SDK code: [PSPSDK license](../../../../third_party/pspsdk/LICENSE), also reproduced in [COPYING.PSPSDK](COPYING.PSPSDK) for binary redistribution.
- Linked Newlib and GCC runtime code: [COPYING.NEWLIB](COPYING.NEWLIB), [COPYING3](COPYING3), and [COPYING.RUNTIME](COPYING.RUNTIME), from the revisions above.
