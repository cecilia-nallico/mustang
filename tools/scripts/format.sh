#!/bin/bash

# Format all first-party sources.
#
#   clang-format  every .h/.cc under mustang/   (config: /.clang-format)
#   buildifier    MODULE.bazel, BUILD.bazel, *.bzl of the repo
#
# --check verifies only (no writes) and exits non-zero when anything is
# unformatted; handy for CI.
set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

check=false
if [[ "${1:-}" == "--check" ]]; then
  check=true
elif [[ $# -gt 0 ]]; then
  echo "usage: $0 [--check]" >&2
  exit 2
fi

for tool in clang-format buildifier; do
  if ! command -v "${tool}" >/dev/null; then
    echo "missing tool: ${tool}" >&2
    exit 1
  fi
done

cpp_files=()
while IFS= read -r -d '' f; do cpp_files+=("${f}"); done < <(
  find mustang -type f \( -name '*.h' -o -name '*.cc' \) -print0
)

build_files=(MODULE.bazel)
while IFS= read -r -d '' f; do build_files+=("${f}"); done < <(
  find . -type f \( -name BUILD.bazel -o -name '*.bzl' \) -not -path './bazel-*' -print0
)

if "${check}"; then
  clang-format --dry-run -Werror "${cpp_files[@]}"
  buildifier --lint=off --mode=check "${build_files[@]}"
else
  clang-format -i "${cpp_files[@]}"
  buildifier --lint=off --mode=fix "${build_files[@]}"
fi
