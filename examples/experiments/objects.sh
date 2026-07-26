#!/usr/bin/env bash
set -euo pipefail

minigit_input=${1:-./build/debug/minigit}
if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [minigit-binary]\n' "$0" >&2
    exit 2
fi
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
if [[ ! -x "$minigit_binary" ]]; then
    printf 'MiniGit executable not found: %s\n' "$minigit_binary" >&2
    exit 1
fi

experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-objects.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

cd -- "$experiment_directory"
printf 'saved content\n' > "$experiment_directory/expected.txt"
outside_id=$("$minigit_binary" hash-object "$experiment_directory/expected.txt")
[[ "$outside_id" =~ ^[0-9a-f]{64}$ ]]
"$minigit_binary" init "$experiment_directory/repo"
cd -- "$experiment_directory/repo"
cp -- "$experiment_directory/expected.txt" sample.txt
saved_id=$("$minigit_binary" hash-object -w sample.txt)
[[ "$saved_id" == "$outside_id" ]]
[[ $("$minigit_binary" hash-object -w sample.txt) == "$saved_id" ]]
[[ $("$minigit_binary" cat-file -t "$saved_id") == blob ]]

printf 'working content\n' > sample.txt
working_id=$("$minigit_binary" hash-object sample.txt)
[[ "$working_id" != "$saved_id" ]]
"$minigit_binary" cat-file -p "$saved_id" > "$experiment_directory/actual.txt"
cmp -- "$experiment_directory/expected.txt" "$experiment_directory/actual.txt"

"$minigit_binary" init .
"$minigit_binary" cat-file -p "$saved_id" > "$experiment_directory/after-init.txt"
cmp -- "$experiment_directory/expected.txt" "$experiment_directory/after-init.txt"
printf 'Object experiment passed: saved blob survives worktree edits and repeated init.\n'
