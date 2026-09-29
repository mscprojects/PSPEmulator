set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

default: build

configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

build: configure
    cmake --build build --parallel

test: build
    ctest --test-dir build --output-on-failure --no-tests=error

format:
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format -i

format-check:
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format --dry-run --Werror

tidy: configure
    find src -type f -name '*.cpp' -print0 | xargs -0 -r clang-tidy -p build --warnings-as-errors='*'

sanitizers:
    cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DPSPEMU_ENABLE_SANITIZERS=ON
    cmake --build build-san --parallel
    ctest --test-dir build-san --output-on-failure --no-tests=error

ci:
    just format-check
    just test
    just tidy
    just sanitizers
