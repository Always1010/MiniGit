#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-gc.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
printf 'base\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m base
head_id() {
    local log_text first_line
    log_text=$("$minigit_binary" log -n 1)
    first_line=${log_text%%$'\n'*}
    [[ $first_line == 'commit '* && ${#first_line} -eq 71 ]]
    printf '%s\n' "${first_line#commit }"
}
base_id=$(head_id)
printf 'later\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m later
old_tip=$(head_id)
"$minigit_binary" reset --hard "$base_id"
printf 'unreferenced object\n' > orphan
orphan_id=$("$minigit_binary" hash-object -w orphan)
[[ ${#orphan_id} -eq 64 ]]
"$minigit_binary" gc
[[ $("$minigit_binary" cat-file -p "$orphan_id") == 'unreferenced object' ]]
"$minigit_binary" gc --prune-now
if "$minigit_binary" cat-file -t "$orphan_id" > /dev/null 2>&1; then
    printf 'orphan unexpectedly retained\n' >&2
    exit 1
fi
[[ $(cat orphan) == 'unreferenced object' ]]
[[ $("$minigit_binary" cat-file -t "$old_tip") == commit ]]
"$minigit_binary" switch --detach "$old_tip"
[[ $(cat note) == later ]]
"$minigit_binary" switch main

# The entire stash stack is a root, even entries below refs/stash's top.
printf 'older stash\n' > note
"$minigit_binary" stash push -m older
printf 'newer stash\n' > note
"$minigit_binary" stash push -m newer
"$minigit_binary" gc --prune-now
"$minigit_binary" stash pop
[[ $(cat note) == 'newer stash' ]]
"$minigit_binary" restore note
"$minigit_binary" stash pop
[[ $(cat note) == 'older stash' ]]
"$minigit_binary" restore note

# Conflict Index stages and long-operation backups also remain reachable.
"$minigit_binary" switch -c topic
printf 'topic\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m topic
"$minigit_binary" switch main
printf 'main\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m main
set +e
"$minigit_binary" merge topic
merge_status=$?
set -e
[[ $merge_status -eq 1 && -f .minigit/operation ]]
index_before=$(sha256sum .minigit/index)
"$minigit_binary" gc --prune-now
[[ $(sha256sum .minigit/index) == "$index_before" ]]
[[ $("$minigit_binary" show :1:note) == base ]]
[[ $("$minigit_binary" show :2:note) == main ]]
[[ $("$minigit_binary" show :3:note) == topic ]]
"$minigit_binary" merge --abort
[[ $(cat note) == main && ! -e .minigit/operation ]]
printf 'GC experiment passed: grace, orphan pruning, reflog, full stash and active conflict roots.\n'
