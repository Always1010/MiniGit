#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-replay.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
head_id() {
    local log_text first_line
    log_text=$("$minigit_binary" log -n 1)
    first_line=${log_text%%$'\n'*}
    [[ $first_line == 'commit '* && ${#first_line} -eq 71 ]]
    printf '%s\n' "${first_line#commit }"
}
"$minigit_binary" config user.name Source
"$minigit_binary" config user.email source@example.test
printf 'base\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m base
"$minigit_binary" switch -c source
printf 'source\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m source-change
source_id=$(head_id)
"$minigit_binary" switch main
"$minigit_binary" config user.name Receiver
"$minigit_binary" config user.email receiver@example.test
printf 'independent current work\n' > extra
"$minigit_binary" add extra
"$minigit_binary" commit -m receiver
original_id=$(head_id)
"$minigit_binary" cherry-pick source
picked_id=$(head_id)
commit_text=$("$minigit_binary" cat-file -p "$picked_id")
[[ $(< note) == source && $(< extra) == 'independent current work' ]]
[[ $commit_text == *"parent $original_id"$'\n''author Source <source@example.test>'* ]]
[[ $commit_text == *'committer Receiver <receiver@example.test>'* ]]
"$minigit_binary" revert "$source_id"
reverted_id=$(head_id)
commit_text=$("$minigit_binary" cat-file -p "$reverted_id")
[[ $(< note) == base && $(< extra) == 'independent current work' ]]
[[ $commit_text == *"parent $picked_id"$'\n''author Receiver <receiver@example.test>'* ]]
[[ $commit_text == *"Revert $source_id"* ]]
# Replaying an already-applied inverse still records one new empty commit.
"$minigit_binary" revert "$source_id"
empty_id=$(head_id)
[[ $empty_id != "$reverted_id" && $(< note) == base ]]
commit_text=$("$minigit_binary" cat-file -p "$empty_id")
[[ $commit_text == *"parent $reverted_id"$'\n''author '* ]]
printf 'ours\n' > note
chmod 0640 note
"$minigit_binary" add note
"$minigit_binary" commit -m ours
cp .minigit/index "$experiment_directory/original-index"
cp .minigit/HEAD "$experiment_directory/original-head"
if "$minigit_binary" cherry-pick --message 'Replay after resolution' source > "$experiment_directory/output"; then
    printf 'Expected a cherry-pick conflict.\n' >&2
    exit 1
else
    replay_status=$?
    [[ $replay_status -eq 1 ]]
fi
[[ $(< "$experiment_directory/output") == *'CONFLICT note'* ]]
[[ $("$minigit_binary" show :1:note) == base ]]
[[ $("$minigit_binary" show :2:note) == ours ]]
[[ $("$minigit_binary" show :3:note) == source ]]
printf 'edited marker\n' > note
"$minigit_binary" cherry-pick --abort
[[ $(< note) == ours && $(stat -c '%a' note) == 640 && ! -e .minigit/operation ]]
cmp .minigit/index "$experiment_directory/original-index"
cmp .minigit/HEAD "$experiment_directory/original-head"
printf 'Replay experiment passed: directed changes, source author, single parents, empty replay and exact abort.\n'
