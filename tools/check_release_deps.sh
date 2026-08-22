#!/usr/bin/env bash
#
# what a released binary is allowed to depend on.
#
#   tools/check_release_deps.sh build-release/echoc
#   tools/check_release_deps.sh /tmp/epm curl
#
# a shared library the machine that downloaded the binary does not have is not a missing feature: the
# loader refuses to start the process, so the release is not a slower compiler there, it is no
# compiler. The rule differs by platform because the failure does - an absolute /opt/homebrew path is
# fatal anywhere else, while a plain soname is resolved by ldconfig on every machine that has the
# distro package - so this is one allowlist per loader rather than one list of library names.
#
# extra names after the binary are linker names (`curl` for `#[link: lib "curl"]`). They extend the
# Linux SONAME allowlist only. Darwin still admits `/usr/lib` and `/System/Library` and nothing else,
# so a Homebrew curl is refused whether you named it or not.
#
# runnable by hand against an unpacked release, which is what makes it a check rather than a CI step.

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <binary> [extra-lib...]" >&2
    exit 2
fi

binary="$1"
shift

if [ ! -f "${binary}" ]; then
    echo "check_release_deps: no such file: ${binary}" >&2
    exit 2
fi

case "$(uname -s)" in
    Darwin)
        # every load command must name something the operating system itself ships. That is the rule
        # that catches a Homebrew path, rather than a list of the libraries we happen to have seen
        deps="$(otool -L "${binary}" | tail -n +2 | awk '{ print $1 }')"
        escaped="$(printf '%s\n' "${deps}" | grep -v -E '^/usr/lib/|^/System/Library/' || true)"
        ;;

    Linux)
        # sonames rather than paths here: the loader resolves them through ldconfig, so what matters is
        # that each one belongs to a package every target machine already has. libzstd is deliberately
        # absent - ECO_STATIC_ZSTD puts it inside the binary, and its return is a regression
        allowed='^(libc|libm|libdl|libpthread|librt|libz|libstdc\+\+|libgcc_s|ld-linux-[a-z0-9-]+)\.so'
        for name in "$@"; do
            allowed="${allowed}|lib${name}\\.so"
        done
        deps="$(readelf -d "${binary}" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p')"
        escaped="$(printf '%s\n' "${deps}" | grep -v -E "${allowed}" || true)"

        # a run path is the same bug in ELF clothing: a directory on the build machine, baked in
        runpath="$(readelf -d "${binary}" | sed -n 's/.*R\(UN\)\?PATH.*\[\(.*\)\].*/\2/p' || true)"

        if [ -n "${runpath}" ]; then
            escaped="${escaped}"$'\n'"RUNPATH ${runpath}"
        fi
        ;;

    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        # imports, not paths: the loader looks next to the exe, then System32. A DLL we ship
        # beside the binary is the Windows spelling of "the binary carries it". VCRUNTIME /
        # MSVCP live in System32 on this runner and on almost no clean machine, so they are
        # refused even when present - that is the static CRT not taking.
        if command -v llvm-readobj >/dev/null 2>&1; then
            deps="$(llvm-readobj --coff-imports "${binary}" | sed -n 's/^  Name: //p')"
        else
            echo "check_release_deps: llvm-readobj is required on Windows" >&2
            exit 2
        fi

        bindir="$(cd "$(dirname "${binary}")" && pwd)"
        sys32=""
        if [ -n "${WINDIR:-}" ]; then
            sys32="${WINDIR}/System32"
        elif [ -d /c/Windows/System32 ]; then
            sys32="/c/Windows/System32"
        fi

        extra=""
        for name in "$@"; do
            extra="${extra} ${name}.dll ${name}.DLL"
        done

        escaped=""
        while IFS= read -r dll; do
            [ -z "${dll}" ] && continue
            lower="$(printf '%s' "${dll}" | tr '[:upper:]' '[:lower:]')"
            case "${lower}" in
                vcruntime*.dll|msvcp*.dll|msvcr*.dll|concrt*.dll)
                    escaped="${escaped}"$'\n'"${dll}"
                    continue
                    ;;
            esac

            listed=0
            for name in ${extra}; do
                extra_lower="$(printf '%s' "${name}" | tr '[:upper:]' '[:lower:]')"
                if [ "${lower}" = "${extra_lower}" ]; then
                    listed=1
                    break
                fi
            done
            if [ "${listed}" -eq 1 ]; then
                continue
            fi

            if [ -f "${bindir}/${dll}" ]; then
                continue
            fi

            if [ -n "${sys32}" ] && [ -f "${sys32}/${dll}" ]; then
                continue
            fi

            escaped="${escaped}"$'\n'"${dll}"
        done <<EOF
${deps}
EOF
        ;;

    *)
        echo "check_release_deps: no rule for $(uname -s), not checking anything" >&2
        exit 2
        ;;
esac

echo "${binary} links:"
printf '%s\n' "${deps}" | sed 's/^/    /'

# the empty string survives the pipeline above as one blank line, so ask about the content
if [ -n "$(printf '%s' "${escaped}" | tr -d '[:space:]')" ]; then
    echo >&2
    echo "check_release_deps: this binary depends on libraries a target machine may not have:" >&2
    printf '%s\n' "${escaped}" | sed '/^$/d; s/^/    /' >&2
    echo >&2
    echo "a release has to carry these - see ECO_STATIC_ZSTD in CMakeLists.txt for how zstd is done." >&2
    exit 1
fi

echo "ok: nothing outside what every target machine has"
