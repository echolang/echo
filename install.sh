#!/usr/bin/env bash
#
# installs the latest released echoc into /usr/local/bin:
#
#   curl -fsSL https://raw.githubusercontent.com/echolang/echo/master/install.sh | bash
#
# a released binary carries the standard library inside it, so this is one file and nothing else.
# set ECHO_INSTALL_DIR to install somewhere other than /usr/local/bin.

set -euo pipefail

REPO="echolang/echo"
INSTALL_DIR="${ECHO_INSTALL_DIR:-/usr/local/bin}"

# spelled out rather than derived: uname says Darwin where the release asset says macos, and these
# two are the only platforms the release workflow builds
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  asset="echo-macos-arm64" ;;
    Linux-x86_64)  asset="echo-linux-x86_64" ;;
    *)
        echo "echo: no prebuilt echoc for $(uname -s) $(uname -m)." >&2
        echo "build it from source instead: https://github.com/${REPO}" >&2
        exit 1
        ;;
esac

url="https://github.com/${REPO}/releases/latest/download/${asset}.tar.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "downloading ${asset} ..."
if ! curl -fsSL "${url}" | tar -xzf - -C "${tmp}"; then
    echo "echo: could not download ${url}" >&2
    echo "see https://github.com/${REPO}/releases for what is available." >&2
    exit 1
fi

# a renamed asset or an error page that still unpacked is a message here rather than half an install
if [ ! -f "${tmp}/echoc" ]; then
    echo "echo: the downloaded archive does not contain echoc." >&2
    exit 1
fi

# sudo only when it is needed, and it reads its password from the tty - so this still works under
# `curl | bash`, where stdin is the script
if [ -w "${INSTALL_DIR}" ]; then
    install -m 755 "${tmp}/echoc" "${INSTALL_DIR}/echoc"
else
    echo "${INSTALL_DIR} is not writable, installing with sudo ..."
    sudo mkdir -p "${INSTALL_DIR}"
    sudo install -m 755 "${tmp}/echoc" "${INSTALL_DIR}/echoc"
fi

echo "installed echoc $("${INSTALL_DIR}/echoc" --version) to ${INSTALL_DIR}/echoc"
