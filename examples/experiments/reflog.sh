#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-reflog.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
printf 'A\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m A
first_commit=$(< .minigit/refs/heads/main)
printf 'B\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m B
second_commit=$(< .minigit/refs/heads/main)
printf 'HEAD@{0} %s -> %s commit\nHEAD@{1} - -> %s commit\n' "$first_commit" "$second_commit" "$first_commit" > "$experiment_directory/expected"
"$minigit_binary" reflog > "$experiment_directory/observed"
cmp -- "$experiment_directory/expected" "$experiment_directory/observed"
cmp -- .minigit/logs/HEAD .minigit/logs/refs/heads/main
cat -- "$experiment_directory/observed"
printf 'Reflog experiment passed: reference events are distinct from parent history.\n'
