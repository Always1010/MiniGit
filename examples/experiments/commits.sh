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
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-commits.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd -- "$experiment_directory/repo"
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
printf 'A\n' > note
printf 'B\n' > note
"$minigit_binary" add note
staged_blob=$("$minigit_binary" hash-object note)
printf 'C\n' > note
[[ $("$minigit_binary" status --porcelain) == 'AM note' ]]
"$minigit_binary" commit -m 'Save B while working file contains C'
first_commit=$(< .minigit/refs/heads/main)
[[ $("$minigit_binary" status --porcelain) == ' M note' ]]
"$minigit_binary" cat-file -p "$first_commit" > "$experiment_directory/first-commit.txt"
first_tree=$(awk '$1 == "tree" { print $2; exit }' "$experiment_directory/first-commit.txt")
"$minigit_binary" cat-file -p "$first_tree" > "$experiment_directory/first-tree.txt"
awk -v expected="$staged_blob" '$1 == "100644" && $2 == expected && $3 == "note" { found=1 } END { exit !found }' "$experiment_directory/first-tree.txt"
[[ $("$minigit_binary" cat-file -p "$staged_blob") == B ]]
[[ $(< note) == C ]]
"$minigit_binary" tag saved-B
"$minigit_binary" add note
"$minigit_binary" commit -m 'Save C'
second_commit=$(< .minigit/refs/heads/main)
"$minigit_binary" cat-file -p "$second_commit" > "$experiment_directory/second-commit.txt"
awk -v expected="$first_commit" '$1 == "parent" && $2 == expected { found=1 } END { exit !found }' "$experiment_directory/second-commit.txt"
[[ -z $("$minigit_binary" status --porcelain) ]]
[[ $("$minigit_binary" tag) == saved-B ]]
[[ $("$minigit_binary" log --oneline -n 1 saved-B) == "${first_commit:0:12} Save B while working file contains C" ]]
[[ $(< .minigit/refs/tags/saved-B) == "$first_commit" ]]
"$minigit_binary" tag -d saved-B
[[ -z $("$minigit_binary" tag) ]]
printf 'Phase 1 experiment passed: staged snapshots, status, parent links, log and a fixed light tag.\n'
