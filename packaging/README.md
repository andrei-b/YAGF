# Linux packaging

YAGF builds native Ubuntu/Debian `.deb` packages and portable AppImages. Both
contain the GUI, benchmark CLI, README with screenshots, detailed guides and
optional inference-service scripts/templates. The scripts do not install the
result or enable services automatically.

## Ubuntu packages

Build on the target Ubuntu release, using a VM or container for another release.
The architecture is detected natively (`amd64`, `arm64`, etc.); the script does
not cross-compile.

```sh
./packaging/install-build-deps-ubuntu.sh --print  # Inspect prerequisites
./packaging/install-build-deps-ubuntu.sh
./packaging/build-deb.sh
sudo apt install ./dist/yagf2_2.2.0-1_amd64.deb
```

`build-deb.sh` performs a Release build, uses CPack and `dpkg-shlibdeps` to derive
shared-library dependencies, and creates `yagf2_VERSION-1_ARCH.deb` with a SHA256
checksum in `dist/`. Tesseract and Poppler are required dependencies; recommended
packages provide English OCR, Wayland and spelling support. LaTeX, additional
languages, scanner/DjVu tools and OCRmyPDF are optional suggested host packages.
The maintainer defaults to the developer in `AUTHORS`; override it with
`-DYAGF_PACKAGE_MAINTAINER="Name <email@example.org>"` when building a derivative.

The default build directory is `build-deb`. Set `YAGF_DEB_BUILD_DIR`,
`YAGF_DIST_DIR` and `CMAKE_BUILD_PARALLEL_LEVEL` to override it, the output
directory and the default four build jobs. Additional CMake arguments are
forwarded, for example:

```sh
./packaging/build-deb.sh -DOpenCV_DIR=/path/to/lib/cmake/opencv4
```

The .deb uses the target system's libraries and has no build-host RPATH. Build
against distribution-provided dependencies so generated package requirements
can be resolved by APT. `dpkg-deb --info` and `dpkg-deb --contents` inspect the
result without installing it.

## AppImages

The AppImage contains YAGF, the `yagf2-benchmark` CLI, Qt 6 (including X11,
Wayland and offscreen platform plugins), and the linked OpenCV libraries.
The output is `YAGF-VERSION-ARCH.AppImage` plus a SHA256 checksum. VERSION comes
from CMake; ARCH is the build host's `x86_64` or `aarch64` architecture.

## Runtime dependencies on the host

LaTeX and document-processing executables remain external host dependencies.
Installing the AppImage does not download OCR models, enable services, or change
selected recognition engines. Programs installed in `~/.local/bin` work when that
directory is on the desktop session's PATH.

Public documentation and profiles use reserved example hostnames, loopback
defaults, and replaceable storage paths. Keep real service URLs, SSH destinations,
tokens and machine-specific overrides in local configuration files. Both builders
audit the source and final package contents before publishing an artifact.
The audit can also be run with `python3 packaging/check-public-files.py` or with
`--root PATH` for an extracted package. It reports rule names rather than matched
private values. Build checkout paths are mapped to a generic source prefix.

Enable the commit audit once per Git clone with
`./packaging/install-git-hooks.sh`. It sets the repository-local hooks path to
`.githooks`, preserves an existing different hook setup, and audits the staged
snapshot before commits, merge commits and `git am`. Manual check:
`python3 packaging/check-public-files.py --staged`. Staged content is checked
even when a different version of the file exists in the working tree.

| Feature | Ubuntu/Debian packages |
| --- | --- |
| Local text OCR | `tesseract-ocr`, plus language data such as `tesseract-ocr-eng` or `tesseract-ocr-rus` |
| PDF import, previews and searchable export | `poppler-utils` |
| Live formula previews and typeset PDF formulas | `texlive-latex-base`, `texlive-fonts-recommended` |
| Cyrillic text and formulas | `texlive-lang-cyrillic`, `cm-super-minimal` |
| Compile exported LaTeX documents, including tables | Also `texlive-latex-recommended`, `texlive-latex-extra` |
| Spelling suggestions | `libenchant-2-2` and dictionaries such as `hunspell-en-us`, `hunspell-ru` |
| Scanner import | `sane-utils`; network AirScan discovery also uses `sane-airscan` |
| DjVu import and conversion | `djvulibre-bin` |
| Archival PDF/A export | `ocrmypdf` |

For local OCR and PDF processing on Ubuntu:

```sh
sudo apt install tesseract-ocr tesseract-ocr-eng poppler-utils
```

To enable formula previews and compile Latin/Cyrillic LaTeX exports:

```sh
sudo apt install texlive-latex-base texlive-latex-recommended texlive-latex-extra \
  texlive-fonts-recommended texlive-lang-cyrillic cm-super-minimal
```

Local Rapid LaTeX OCR uses its own optional Python environment. Remote text,
formula and layout models run on their configured inference hosts. Their Python
workers, dependency lists, documentation and systemd templates are installed in
`usr/share/yagf2/service` inside the AppImage; they are not started automatically.
The benchmark metrics collector is in `usr/share/yagf2/tools/jetson_metrics.py`.

### Building an AppImage

Install the C++23 compiler, CMake, Ninja, Qt 6 development packages, `qmake6`,
OpenCV development packages, `curl`, and Qt 6 Wayland plugins. On Ubuntu the
prerequisite installer above provides these plus metadata validators.

```sh
./packaging/build-appimage.sh
# A nonstandard OpenCV installation:
./packaging/build-appimage.sh -DOpenCV_DIR=/path/to/lib/cmake/opencv4
```

The script uses a Release build with four parallel jobs by default. It validates
desktop/AppStream metadata when the validators are available, stages a fresh
AppDir using CMake's install rules, deploys both executables' shared libraries,
and creates the image and checksum in `dist/`. A build-info file records the
version, architecture and source revision. Existing unrelated images are left
in place.

Use `CMAKE_BUILD_PARALLEL_LEVEL`, `YAGF_APPIMAGE_BUILD_DIR` or `YAGF_DIST_DIR` to
override jobs and output locations. Use `LINUXDEPLOY`, `LINUXDEPLOY_QT_PLUGIN` and
`QMAKE` for existing packaging tools. Run `--help` for the complete interface.
The official linuxdeploy and Qt plugin AppImages are cached on first use.

The AppImage targets the build distribution's ABI. Builds on Ubuntu 24.04 need
testing before use on older distributions. Qt is bundled; external command-line
tools still use the host installation.

## Checking the results

```sh
cd dist
sha256sum --check YAGF-VERSION-ARCH.AppImage.sha256
QT_QPA_PLATFORM=offscreen ./YAGF-VERSION-ARCH.AppImage --appimage-extract-and-run --version
sha256sum --check yagf2_VERSION-1_ARCH.deb.sha256
dpkg-deb --info yagf2_VERSION-1_ARCH.deb
```

Extract with `--appimage-extract` to inspect the installed files and run
`squashfs-root/usr/bin/yagf2-benchmark --help`. Check GUI startup, actual OCR/PDF
processing, and formula previews on a target host as well as metadata validation.
