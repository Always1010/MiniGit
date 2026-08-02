#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-merge.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
head_id() {
    local log_text first_line
    log_text=$("$minigit_binary" log -n 1)
    first_line=${log_text%%$'\n'*}
    [[ $first_line == 'commit '* && ${#first_line} -eq 71 ]]
    printf '%s\n' "${first_line#commit }"
}
printf 'base\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m base
"$minigit_binary" switch -c topic
printf 'theirs\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m theirs
theirs=$(head_id)
"$minigit_binary" switch main
printf 'ours\n' > note
chmod 0640 note
"$minigit_binary" add note
"$minigit_binary" commit -m ours
ours=$(head_id)
cp .minigit/index "$experiment_directory/original-index"
cp .minigit/HEAD "$experiment_directory/original-head"
merge_conflict() {
    if "$minigit_binary" merge "$@" > "$experiment_directory/output"; then
        printf 'Expected a conflict.\n' >&2
        exit 1
    else
        merge_status=$?
        [[ $merge_status -eq 1 ]]
    fi
    [[ $(< "$experiment_directory/output") == *'CONFLICT note'* ]]
    [[ -f .minigit/operation && ! -e .minigit/transaction ]]
}
merge_conflict topic
[[ $("$minigit_binary" status --porcelain) == 'UU note' ]]
[[ $("$minigit_binary" show :1:note) == base ]]
[[ $("$minigit_binary" show :2:note) == ours ]]
[[ $("$minigit_binary" show :3:note) == theirs ]]
printf 'edited while resolving\n' > note
"$minigit_binary" merge --abort
[[ $(< note) == ours && $(stat -c '%a' note) == 640 && ! -e .minigit/operation ]]
cmp .minigit/index "$experiment_directory/original-index"
cmp .minigit/HEAD "$experiment_directory/original-head"
merge_conflict -m 'Keep both ideas' topic
printf 'ours and theirs\n' > note
"$minigit_binary" add note
printf 'a later unstaged edit\n' > note
"$minigit_binary" merge --continue
merged=$(head_id)
commit_text=$("$minigit_binary" cat-file -p "$merged")
[[ $commit_text == *"parent $ours"$'\n'"parent $theirs"$'\n'* ]]
[[ $("$minigit_binary" show HEAD:note) == 'ours and theirs' ]]
[[ $(< note) == 'a later unstaged edit' && ! -e .minigit/operation ]]
printf 'Merge experiment passed: stages, exact abort, ordered parents and staged continuation.\n'
