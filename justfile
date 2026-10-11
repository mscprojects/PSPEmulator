set shell := ["bash", "-eu", "-o", "pipefail", "-c"]
set positional-arguments

pspdev_version := "v20261001"
pspdev_directory := env("HOME") / ".local/opt/pspdev" / pspdev_version

default: build

configure preset="debug":
    cmake --preset "$1"

build preset="debug": (configure preset)
    cmake --build --preset "$1" --parallel "$(nproc)"

# Run tests in separate processes, one worker per available CPU.
test preset="debug": (build preset)
    ctest --preset "$1" --parallel "$(nproc)"

# Select a preset explicitly, then pass the PRX path and any emulator arguments.
run preset +arguments: (build preset)
    #!/usr/bin/env bash
    set -euo pipefail
    preset="$1"
    shift
    exec "./build/$preset/pspemu" "$@"

format: configure
    cmake --build --preset debug --target format --parallel "$(nproc)"

format-check: configure
    cmake --build --preset debug --target format-check --parallel "$(nproc)"

tidy: (build "tidy")

sanitizers: (test "sanitizers")

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

# Build unmodified SDK examples and the project triangle as PRXs outside the submodule.
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
    output="build-homebrew/triangle"
    mkdir -p "$output"
    cp -p homebrew/triangle/main.c homebrew/triangle/Makefile "$output/"
    make -C "$output" --jobs "$(nproc)"

ci: format-check (test "debug") (test "release") tidy sanitizers
