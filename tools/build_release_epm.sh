#!/usr/bin/env bash
#
# compile epm with a just-built echoc, after cloning the path-depends it
# bootstraps on.
#
#   tools/build_release_epm.sh build-release/echoc /tmp/epm /tmp/static-curl 0.2.2
#
# epm/module.eco writes ../../echolibs/{libjson,libcurl,libcommand} from epm/, so the
# clones land next to the echo checkout, not inside it. The depends stay
# paths: epm must not need itself to build itself.
#
# the third argument is the prefix tools/build_static_curl.sh wrote. The
# compile passes --define static_curl and --link search:<prefix>/lib so
# the released binary seats libcurl.a rather than the system dylib.
# the fourth is the version echoc was stamped with; it is written into
# epm/src/cli/version.eco before the compile.

set -euo pipefail

if [ $# -ne 4 ]; then
    echo "usage: $0 <echoc> <out-binary> <static-curl-prefix> <version>" >&2
    exit 2
fi

if [ ! -x "$1" ]; then
    echo "build_release_epm: not an executable: $1" >&2
    exit 2
fi

echoc="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

if [ ! -d "$3" ]; then
    echo "build_release_epm: static curl prefix is not a directory: $3" >&2
    exit 2
fi

curl_prefix="$(cd "$3" && pwd)"

if [ ! -f "${curl_prefix}/lib/libcurl.a" ] && [ ! -f "${curl_prefix}/lib/libcurl.lib" ]; then
    echo "build_release_epm: no libcurl.a / libcurl.lib under ${curl_prefix}/lib" >&2
    echo "  run tools/build_static_curl.sh ${curl_prefix} first" >&2
    exit 2
fi

# Unix seats OpenSSL next to libcurl. Windows uses Schannel, so those archives
# are not expected and must not be demanded.
if [ -f "${curl_prefix}/lib/libcurl.a" ]; then
    if [ ! -f "${curl_prefix}/lib/libssl.a" ] || [ ! -f "${curl_prefix}/lib/libcrypto.a" ]; then
        echo "build_release_epm: no libssl.a / libcrypto.a under ${curl_prefix}/lib" >&2
        echo "  a lib64 install is the usual cause - OpenSSL must be configured --libdir=lib" >&2
        exit 2
    fi
fi

version="$4"
if [ -z "${version}" ]; then
    echo "build_release_epm: version is empty" >&2
    exit 2
fi

out="$2"
if [ "${out#/}" = "${out}" ]; then
    out="$(pwd)/${out}"
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
epm="${root}/epm"
echolibs="$(cd "${root}/.." && pwd)/echolibs"

if [ ! -f "${epm}/module.eco" ]; then
    echo "build_release_epm: no epm/module.eco under ${root}" >&2
    exit 2
fi

# echoc (and Windows CPython) are native binaries. Git Bash `pwd` is `/d/a/...`,
# and Windows std::filesystem / pathlib settle that to `D:\d\a\...`. Git Bash
# auto-converts a *bare* unix argv to a native exe; it does not convert one
# glued behind `search:` or handed to pathlib. every path those two see is native.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

clone() {
    local name="$1"
    local dest="${echolibs}/${name}"

    if [ -f "${dest}/module.eco" ]; then
        return 0
    fi

    if [ -e "${dest}" ]; then
        echo "build_release_epm: ${dest} exists but has no module.eco" >&2
        exit 1
    fi

    mkdir -p "${echolibs}"
    git clone --depth 1 "https://github.com/echolang/${name}.git" "${dest}"
}

clone libjson
clone libcurl
clone libcommand

# --define static_curl is a no-op unless libcurl's manifest names it.
# write the gate if this clone predates that line, so the echo release
# does not wait on a second repository.
#
# ssl/crypto are OpenSSL. Unix builds them next to libcurl; Windows curl is
# Schannel and those archives are never written. wrapping them is what keeps
# `lld-link: could not open 'ssl.lib'` off a Windows release - epm's own
# static_curl arm already names the Windows system libraries
python3 - "$(native_path "${echolibs}/libcurl/module.eco")" <<'PY'
import pathlib, sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()

gated_tls = (
    "#[if: os != windows]\n"
    "#[link: lib { name: \"ssl\", linkage: static }]\n"
    "#[link: lib { name: \"crypto\", linkage: static }]\n"
    "#[end]\n"
)
ungated_tls = (
    "#[link: lib { name: \"ssl\", linkage: static }]\n"
    "#[link: lib { name: \"crypto\", linkage: static }]\n"
)
static_block = (
    "#[if: static_curl]\n"
    "#[link: lib { name: \"curl\", linkage: static }]\n"
    + gated_tls
    + "#[else]\n"
    "#[link: lib \"curl\"]\n"
    "#[end]\n"
)

if "static_curl" not in text:
    old = '#[link: lib "curl"]\n'
    if old not in text:
        sys.stderr.write(
            "build_release_epm: libcurl/module.eco has no '#[link: lib \"curl\"]' to wrap\n")
        sys.exit(1)
    text = text.replace(old, static_block, 1)
elif ungated_tls in text and gated_tls not in text:
    text = text.replace(ungated_tls, gated_tls, 1)

path.write_text(text)
PY

mkdir -p "$(dirname "${out}")"

# refuse quotes so a bad version cannot escape the Echo string
case "${version}" in
    *\'*|*\")
        echo "build_release_epm: version '${version}' contains a quote" >&2
        exit 2
        ;;
esac

cat > "${epm}/src/cli/version.eco" <<EOF
namespace epm::cli;

public const VERSION = '${version}';
EOF

# ECHOC so anything the compile shells out to sees this compiler, not PATH
ECHOC="${echoc}" "${echoc}" build -m "$(native_path "${epm}")" --target epm \
    -o "$(native_path "${out}")" \
    --define static_curl \
    --link "search:$(native_path "${curl_prefix}/lib")"

if [ ! -e "${out}" ] && [ -e "${out}.exe" ]; then
    out="${out}.exe"
fi

if [ ! -x "${out}" ] && [ ! -f "${out}" ]; then
    echo "build_release_epm: ${echoc} did not write an executable to ${out}" >&2
    exit 1
fi
