#!/usr/bin/env bash
set -euo pipefail

packages=(build-essential cmake ninja-build pkg-config qt6-base-dev qmake6 qt6-wayland
          libopencv-dev curl file dpkg-dev fakeroot desktop-file-utils appstream python3)
case "${1:-}" in
    --help)
        echo "Usage: packaging/install-build-deps-ubuntu.sh [--print]"
        echo "Install Ubuntu build tools for YAGF, AppImage and .deb packaging."
        echo "LaTeX and other optional runtime features are listed in packaging/README.md."
        exit 0 ;;
    --print)
        printf 'apt-get install'
        printf ' %s' "${packages[@]}"
        printf '\n'
        exit 0 ;;
    '') ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
esac
if (( EUID == 0 )); then
    apt-get update
    apt-get install --yes "${packages[@]}"
else
    sudo apt-get update
    sudo apt-get install --yes "${packages[@]}"
fi
