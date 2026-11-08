#!/usr/bin/env bash

set -euo pipefail

for name in LLVM_PATH VERSION ECO_VERSION_PATCH BIN_NAME EPM_BIN_NAME RUNNER_TEMP; do
    if [ -z "${!name:-}" ]; then
        echo "build_release_unix: ${name} is not set" >&2
        exit 2
    fi
done

case "$(uname -s)" in
    Darwin)
        cc=/usr/bin/clang
        cxx=/usr/bin/clang++
        ;;
    *)
        cc="${LLVM_PATH}/bin/clang"
        cxx="${LLVM_PATH}/bin/clang++"
        ;;
esac

launcher=()
if command -v ccache >/dev/null 2>&1; then
    launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# the binary that ships. The stdlib goes inside it, because the path a plain build reads it from is
# this runner's and means nothing to whoever downloads the result
cmake -B build-release -G Ninja \
    ${launcher[@]+"${launcher[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${cc}" \
    -DCMAKE_CXX_COMPILER="${cxx}" \
    -DLLVM_DIR="${LLVM_PATH}/lib/cmake/llvm" \
    -DECO_EMBED_STDLIB=ON \
    -DECO_STATIC_ZSTD=ON \
    -DECO_VERSION_PATCH="${ECO_VERSION_PATCH}" \
    -DECO_VERSION_SUFFIX="${ECO_VERSION_SUFFIX:-}"

cmake --build build-release --config Release --target echoc

root="$(pwd)"
cp build-release/echoc "${RUNNER_TEMP}/echoc"

reported=$("${RUNNER_TEMP}/echoc" --version)
if [ "${reported}" != "${VERSION}" ]; then
    echo "The binary reports '${reported}', expected '${VERSION}'." >&2
    exit 1
fi

# from outside the checkout, and using the standard library: the one thing that proves the
# stdlib is really inside the binary rather than being read from this runner's source tree
(
    cd "${RUNNER_TEMP}"
    printf 'assert(1 == 1);\nvar $xs = [1, 2, 3];\ndprint($xs[1]);\n' > smoke.eco
    ./echoc run smoke.eco
    if [ -n "${WASI_SDK_PATH:-}" ]; then
        ./echoc build --target-os wasi -p ir -o smoke.wasm smoke.eco > smoke.ir
        if grep -q pthread smoke.ir; then
            echo "embedded wasi IR still names pthread; stdlib thread should be dropped." >&2
            exit 1
        fi
    fi
)

# the smoke test above runs on the machine that built the binary, so it cannot see a dependency
# on something only that machine has - which the loader turns into a process that never starts
./tools/check_release_deps.sh build-release/echoc

# epm is Echo, not a CMake target. The compiler that just shipped is the one that compiles it.
# libjson and libcurl are path-depends next to this checkout; the script clones them there.
# static curl is built in this job so the released epm does not NEED libcurl.so.
./tools/build_static_curl.sh "${RUNNER_TEMP}/static-curl"
./tools/build_release_epm.sh build-release/echoc "${RUNNER_TEMP}/epm" "${RUNNER_TEMP}/static-curl" "${VERSION}"

reported=$("${RUNNER_TEMP}/epm" --version)
if [ "${reported}" != "${VERSION}" ]; then
    echo "epm reports '${reported}', expected '${VERSION}'." >&2
    exit 1
fi

./tools/check_release_deps.sh "${RUNNER_TEMP}/epm"

rm -rf stage epm-stage
mkdir -p stage epm-stage
if [ "$(uname -s)" = "Linux" ]; then
    ./tools/linux/bundle_musl.sh stage/sysroot
fi
cp build-release/echoc stage/
cp "${RUNNER_TEMP}/epm" stage/
cp "${RUNNER_TEMP}/epm" epm-stage/
cp README.md LICENSE stage/
cp LICENSE epm-stage/
tar -czf "${BIN_NAME}.tar.gz" -C stage .
tar -czf "${EPM_BIN_NAME}.tar.gz" -C epm-stage .

if [ "$(uname -s)" = "Linux" ]; then
    (
        cd "${RUNNER_TEMP}"
        printf 'echo 1.5;\n' > smoke-musl.eco
        "${root}/stage/echoc" build --target-libc musl -o smoke-musl smoke-musl.eco
        if ldd ./smoke-musl >ldd.out 2>&1; then
            echo "musl command is dynamic:" >&2
            cat ldd.out >&2
            exit 1
        fi
        grep -q 'not a dynamic executable' ldd.out
        test "$(./smoke-musl)" = "1.500000"
    )
fi
