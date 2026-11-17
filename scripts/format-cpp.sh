#!/usr/bin/env bash
set -euo pipefail

mode="${1:---check}"
if ! command -v clang-format >/dev/null 2>&1; then
    printf '%s\n' "clang-format is required. Install the LLVM clang-format package first." >&2
    exit 127
fi

mapfile -d '' files < <(git ls-files -z -- '*.cpp' '*.hpp')
if (("${#files[@]}" == 0)); then
    printf '%s\n' "No tracked C++ files were found." >&2
    exit 1
fi

case "$mode" in
    --write)
        clang-format -i "${files[@]}"
        ;;
    --check)
        clang-format --dry-run --Werror "${files[@]}"
        ;;
    *)
        printf 'Usage: %s [--check|--write]\n' "$0" >&2
        exit 2
        ;;
esac
