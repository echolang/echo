#!/usr/bin/env bash
#
# installs the latest released echoc and epm into /usr/local/bin:
#
#   curl -fsSL https://raw.githubusercontent.com/echolang/echo/master/install.sh | bash
#
# a released echoc carries the standard library inside it. epm is the package
# manager and sits next to it in the same archive.
# set ECHO_INSTALL_DIR to install somewhere other than /usr/local/bin.
# set ECHO_REQUIRE_ATTESTATION=1 to refuse an archive `gh attestation verify` cannot prove.

set -euo pipefail

REPO="echolang/echo"
INSTALL_DIR="${ECHO_INSTALL_DIR:-/usr/local/bin}"

# spelled out rather than derived: uname says Darwin where the release asset says macos, and these
# four are the platforms the release workflow builds (windows is install.ps1)
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  asset="echo-macos-arm64" ;;
    Linux-x86_64)  asset="echo-linux-x86_64" ;;
    Linux-aarch64) asset="echo-linux-arm64" ;;
    *)
        echo "echo: no prebuilt Echo for $(uname -s) $(uname -m)." >&2
        echo "build it from source instead: https://github.com/${REPO}" >&2
        exit 1
        ;;
esac

# resolve the tag once: two requests against /latest can see two releases
latest_url="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/${REPO}/releases/latest")" || {
    echo "echo: could not resolve the latest release at https://github.com/${REPO}/releases/latest" >&2
    exit 1
}
tag="${latest_url##*/}"
base="https://github.com/${REPO}/releases/download/${tag}"
url="${base}/${asset}.tar.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

# to a file first, and checked before anything unpacks it: an archive SHA256SUMS does not vouch for is never extracted
echo "downloading ${asset} ..."
if ! curl -fsSL --proto '=https' --tlsv1.2 -o "${tmp}/${asset}.tar.gz" "${url}"; then
    echo "echo: could not download ${url}" >&2
    echo "see https://github.com/${REPO}/releases for what is available." >&2
    exit 1
fi

if ! curl -fsSL --proto '=https' --tlsv1.2 -o "${tmp}/SHA256SUMS" "${base}/SHA256SUMS"; then
    echo "echo: could not download ${base}/SHA256SUMS, so ${asset} cannot be checked." >&2
    exit 1
fi

expected="$(awk -v f="${asset}.tar.gz" '$2 == f || $2 == "*" f { print $1 }' "${tmp}/SHA256SUMS")"

if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "${tmp}/${asset}.tar.gz" | awk '{ print $1 }')"
else
    actual="$(shasum -a 256 "${tmp}/${asset}.tar.gz" | awk '{ print $1 }')"
fi

if [ -z "${expected}" ] || [ "${expected}" != "${actual}" ]; then
    echo "echo: ${asset}.tar.gz does not match the release's SHA256SUMS - nothing was installed." >&2
    exit 1
fi

# SHA256SUMS is the install check. attestation is extra provenance: a warning when
# `gh` is on PATH, a refusal only when ECHO_REQUIRE_ATTESTATION=1
if [ "${ECHO_REQUIRE_ATTESTATION:-}" = "1" ]; then
    if ! command -v gh >/dev/null 2>&1; then
        echo "echo: ECHO_REQUIRE_ATTESTATION=1 needs gh on PATH." >&2
        exit 1
    fi
    if ! gh attestation verify "${tmp}/${asset}.tar.gz" --repo "${REPO}" >/dev/null 2>&1; then
        echo "echo: ${asset}.tar.gz has no valid build attestation from ${REPO} - nothing was installed." >&2
        exit 1
    fi
elif command -v gh >/dev/null 2>&1; then
    if ! gh attestation verify "${tmp}/${asset}.tar.gz" --repo "${REPO}" >/dev/null 2>&1; then
        echo "echo: warning: no valid build attestation from ${REPO}; SHA256SUMS still matched, installing." >&2
    fi
fi

if ! tar -xzf "${tmp}/${asset}.tar.gz" -C "${tmp}"; then
    echo "echo: could not unpack ${asset}.tar.gz" >&2
    exit 1
fi

# a renamed asset or an error page that still unpacked is a message here rather than half an install
if [ ! -f "${tmp}/echoc" ]; then
    echo "echo: the downloaded archive does not contain echoc." >&2
    exit 1
fi

if [ ! -f "${tmp}/epm" ]; then
    echo "echo: the downloaded archive does not contain epm." >&2
    exit 1
fi

# sudo only when it is needed, and it reads its password from the tty - so this still works under
# `curl | bash`, where stdin is the script
if [ -w "${INSTALL_DIR}" ]; then
    install -m 755 "${tmp}/echoc" "${INSTALL_DIR}/echoc"
    install -m 755 "${tmp}/epm" "${INSTALL_DIR}/epm"
else
    echo "${INSTALL_DIR} is not writable, installing with sudo ..."
    sudo mkdir -p "${INSTALL_DIR}"
    sudo install -m 755 "${tmp}/echoc" "${INSTALL_DIR}/echoc"
    sudo install -m 755 "${tmp}/epm" "${INSTALL_DIR}/epm"
fi

echo "installed echoc $("${INSTALL_DIR}/echoc" --version) to ${INSTALL_DIR}/echoc"
echo "installed epm $("${INSTALL_DIR}/epm" --version) to ${INSTALL_DIR}/epm"
