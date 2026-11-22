#!/usr/bin/env bash
set -euo pipefail

usage() { printf 'Usage: %s [--cli-only]\n' "$0"; }
with_tools=true
case "${1:-}" in
    --help|-h) usage; exit 0 ;;
    --cli-only) with_tools=false ;;
    '') ;;
    *) usage >&2; exit 2 ;;
esac
if (( $# > 1 )); then usage >&2; exit 2; fi

admin() {
    if (( EUID == 0 )); then "$@"
    elif command -v sudo >/dev/null && [[ -t 0 ]]; then sudo "$@"
    elif command -v pkexec >/dev/null; then pkexec "$@"
    else sudo "$@"
    fi
}

source /etc/os-release
case " $ID ${ID_LIKE:-} " in
    *' arch '*)
        deps=(gcc cmake make cli11 nlohmann-json pugixml libarchive shared-mime-info desktop-file-utils xdg-utils)
        if ! pacman -T "${deps[@]}" >/dev/null; then
            admin /usr/bin/pacman -S --needed --noconfirm "${deps[@]}"
        fi ;;
    *' debian '*|*' ubuntu '*)
        deps=(g++ cmake make libcli11-dev nlohmann-json3-dev libpugixml-dev libarchive-dev shared-mime-info desktop-file-utils xdg-utils ca-certificates)
        missing=()
        for pkg in "${deps[@]}"; do
            if [[ $(dpkg-query -W -f='${db:Status-Status}' "$pkg" 2>/dev/null || :) != installed ]]; then missing+=("$pkg"); fi
        done
        if (( ${#missing[@]} )); then
            admin /usr/bin/apt-get update
            admin /usr/bin/apt-get install -y --no-install-recommends "${missing[@]}"
        fi ;;
    *' fedora '*|*' rhel '*)
        deps=(gcc-c++ cmake make cli11-devel json-devel pugixml-devel libarchive-devel shared-mime-info desktop-file-utils xdg-utils)
        if ! rpm -q "${deps[@]}" >/dev/null; then admin /usr/bin/dnf install -y "${deps[@]}"; fi ;;
esac

repo=$(cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
prefix="${HOME}/.local"
cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
cmake --install "$build"
if "$with_tools"; then "$prefix/bin/bedrock" tools install; fi
printf 'Installed %s/bin/bedrock\n' "$prefix"
case ":${PATH}:" in
    *":$prefix/bin:"*) ;;
    *) printf 'Add %s/bin to your PATH to use bedrock from any directory.\n' "$prefix" ;;
esac
