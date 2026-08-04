#!/usr/bin/env bash
set -euo pipefail
minigit_bin=$(realpath "${1:-./build/debug/minigit}")
lab=$(mktemp -d "${TMPDIR:-/tmp}/minigit-stash.XXXXXX")
trap 'rm -rf "$lab"' EXIT
mkdir "$lab/repo"
cd "$lab/repo"
run() { "$minigit_bin" "$@"; }
head_id() { run log -n 1 | sed -n '1s/^commit //p'; }
run init
run config user.name Learner
run config user.email learner@example.test
printf 'A\n' > f
printf 'ignored\n' > .minigitignore
run add -A
run commit -m base
base=$(head_id)
head_before=$(sha256sum .minigit/HEAD)
log_before=$(sha256sum .minigit/logs/HEAD)
printf 'B\n' > f
run add f
printf 'C\n' > f
printf 'untracked bytes\n' > unknown
printf 'keep ignored\n' > ignored
run stash push -u -m 'three saved layers'
stash_id=$(tr -d '\n' < .minigit/refs/stash)
mapfile -t parents < <(run cat-file -p "$stash_id" | sed -n 's/^parent //p')
[[ ${#parents[@]} -eq 3 && ${parents[0]} == "$base" ]]
[[ $(run show "${parents[1]}:f") == B ]]
[[ $(run show "$stash_id:f") == C ]]
[[ $(run show "${parents[2]}:unknown") == 'untracked bytes' ]]
[[ $(cat f) == A && $(run show :f) == A ]]
[[ ! -e unknown && $(cat ignored) == 'keep ignored' ]]
[[ $(sha256sum .minigit/HEAD) == "$head_before" ]]
[[ $(sha256sum .minigit/logs/HEAD) == "$log_before" ]]
patch=$(run stash show)
[[ $patch == *'+C'* && $patch == *'+untracked bytes'* ]]
run stash list
stack_before=$(sha256sum .minigit/stashstack)
index_before=$(sha256sum .minigit/index)
[[ $(run stash) == 'No local changes to save.' ]]
[[ $(sha256sum .minigit/stashstack) == "$stack_before" ]]
[[ $(sha256sum .minigit/index) == "$index_before" ]]
printf 'D\n' > f
run stash push -m 'newer entry'
new_top=$(cat .minigit/refs/stash)
run stash drop 'stash@{1}'
[[ $(cat .minigit/refs/stash) == "$new_top" ]]
[[ $(run stash list) != *'stash@{1}'* ]]
run stash clear
[[ ! -e .minigit/stashstack && ! -e .minigit/refs/stash ]]
[[ $(cat f) == A && $(cat ignored) == 'keep ignored' ]]
[[ $(sha256sum .minigit/HEAD) == "$head_before" ]]
[[ $(sha256sum .minigit/logs/HEAD) == "$log_before" ]]
printf '%s\n' 'Stash save experiment passed: separate HEAD/Index/Work/untracked snapshots, cleanup, stack order and unchanged history.'
