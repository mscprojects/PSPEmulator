set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

pspdev_version := "v20261001"
pspdev_directory := env("HOME") / ".local/opt/pspdev" / pspdev_version

default: build

configure:
    cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug

build: configure
    cmake --build build --parallel "$(nproc)"

# Run tests in separate processes, one worker per available CPU.
test: build
    ctest --test-dir build --parallel "$(nproc)" --output-on-failure --no-tests=error

# Strict floating-point behavior must also hold after optimization.
release-test:
    cmake -S . -B build-release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release --parallel "$(nproc)"
    ctest --test-dir build-release --parallel "$(nproc)" --output-on-failure --no-tests=error

format: configure
    cmake --build build --target format --parallel "$(nproc)"

format-check: configure
    cmake --build build --target format-check --parallel "$(nproc)"

tidy:
    cmake -S . -B build-tidy -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_CLANG_TIDY=clang-tidy
    cmake --build build-tidy --clean-first --parallel "$(nproc)"

sanitizers:
    cmake -S . -B build-san -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPEMU_ENABLE_SANITIZERS=ON
    cmake --build build-san --parallel "$(nproc)"
    ctest --test-dir build-san --parallel "$(nproc)" --output-on-failure --no-tests=error

# Install the pinned Ubuntu x86_64 release without changing the shell environment.
setup-pspdev:
    #!/usr/bin/env bash
    set -euo pipefail
    installation="{{pspdev_directory}}"
    if [[ -e "$installation" ]]; then
        PSPDEV="$installation" "$installation/bin/psp-gcc" --version
        exit 0
    fi
    if [[ "$(uname -sm)" != "Linux x86_64" ]]; then
        echo "This PSPDEV release requires Ubuntu x86_64." >&2
        exit 1
    fi
    cache="${XDG_CACHE_HOME:-$HOME/.cache}/pspdev/{{pspdev_version}}"
    archive="$cache/pspdev-ubuntu-latest-x86_64.tar.gz"
    mkdir -p "$cache" "$(dirname "$installation")"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 --output "$archive.part" \
            "https://github.com/pspdev/pspdev/releases/download/{{pspdev_version}}/pspdev-ubuntu-latest-x86_64.tar.gz"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' a86efe624e770005290919617859e9df24bd8fcd3e364f4587f23ec7364b0acd "$archive" | sha256sum --check
    staging=$(mktemp -d "$(dirname "$installation")/.install-XXXXXX")
    trap 'rm -rf "$staging"' EXIT
    tar -xzf "$archive" -C "$staging"
    PSPDEV="$staging/pspdev" "$staging/pspdev/bin/psp-gcc" --version
    mv "$staging/pspdev" "$installation"

# Build unmodified SDK examples as PRXs outside the submodule.
homebrew: setup-pspdev
    #!/usr/bin/env bash
    set -euo pipefail
    export PSPDEV="{{pspdev_directory}}"
    export PATH="$PSPDEV/bin:$PATH"
    for sample in template/prx_template template/elf_template controller/basic; do
        source="third_party/pspsdk/src/samples/$sample"
        output="build-homebrew/$sample"
        mkdir -p "$output"
        cp -p "$source/main.c" "$output/main.c"
        cp -p "$source/Makefile.sample" "$output/Makefile"
        make -C "$output" --jobs "$(nproc)" BUILD_PRX=1 EXTRA_TARGETS=
    done

ci: format-check test release-test tidy sanitizers
