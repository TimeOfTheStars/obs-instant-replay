#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
obs_version="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["dependencies"]["obs-studio"]["version"])' "${project_root}/buildspec.json")"
obs_headers="${project_root}/.deps-syntax/obs-studio-${obs_version}/libobs"
if [[ ! -d "${obs_headers}" ]]; then
    echo 'Run build-aux/syntax-check.sh first to prepare OBS headers.' >&2
    exit 1
fi
test_dir="$(mktemp -d)"
trap 'rm -rf "${test_dir}"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize="${SANITIZERS:-address,undefined}" -g \
    -I "${project_root}/src" -I "${obs_headers}" \
    "${project_root}/src/core/frame-ring.cpp" \
    "${project_root}/build-aux/tests/frame-ring-test.cpp" \
    -o "${test_dir}/frame-ring-test"
"${test_dir}/frame-ring-test"
