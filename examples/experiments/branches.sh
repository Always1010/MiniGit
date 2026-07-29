#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-branches.XXXXXX")
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
"$minigit_binary" branch topic
[[ $(< .minigit/refs/heads/topic) == "$first_commit" ]]
"$minigit_binary" branch > "$experiment_directory/observed"
printf '* main %s\n  topic %s\n' "$first_commit" "$first_commit" > "$experiment_directory/expected"
cmp -- "$experiment_directory/expected" "$experiment_directory/observed"
printf 'B\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m B
[[ $(< .minigit/refs/heads/topic) == "$first_commit" ]]
[[ $("$minigit_binary" show topic:note) == A ]]
[[ $("$minigit_binary" show HEAD:note) == B ]]
"$minigit_binary" branch -d topic
[[ ! -e .minigit/refs/heads/topic && ! -e .minigit/logs/refs/heads/topic ]]
"$minigit_binary" branch topic/sub
"$minigit_binary" branch -d topic/sub
"$minigit_binary" branch topic
"$minigit_binary" branch
printf 'Branch experiment passed: lightweight pointers, ancestry deletion and namespace reuse.\n'
