#!/usr/bin/env bash
# Launch debug_app under gdb with sculptcore pretty-printers + helpers.
#
#   tools/gdb/run-gdb.sh tests/scripts/repro.txt [--break-on-throw]
#
# Extra args after the script path are passed through to gdb.

set -e

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
bin="$repo_root/build/native/source/debug/debug_app"

if [[ ! -x "$bin" ]]; then
  echo "debug_app not built. Run: node make.mjs build native" >&2
  exit 2
fi

script="${1:-}"
shift || true
if [[ -z "$script" ]]; then
  echo "usage: $0 <script.txt> [extra gdb args]" >&2
  exit 2
fi

gdb_args=(
  -q
  -ex "source $repo_root/tools/gdb/sculptcore.py"
  -ex "source $repo_root/tools/gdb/helpers.gdb"
)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --break-on-throw)
      gdb_args+=(-ex "catch throw")
      shift
      ;;
    *)
      gdb_args+=("$1")
      shift
      ;;
  esac
done

gdb_args+=(--args "$bin" --script "$script" --out "$repo_root/build/shots")

exec gdb "${gdb_args[@]}"
