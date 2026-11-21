#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == "--help" ]]; then
    cat <<'EOF'
Usage: packaging/build-appimage.sh [CMake configure arguments]

Build a Release AppImage and SHA256 checksum for the current CMake project version.
Extra arguments, such as -DOpenCV_DIR=/path/to/opencv4, are passed to CMake.

Environment:
  YAGF_APPIMAGE_BUILD_DIR  Build/staging directory (default: build-appimage)
  YAGF_DIST_DIR            Artifact directory (default: dist)
  CMAKE_BUILD_PARALLEL_LEVEL  Parallel build jobs (default: 4)
  LINUXDEPLOY             Existing linuxdeploy executable/AppImage
  LINUXDEPLOY_QT_PLUGIN   Existing linuxdeploy Qt plugin executable/AppImage
  QMAKE                  Qt 6 qmake executable (default: qmake6)

Tesseract, Poppler, LaTeX, scanners and inference models remain host dependencies.
See packaging/README.md for the required and optional runtime packages.
EOF
    exit 0
fi
python3 "${project_dir}/packaging/check-public-files.py"
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
qt_plugin="${LINUXDEPLOY_QT_PLUGIN:-}"
if [[ -z "${qt_plugin}" ]]; then
    qt_plugin="$(fetch_tool "linuxdeploy-plugin-qt-${arch}.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${arch}.AppImage")"
fi
qt_plugin="$(readlink -f "${qt_plugin}")"
if [[ "${qt_plugin}" != "${tools_dir}/linuxdeploy-plugin-qt" ]]; then
    ln -sfn "${qt_plugin}" "${tools_dir}/linuxdeploy-plugin-qt"
fi

if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "${project_dir}/packaging/io.github.andrei_b.YAGF.desktop"
fi
if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net "${project_dir}/packaging/io.github.andrei_b.YAGF.appdata.xml"
fi

cmake -S "${project_dir}" -B "${build_dir}/qt6-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON -DCMAKE_SKIP_INSTALL_RPATH=OFF "$@"
version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "${build_dir}/qt6-build/CMakeCache.txt")"
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Cannot determine the CMake project version" >&2
    exit 1
fi
cmake --build "${build_dir}/qt6-build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
appdir="${build_dir}/YAGF.AppDir"
rm -rf "${appdir}"
DESTDIR="${appdir}" cmake --install "${build_dir}/qt6-build"
revision="$(git -C "${project_dir}" describe --always --dirty 2>/dev/null || printf 'unknown')"
printf 'YAGF %s\nArchitecture: %s\nSource revision: %s\n' "${version}" "${arch}" "${revision}" \
    > "${appdir}/usr/share/doc/yagf2/build-info.txt"

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6)}"
export LINUXDEPLOY_OUTPUT_VERSION="${version}"
artifact="YAGF-${version}-${arch}.AppImage"
output_dir="$(mktemp -d "${build_dir}/appimage-output.XXXXXX")"
trap 'rm -rf -- "${output_dir}"' EXIT
export LDAI_OUTPUT="${output_dir}/${artifact}"
export ARCH="${arch}"
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so"
export PATH="${tools_dir}:${PATH}"
cd "${build_dir}"
"${linuxdeploy}" --appdir "${appdir}" \
    --executable "${appdir}/usr/bin/yagf2" \
    --executable "${appdir}/usr/bin/yagf2-benchmark" \
    --desktop-file "${appdir}/usr/share/applications/io.github.andrei_b.YAGF.desktop" \
    --plugin qt --output appimage
python3 "${project_dir}/packaging/check-public-files.py" --root "${appdir}"
if [[ ! -s "${LDAI_OUTPUT}" ]]; then
    echo "linuxdeploy did not create ${artifact}" >&2
    exit 1
fi
mv "${LDAI_OUTPUT}" "${dist_dir}/${artifact}"
cd "${dist_dir}"
sha256sum "${artifact}" > "${artifact}.sha256"
echo "Created ${dist_dir}/${artifact}"
echo "Checksum ${dist_dir}/${artifact}.sha256"
