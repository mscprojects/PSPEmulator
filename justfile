set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

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

format:
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format -i

format-check:
    find src -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format --dry-run --Werror

tidy:
    cmake -S . -B build-tidy -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_CLANG_TIDY=clang-tidy
    cmake --build build-tidy --clean-first --parallel "$(nproc)"

sanitizers:
    cmake -S . -B build-san -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPEMU_ENABLE_SANITIZERS=ON
    cmake --build build-san --parallel "$(nproc)"
    ctest --test-dir build-san --parallel "$(nproc)" --output-on-failure --no-tests=error

ci: format-check test release-test tidy sanitizers
