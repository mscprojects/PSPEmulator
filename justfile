set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

default: build

configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

build: configure
    cmake --build build --parallel

# Each test runs in its own process; bound concurrency for guest RAM and sanitizer overhead.
test: build
    ctest --test-dir build --parallel "${CTEST_PARALLEL_LEVEL:-4}" --output-on-failure --no-tests=error

clang-test:
    cmake -S . -B build-clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-clang --parallel
    ctest --test-dir build-clang --parallel "${CTEST_PARALLEL_LEVEL:-4}" --output-on-failure --no-tests=error

release-build:
    cmake -S . -B build-release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release --parallel

# Fast iteration on complete guest PRXs, including hardware-output comparisons.
prx-test: release-build
    ctest --test-dir build-release --parallel "${CTEST_PARALLEL_LEVEL:-4}" --tests-regex 'BundledCpu/|ExecutionTest\.Bundled(Lsu|Llsc)' --output-on-failure --no-tests=error

# Strict floating-point behavior must also hold after optimization.
release-test: release-build
    ctest --test-dir build-release --parallel "${CTEST_PARALLEL_LEVEL:-4}" --output-on-failure --no-tests=error
    cmake -S . -B build-release-clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release-clang --parallel
    ctest --test-dir build-release-clang --parallel "${CTEST_PARALLEL_LEVEL:-4}" --output-on-failure --no-tests=error

format:
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format -i

format-check:
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format --dry-run --Werror

tidy: configure
    find src -type f -name '*.cpp' -print0 | xargs -0 -r clang-tidy -p build --warnings-as-errors='*' --header-filter='^{{justfile_directory()}}/src/'

sanitizers:
    cmake -S . -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPEMU_ENABLE_SANITIZERS=ON
    cmake --build build-san --parallel
    ctest --test-dir build-san --parallel "${CTEST_PARALLEL_LEVEL:-4}" --output-on-failure --no-tests=error

ci:
    just format-check
    just test
    just clang-test
    just release-test
    just tidy
    just sanitizers
