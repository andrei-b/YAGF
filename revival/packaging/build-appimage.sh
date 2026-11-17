#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${YAGF_APPIMAGE_BUILD_DIR:-${project_dir}/build-appimage}"
dist_dir="${YAGF_DIST_DIR:-${project_dir}/dist}"
arch="$(uname -m)"
if [[ "${arch}" != "x86_64" && "${arch}" != "aarch64" ]]; then
    echo "Unsupported AppImage architecture: ${arch}" >&2
    exit 1
fi
mkdir -p "${build_dir}" "${dist_dir}"
build_dir="$(cd "${build_dir}" && pwd)"
dist_dir="$(cd "${dist_dir}" && pwd)"
tools_dir="${build_dir}/tools"
mkdir -p "${tools_dir}"

fetch_tool() {
    local name="$1" url="$2"
    local path="${tools_dir}/${name}"
    if [[ ! -x "${path}" ]]; then
        curl --fail --location --retry 3 --silent --show-error --output "${path}.download" "${url}"
        mv "${path}.download" "${path}"
        chmod +x "${path}"
    fi
    printf '%s\n' "${path}"
}

linuxdeploy="${LINUXDEPLOY:-}"
if [[ -z "${linuxdeploy}" ]]; then
    linuxdeploy="$(fetch_tool "linuxdeploy-${arch}.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage")"
fi
if [[ -n "${LINUXDEPLOY_QT_PLUGIN:-}" ]]; then
    ln -sfn "$(readlink -f "${LINUXDEPLOY_QT_PLUGIN}")" \
        "${tools_dir}/linuxdeploy-plugin-qt-${arch}.AppImage"
else
    fetch_tool "linuxdeploy-plugin-qt-${arch}.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${arch}.AppImage" >/dev/null
fi

cmake -S "${project_dir}/revival" -B "${build_dir}/cmake" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build "${build_dir}/cmake" --parallel
appdir="${build_dir}/YAGF.AppDir"
rm -rf "${appdir}"
DESTDIR="${appdir}" cmake --install "${build_dir}/cmake"

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6)}"
export LINUXDEPLOY_OUTPUT_VERSION="2.0.0"
export ARCH="${arch}"
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so"
export PATH="${tools_dir}:${PATH}"
cd "${build_dir}"
"${linuxdeploy}" --appdir "${appdir}" --plugin qt --output appimage
shopt -s nullglob
bundles=(./*.AppImage)
if (( ${#bundles[@]} == 0 )); then
    echo "linuxdeploy did not create an AppImage" >&2
    exit 1
fi
for bundle in "${bundles[@]}"; do
    mv "${bundle}" "${dist_dir}/"
    echo "Created ${dist_dir}/$(basename "${bundle}")"
done
