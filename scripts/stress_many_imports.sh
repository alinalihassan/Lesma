#!/usr/bin/env bash
# Optional stress: linear import chain of tiny modules, then `lesma compile` the deepest file.
# (Deep `lesma run` / JIT may fail on module init linking; compile exercises the import graph.)
# Usage from repo root (after build):
#   LESMA=build/Debug/lesma bash scripts/stress_many_imports.sh 50
set -euo pipefail
n="${1:-20}"
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
lesma="${LESMA:-$root/build/Debug/lesma}"
for i in $(seq 1 "$n"); do
  f="$tmp/m${i}.les"
  if [[ "$i" -eq 1 ]]; then
    printf 'exit(0)\n' >"$f"
  else
    prev="m$((i - 1)).les"
    printf 'import "%s"\nexit(0)\n' "$prev" >"$f"
  fi
done
out="$tmp/leaf.o"
"$lesma" compile "$tmp/m${n}.les" -o "$out" --no-warnings
