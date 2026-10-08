# SDK Hello World fixture

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
