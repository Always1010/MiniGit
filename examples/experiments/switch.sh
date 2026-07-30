#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-switch.XXXXXX")
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
"$minigit_binary" switch -c topic
printf 'B\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m B
"$minigit_binary" switch main
[[ $(< note) == A && $("$minigit_binary" show :note) == A ]]
printf 'staged\n' > note
"$minigit_binary" add note
printf 'working\n' > note
if "$minigit_binary" switch topic > "$experiment_directory/output" 2> "$experiment_directory/error"; then
    printf 'Dirty overlapping switch unexpectedly succeeded.\n' >&2
    exit 1
fi
[[ $(< note) == working && $("$minigit_binary" show :note) == staged ]]
"$minigit_binary" switch -c carried
[[ $(< note) == working && $("$minigit_binary" show :note) == staged ]]
printf 'A\n' > note
"$minigit_binary" add note
"$minigit_binary" switch topic
[[ $(< note) == B ]]
"$minigit_binary" switch --detach "$first_commit"
[[ $(< .minigit/HEAD) == "$first_commit" && $(< note) == A ]]
"$minigit_binary" switch topic
"$minigit_binary" reflog
printf 'Switch experiment passed: snapshots, local changes, detached HEAD and reflog.\n'
