# SDK homebrew fixtures

`hello_world.prx` is the unmodified PSPSDK [PRX template](../../../../third_party/pspsdk/src/samples/template/prx_template/main.c), built by `just homebrew` with the pinned PSPDEV `v20261001` Ubuntu x86_64 release. Its source and Makefile come from PSPSDK revision `6f15c154c902963864ea5c4538cc94febf8a2dad`. The release includes GCC 15.2.0 and Newlib 4.5.0; its `build.txt` records GCC revision `1a33997924916ff5a6f61b64179ff9c8921f46c6` and Newlib revision `9e0a073634ad73e8e088f2e071c55a9fe5d39709`.

The PRX is stored in Git LFS so the ordinary emulator test suite can run without installing PSPDEV. Run `git lfs pull` if the working copy contains an LFS pointer instead of the binary. Its SHA-256 is `c39b124b2cd85014bed9082b8f197c6f93b32ac7f5e38bf859ce1bcc98a31d78`.

To rebuild and replace it from the pinned sources:

```sh
just homebrew
cp build-homebrew/template/prx_template/template.prx src/runtime/tests/fixtures/hello_world.prx
sha256sum src/runtime/tests/fixtures/hello_world.prx
just test
```

Update the recorded checksum when replacing the binary. The test executes it at two load addresses and requires exactly `Hello World\n`, exit code zero, and completion within one million instructions. GCC optimizes this sample's constant `printf` call into `puts`, so it exercises Newlib stream output and cleanup rather than format conversion.

The sample and SDK code carry the [PSPSDK license](../../../../third_party/pspsdk/LICENSE). Notices for the linked Newlib and GCC runtime code are reproduced in [COPYING.NEWLIB](COPYING.NEWLIB), [COPYING3](COPYING3), and [COPYING.RUNTIME](COPYING.RUNTIME), copied from the revisions above.

## Screen Hello World

`screen_hello_world.prx` is the unmodified SDK [screen ELF template](../../../../third_party/pspsdk/src/samples/template/elf_template/main.c), built as a PRX with the same pinned toolchain and SDK revisions above. It initializes a 480 × 272 RGBA 8888 framebuffer in uncached VRAM with a 512-pixel stride and calls `pspDebugScreenPrintf("Hello World\n")`. Its SHA-256 is `9458a58f4fec26f497670d901b5289e08a1cd962f3e73ad3daabda9bcdef7f0c`.

```sh
just homebrew
cp build-homebrew/template/elf_template/template.prx src/runtime/tests/fixtures/screen_hello_world.prx
sha256sum src/runtime/tests/fixtures/screen_hello_world.prx
just test
./build/pspemu src/runtime/tests/fixtures/screen_hello_world.prx --window
```

The test executes it at two load addresses, requires successful termination within two million instructions, and compares the entire captured image against a fixed bitmap established from the pinned SDK [MSX font](../../../../third_party/pspsdk/src/debug/font.c) and the seven-pixel character advance in [scr_printf.c](../../../../third_party/pspsdk/src/debug/scr_printf.c). White text occupies the top eight rows; all remaining pixels are opaque black. This expectation is independent of emulator output. The SDL smoke test also reads back the rendered pixels at 2× scale and after resizing to a square window. This fixture is stored in Git LFS and covered by the same license notices above.

## Controller basic

`controller_basic.prx` is the unmodified SDK [controller sample](../../../../third_party/pspsdk/src/samples/controller/basic/main.c), built with the same pinned toolchain and SDK revisions above. Its SHA-256 is `c4f5c88809321de3b545d48fc7598b0e5c7fe115cc7c609372b98137d35ae165`. It is stored in Git LFS and covered by the same license notices.

```sh
just homebrew
cp build-homebrew/controller/basic/controller_basic.prx src/runtime/tests/fixtures/controller_basic.prx
sha256sum src/runtime/tests/fixtures/controller_basic.prx
just test
./build/pspemu src/runtime/tests/fixtures/controller_basic.prx --window
```

Tests run it at two load addresses, compare its analog and button text against frozen rows from the pinned MSX font, release input back to neutral, and request exit. Successful termination requires the guest callback to write the sample's `done` variable and main to call `sceKernelExitGame`. An early exit request also verifies registration and callback-enabled sleep. The integration test covers the optional right-stick fields through the SDL-independent execution API; the keyboard frontend controls only the left stick. The sample's `Cicle pressed` spelling is preserved. Keyboard controls are documented in the [project README](../../../../README.md).
