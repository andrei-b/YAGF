#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == "--help" ]]; then
    cat <<'EOF'
Usage: packaging/build-deb.sh [CMake configure arguments]

Build an Ubuntu/Debian package for this host's architecture and shared libraries.
Run packaging/install-build-deps-ubuntu.sh first if build tools are missing.
Additional arguments, such as -DOpenCV_DIR=/path/to/opencv4, are passed to CMake.

Environment:
  YAGF_DEB_BUILD_DIR       CMake build directory (default: build-deb)
  YAGF_DIST_DIR            Artifact directory (default: dist)
  CMAKE_BUILD_PARALLEL_LEVEL  Parallel build jobs (default: 4)

The .deb includes the GUI, benchmark CLI, documentation and worker templates.
Core libraries, Tesseract and Poppler are dependencies. LaTeX is optional on the host.
EOF
    exit 0
fi
python3 "${project_dir}/packaging/check-public-files.py"
for tool in cmake cpack ninja dpkg dpkg-deb dpkg-shlibdeps; do
    if ! command -v "${tool}" >/dev/null; then
        echo "Missing ${tool}; run packaging/install-build-deps-ubuntu.sh" >&2
        exit 1
    fi
done
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "${project_dir}/packaging/io.github.andrei_b.YAGF.desktop"
fi
if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net "${project_dir}/packaging/io.github.andrei_b.YAGF.appdata.xml"
fi
build_dir="${YAGF_DEB_BUILD_DIR:-${project_dir}/build-deb}"
dist_dir="${YAGF_DIST_DIR:-${project_dir}/dist}"
mkdir -p "${build_dir}" "${dist_dir}"
build_dir="$(cd "${build_dir}" && pwd)"
dist_dir="$(cd "${dist_dir}" && pwd)"

cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_RPATH= -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF \
    -DCMAKE_SKIP_INSTALL_RPATH=ON "$@"
cmake --build "${build_dir}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "${build_dir}/CMakeCache.txt")"
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Cannot determine the CMake project version" >&2
    exit 1
fi
arch="$(dpkg --print-architecture)"
artifact="yagf2_${version}-1_${arch}.deb"
output_dir="$(mktemp -d "${build_dir}/deb-output.XXXXXX")"
trap 'rm -rf -- "${output_dir}"' EXIT
cpack --config "${build_dir}/CPackConfig.cmake" -G DEB -B "${output_dir}"
if [[ ! -s "${output_dir}/${artifact}" ]]; then
    echo "CPack did not create ${artifact}" >&2
    exit 1
fi
dpkg-deb --info "${output_dir}/${artifact}"
dpkg-deb --extract "${output_dir}/${artifact}" "${output_dir}/verify"
python3 "${project_dir}/packaging/check-public-files.py" --root "${output_dir}/verify"
mv "${output_dir}/${artifact}" "${dist_dir}/${artifact}"
cd "${dist_dir}"
sha256sum "${artifact}" > "${artifact}.sha256"
echo "Created ${dist_dir}/${artifact}"
echo "Checksum ${dist_dir}/${artifact}.sha256"
