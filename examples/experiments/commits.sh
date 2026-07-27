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
"$minigit_binary" init .
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
printf 'A\n' > note
printf 'B\n' > note
"$minigit_binary" add note
staged_blob=$("$minigit_binary" hash-object note)
printf 'C\n' > note
"$minigit_binary" commit -m 'Save B while working file contains C'
first_commit=$(< .minigit/refs/heads/main)
"$minigit_binary" cat-file -p "$first_commit" > first-commit.txt
first_tree=$(awk '$1 == "tree" { print $2; exit }' first-commit.txt)
"$minigit_binary" cat-file -p "$first_tree" > first-tree.txt
awk -v expected="$staged_blob" '$1 == "100644" && $2 == expected && $3 == "note" { found=1 } END { exit !found }' first-tree.txt
[[ $("$minigit_binary" cat-file -p "$staged_blob") == B ]]
[[ $(< note) == C ]]
"$minigit_binary" add note
"$minigit_binary" commit -m 'Save C'
second_commit=$(< .minigit/refs/heads/main)
"$minigit_binary" cat-file -p "$second_commit" > second-commit.txt
awk -v expected="$first_commit" '$1 == "parent" && $2 == expected { found=1 } END { exit !found }' second-commit.txt
printf 'Commit experiment passed: index preserves B and the next commit links its parent.\n'
