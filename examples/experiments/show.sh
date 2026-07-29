#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [minigit-binary]\n' "$0" >&2
    exit 2
fi
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-show.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
"$minigit_binary" init "$experiment_directory/repo"
cd -- "$experiment_directory/repo"
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
printf 'A\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m 'Baseline A'
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
[[ $("$minigit_binary" show HEAD:note) == A ]]
[[ $("$minigit_binary" show :note) == B ]]
[[ $("$minigit_binary" show :0:note) == B ]]
[[ $(< note) == C ]]
printf '\000\377\n' > binary
"$minigit_binary" add binary
binary_id=$("$minigit_binary" hash-object binary)
"$minigit_binary" show "$binary_id" > "$experiment_directory/object.bin"
"$minigit_binary" show :binary > "$experiment_directory/index.bin"
cmp binary "$experiment_directory/object.bin"
cmp binary "$experiment_directory/index.bin"
"$minigit_binary" commit -m 'Save B and binary content'
"$minigit_binary" show > "$experiment_directory/commit.txt"
awk '$0 == "-A" { old=1 } $0 == "+B" { new=1 } END { exit !(old && new) }' "$experiment_directory/commit.txt"
[[ $("$minigit_binary" show HEAD:note) == B ]]
[[ $(< note) == C ]]
printf 'Verified committed A, staged B, working C and exact binary show output.\n'
