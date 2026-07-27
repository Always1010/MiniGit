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
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-diff.XXXXXX")
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
first_commit=$(< .minigit/refs/heads/main)
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
printf 'untracked\n' > other
"$minigit_binary" diff -U0 > "$experiment_directory/working.patch"
"$minigit_binary" diff --cached -U0 > "$experiment_directory/staged.patch"
"$minigit_binary" diff HEAD -U0 > "$experiment_directory/combined.patch"
awk '$0 == "-B" { old=1 } $0 == "+C" { new=1 } END { exit !(old && new) }' "$experiment_directory/working.patch"
awk '$0 == "-A" { old=1 } $0 == "+B" { new=1 } END { exit !(old && new) }' "$experiment_directory/staged.patch"
awk '$0 == "-A" { old=1 } $0 == "+C" { new=1 } END { exit !(old && new) }' "$experiment_directory/combined.patch"
[[ $(< note) == C ]]
[[ $(< .minigit/refs/heads/main) == "$first_commit" ]]
"$minigit_binary" commit -m 'Commit staged B'
second_commit=$(< .minigit/refs/heads/main)
"$minigit_binary" diff "$first_commit" "$second_commit" -U0 > "$experiment_directory/history.patch"
awk '$0 == "-A" { old=1 } $0 == "+B" { new=1 } END { exit !(old && new) }' "$experiment_directory/history.patch"
"$minigit_binary" diff -U0 > "$experiment_directory/after.patch"
cmp "$experiment_directory/working.patch" "$experiment_directory/after.patch"
printf 'Verified HEAD A, index B, worktree C and commit-to-commit A to B.\n'
