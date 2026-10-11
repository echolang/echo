#!/usr/bin/env bash
#
# the GitHub Tests job, on this machine: Ubuntu 24.04, LLVM 20, RelWithDebInfo, the full suite.
#
#   tools/linux-ci.sh
#
# CI is amd64; Docker Desktop on Apple Silicon is arm64. Both are glibc, which is the half that
# aborted — `malloc_consolidate(): unaligned fastbin chunk` — and the half macOS cannot see.
# Artifacts go in `build-linux/`, not `build/`.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
image=echo-ci-linux
build_dir=build-linux

if ! command -v docker >/dev/null 2>&1; then
    echo "tools/linux-ci.sh: docker is not on PATH" >&2
    exit 2
fi

# deps only, so a failed compile does not reinstall LLVM. rebuild the image by deleting it
if ! docker image inspect "${image}" >/dev/null 2>&1; then
    docker build -t "${image}" -f - "${root}" <<'EOF'
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        wget \
        gnupg \
        lsb-release \
        software-properties-common \
        cmake \
        ninja-build \
        git \
        zlib1g-dev \
        libzstd-dev \
        libc++-dev \
        libc++abi-dev \
        python3 \
    && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh 20 \
    && apt-get install -y --no-install-recommends llvm-20-dev \
    && rm -f /tmp/llvm.sh \
    && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/lib/llvm-20/bin:$PATH
EOF
fi

docker run --rm \
    -v "${root}:/src" \
    -w /src \
    "${image}" \
    bash -lc "
        set -euo pipefail
        cmake -B ${build_dir} -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_COMPILER=/usr/lib/llvm-20/bin/clang \
            -DCMAKE_CXX_COMPILER=/usr/lib/llvm-20/bin/clang++ \
            -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm
        cmake --build ${build_dir} --target tests -j\"\$(nproc)\"
        (cd ${build_dir} && ./tests)
    "
