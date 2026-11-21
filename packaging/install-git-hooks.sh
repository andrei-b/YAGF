#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
current="$(git -C "${project_dir}" config --get core.hooksPath || true)"
if [[ -n "${current}" && "${current}" != ".githooks" && "${current}" != "${project_dir}/.githooks" ]]; then
    echo "Existing core.hooksPath is preserved. Add 'python3 packaging/check-public-files.py --staged' to its pre-commit hook, or select .githooks explicitly." >&2
    exit 1
fi
legacy_hook="$(git -C "${project_dir}" rev-parse --git-path hooks/pre-commit)"
if [[ "${legacy_hook}" != /* ]]; then legacy_hook="${project_dir}/${legacy_hook}"; fi
if [[ -z "${current}" && -x "${legacy_hook}" ]]; then
    echo "An existing pre-commit hook is preserved. Integrate the staged audit into it before switching hooks." >&2
    exit 1
fi
git -C "${project_dir}" config --local core.hooksPath .githooks
echo "Public-file audit enabled for commits, merge commits and git am in this checkout."
