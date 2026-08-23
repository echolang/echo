#!/usr/bin/env bash
#
# build a minimal static libcurl into <prefix>, for the released epm.
#
#   tools/build_static_curl.sh /tmp/static-curl
#
# HTTP/1.1 + HTTPS only. Unix sits on a static OpenSSL we build here;
# Darwin also enables Apple SecTrust so CAs come from the keychain.
# Windows uses CMake + Schannel and writes libcurl.lib. curl 8.21 dropped
# --with-secure-transport. Homebrew must not win: PATH is /usr/bin:/bin
# plus the prefix, PKG_CONFIG_LIBDIR is the prefix.

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <prefix>" >&2
    exit 2
fi

prefix="$1"
mkdir -p "${prefix}"
prefix="$(cd "${prefix}" && pwd)"

if [ -f "${prefix}/lib/libcurl.a" ] || [ -f "${prefix}/lib/libcurl.lib" ]; then
    echo "build_static_curl: already have a static libcurl under ${prefix}/lib"
    exit 0
fi

CURL_VER="8.21.0"
CURL_SHA="aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6"
CURL_URL="https://curl.se/download/curl-${CURL_VER}.tar.xz"

OPENSSL_VER="3.5.7"
OPENSSL_SHA="a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
src="$(mktemp -d)"
trap 'rm -rf "${src}"' EXIT

file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    else
        shasum -a 256 "$1" | awk '{ print $1 }'
    fi
}

fetch() {
    local url="$1"
    local dest="$2"
    local expect="$3"

    curl -fsSL "${url}" -o "${dest}"
    local got
    got="$(file_sha256 "${dest}")"

    if [ "${got}" != "${expect}" ]; then
        echo "build_static_curl: hash mismatch for ${dest}" >&2
        echo "  expected ${expect}" >&2
        echo "  got      ${got}" >&2
        exit 1
    fi
}

os="$(uname -s)"
case "${os}" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        ;;
    *)
        # replace the search path, do not prepend to Homebrew's
        export PKG_CONFIG_LIBDIR="${prefix}/lib/pkgconfig"
        unset PKG_CONFIG_PATH
        export PATH="${prefix}/bin:/usr/bin:/bin"
        ;;
esac

openssl_target=""
curl_tls=(--with-openssl="${prefix}")

case "${os}" in
    Linux)
        case "$(uname -m)" in
            x86_64) openssl_target=linux-x86_64 ;;
            aarch64|arm64) openssl_target=linux-aarch64 ;;
            *)
                echo "build_static_curl: no OpenSSL target for Linux $(uname -m)" >&2
                exit 2
                ;;
        esac
        curl_tls+=(--with-ca-path=/etc/ssl/certs
            --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt)
        ;;

    Darwin)
        openssl_target=darwin64-arm64-cc
        curl_tls+=(--with-apple-sectrust)
        ;;

    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        # Schannel is the OS TLS stack, so there is no OpenSSL to build and no
        # Autotools. CMake writes libcurl.lib into <prefix>/lib.
        fetch "${CURL_URL}" "${src}/curl.tar.xz" "${CURL_SHA}"
        mkdir -p "${src}/curl"
        tar -xJf "${src}/curl.tar.xz" -C "${src}/curl" --strip-components=1
        # MultiThreaded is libcmt, the CRT echoc's Windows link line always
        # names. CMake defaults to /MD; that archive then pulls ucrt.lib in
        # next to libucrt and lld-link duplicate-symbols malloc
        cmake -S "${src}/curl" -B "${src}/curl-build" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="${prefix}" \
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
            -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
            -DBUILD_SHARED_LIBS=OFF \
            -DBUILD_CURL_EXE=OFF \
            -DCURL_USE_SCHANNEL=ON \
            -DCURL_USE_OPENSSL=OFF \
            -DCURL_USE_LIBPSL=OFF \
            -DCURL_BROTLI=OFF \
            -DCURL_ZSTD=OFF \
            -DCURL_USE_LIBSSH2=OFF \
            -DUSE_NGHTTP2=OFF \
            -DCURL_DISABLE_LDAP=ON \
            -DCURL_DISABLE_RTSP=ON \
            -DENABLE_UNICODE=ON
        cmake --build "${src}/curl-build" --config Release
        cmake --install "${src}/curl-build" --config Release

        if [ ! -f "${prefix}/lib/libcurl.lib" ] && [ ! -f "${prefix}/lib/libcurl.a" ]; then
            echo "build_static_curl: CMake did not write libcurl.lib / libcurl.a into ${prefix}/lib" >&2
            ls -la "${prefix}/lib" 2>/dev/null || true
            exit 1
        fi

        exit 0
        ;;

    *)
        echo "build_static_curl: no rule for ${os}" >&2
        exit 2
        ;;
esac

fetch "${OPENSSL_URL}" "${src}/openssl.tar.gz" "${OPENSSL_SHA}"
mkdir -p "${src}/openssl"
tar -xzf "${src}/openssl.tar.gz" -C "${src}/openssl" --strip-components=1
(
    cd "${src}/openssl"
    # --libdir=lib is load-bearing on x86_64 Linux: Configure defaults to lib64 there,
    # and --link search:<prefix>/lib would miss the archives and pick up the distro
    # OpenSSL 3.0 instead. SSL_get0_group_name is 3.2+; that mismatch is a link error.
    ./Configure --prefix="${prefix}" --libdir=lib no-shared no-tests "${openssl_target}"
    make -j"${jobs}"
    make install_sw
)

if [ ! -f "${prefix}/lib/libssl.a" ] || [ ! -f "${prefix}/lib/libcrypto.a" ]; then
    echo "build_static_curl: OpenSSL did not install libssl.a / libcrypto.a into ${prefix}/lib" >&2
    ls -la "${prefix}/lib" "${prefix}/lib64" 2>/dev/null || true
    exit 1
fi

fetch "${CURL_URL}" "${src}/curl.tar.xz" "${CURL_SHA}"
mkdir -p "${src}/curl"
tar -xJf "${src}/curl.tar.xz" -C "${src}/curl" --strip-components=1
(
    cd "${src}/curl"
    ./configure --prefix="${prefix}" \
        --disable-shared --enable-static --disable-ldap --disable-rtsp \
        --disable-manual --disable-docs --without-libidn2 --without-nghttp2 \
        --without-brotli --without-zstd --without-libssh2 --without-libpsl \
        "${curl_tls[@]}"
    make -j"${jobs}"
    make install
)

if [ ! -f "${prefix}/lib/libcurl.a" ]; then
    echo "build_static_curl: configure/make did not write ${prefix}/lib/libcurl.a" >&2
    exit 1
fi
