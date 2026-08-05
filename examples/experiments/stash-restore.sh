#!/usr/bin/env bash
set -euo pipefail
binary=$(realpath "${1:-./build/debug/minigit}")
experiment=$(mktemp -d)
trap 'rm -rf "$experiment"' EXIT
cd "$experiment"
run() { "$binary" "$@"; }
run init
run config user.name Learner
run config user.email learner@example.test
printf 'one\ntwo\nthree\n' > f
run add f
run commit -m base
printf 'ONE\ntwo\nthree\n' > f
run add f
printf 'ONE\nTWO\nthree\n' > f
run stash push -m 'separate staged and working changes'
printf 'one\ntwo\nTHREE\n' > f
run add f
run commit -m 'independent current change'
head_before=$(sha256sum .minigit/HEAD)
branch_before=$(sha256sum .minigit/refs/heads/main)
log_before=$(sha256sum .minigit/logs/HEAD)
run stash apply
[[ $(cat f) == $'ONE\nTWO\nTHREE' ]]
[[ $(run show :f) == $'one\ntwo\nTHREE' ]]
printf '%s\n' 'Default apply restores working changes while retaining the current Index:'
run status
run reset --hard
run stash apply --index
[[ $(cat f) == $'ONE\nTWO\nTHREE' ]]
[[ $(run show :f) == $'ONE\ntwo\nTHREE' ]]
printf '%s\n' '--index separately restores staged intent; the second line stays unstaged:'
run status
[[ $(sha256sum .minigit/HEAD) == "$head_before" ]]
[[ $(sha256sum .minigit/refs/heads/main) == "$branch_before" ]]
[[ $(sha256sum .minigit/logs/HEAD) == "$log_before" ]]
run reset --hard
run stash clear

# A failed pop keeps its entry; abort and continuation have different outcomes.
printf 'ours\n' > f
run stash push -m conflict
printf 'theirs\n' > f
run add f
run commit -m current
head_before=$(sha256sum .minigit/HEAD)
log_before=$(sha256sum .minigit/logs/HEAD)
stack_before=$(sha256sum .minigit/stashstack)
set +e
run stash pop
rc=$?
set -e
[[ $rc == 1 && -f .minigit/operation ]]
[[ $(sha256sum .minigit/stashstack) == "$stack_before" ]]
run stash --abort
[[ $(cat f) == theirs && ! -e .minigit/operation ]]
[[ $(sha256sum .minigit/stashstack) == "$stack_before" ]]
set +e
run stash pop
rc=$?
set -e
[[ $rc == 1 ]]
printf 'resolved\n' > f
run add f
printf 'resolved with a later unstaged edit\n' > f
run stash --continue
[[ $(run show :f) == resolved ]]
[[ $(cat f) == 'resolved with a later unstaged edit' ]]
[[ ! -e .minigit/operation && ! -e .minigit/stashstack ]]
[[ $(sha256sum .minigit/HEAD) == "$head_before" ]]
[[ $(sha256sum .minigit/logs/HEAD) == "$log_before" ]]
printf '%s\n' 'Stash restore experiment passed: independent layers, conflict retention, exact abort and pop continuation without a new commit.'
