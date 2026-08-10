#!/usr/bin/env bash
#
# what a released binary is allowed to depend on.
#
#   tools/check_release_deps.sh build-release/echoc
#
# a shared library the machine that downloaded the binary does not have is not a missing feature: the
# loader refuses to start the process, so the release is not a slower compiler there, it is no
# compiler. The rule differs by platform because the failure does - an absolute /opt/homebrew path is
# fatal anywhere else, while a plain soname is resolved by ldconfig on every machine that has the
# distro package - so this is one allowlist per loader rather than one list of library names.
#
# runnable by hand against an unpacked release, which is what makes it a check rather than a CI step.

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <binary>" >&2
    exit 2
fi

binary="$1"

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
        allowed='^(libc|libm|libdl|libpthread|librt|libz|libstdc\+\+|libgcc_s|ld-linux-x86-64)\.so'
        deps="$(readelf -d "${binary}" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p')"
        escaped="$(printf '%s\n' "${deps}" | grep -v -E "${allowed}" || true)"

        # a run path is the same bug in ELF clothing: a directory on the build machine, baked in
        runpath="$(readelf -d "${binary}" | sed -n 's/.*R\(UN\)\?PATH.*\[\(.*\)\].*/\2/p' || true)"

        if [ -n "${runpath}" ]; then
            escaped="${escaped}"$'\n'"RUNPATH ${runpath}"
        fi
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
