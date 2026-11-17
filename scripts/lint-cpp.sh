#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build/linux-release}"
if ! command -v clang-tidy >/dev/null 2>&1; then
    printf '%s\n' "clang-tidy is required. Install the LLVM clang-tidy package first." >&2
    exit 127
fi
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    printf 'Compile database not found: %s/compile_commands.json\n' "$build_dir" >&2
    printf '%s\n' "Configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON before running this script." >&2
    exit 2
fi

mapfile -d '' files < <(git ls-files -z -- 'src/*.cpp' 'src/core/*.cpp' 'src/platform/*.cpp' 'tests/*.cpp')
if (("${#files[@]}" == 0)); then
    printf '%s\n' "No tracked C++ implementation files were found." >&2
    exit 1
fi

clang-tidy -p "$build_dir" "${files[@]}"
