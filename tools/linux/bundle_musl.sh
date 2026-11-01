#!/usr/bin/env bash
#
# build a musl sysroot into <dest>:
#
#   tools/linux/bundle_musl.sh stage/sysroot
#
# libc.a, include/, musl crt objects, and clang's compiler-rt builtins copied
# from this machine's clang. `--target-libc musl` passes `--sysroot` with
# `-static -nostdlib`, then those files, so the link does not need a GCC
# install next to clang. a released Linux echoc looks here. native only.

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: bundle_musl.sh <dest-sysroot>" >&2
    exit 2
fi

dest=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
MUSL_VER=1.2.5
MUSL_SHA=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
PATCH_DIR=$(cd "$(dirname "$0")" && pwd)/patches
# upstream fixes not in a 1.2.5 release, applied in order. drop them on a musl >= 1.2.7
PATCHES="CVE-2025-26519-1 CVE-2025-26519-2 CVE-2026-6042 CVE-2026-40200"

work=$(mktemp -d)
trap 'rm -rf "${work}"' EXIT

echo "bundle_musl: fetching musl ${MUSL_VER}"
curl -fsSL --proto '=https' --tlsv1.2 -o "${work}/musl.tar.gz" "${MUSL_URL}"

if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "${work}/musl.tar.gz" | awk '{ print $1 }')
else
    actual=$(shasum -a 256 "${work}/musl.tar.gz" | awk '{ print $1 }')
fi

if [ "${actual}" != "${MUSL_SHA}" ]; then
    echo "bundle_musl: musl-${MUSL_VER}.tar.gz sha256 ${actual} != ${MUSL_SHA}" >&2
    exit 1
fi

mkdir -p "${work}/src"
tar -xzf "${work}/musl.tar.gz" -C "${work}/src" --strip-components=1

for p in ${PATCHES}; do
    echo "bundle_musl: applying ${p}"
    patch -d "${work}/src" -p1 < "${PATCH_DIR}/${p}.patch"
done

mkdir -p "${dest}"
(
    cd "${work}/src"
    CC="${CC:-clang}" ./configure --prefix="${dest}" --disable-shared
    make -j"$(nproc 2>/dev/null || echo 4)"
    make install
)

if [ ! -f "${dest}/lib/libc.a" ] || [ ! -d "${dest}/include" ]; then
    echo "bundle_musl: ${dest} is not a musl sysroot (need lib/libc.a and include/)" >&2
    exit 1
fi

CC="${CC:-clang}"
builtins=$("${CC}" -rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null || true)
if [ -n "${builtins}" ] && [ -f "${builtins}" ]; then
    cp "${builtins}" "${dest}/lib/libclang_rt.builtins.a"
else
    echo "bundle_musl: ${CC} -rtlib=compiler-rt -print-libgcc-file-name did not name a file" >&2
    exit 1
fi

printf '%s+%s\n%s\n' "${MUSL_VER}" "$(echo ${PATCHES} | tr ' ' '+')" "$("${CC}" --version | sed -n 1p)" > "${dest}/echo-musl-version"

echo "bundle_musl: ${dest}"
