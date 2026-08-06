#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-rebase.XXXXXX")
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
parent_id() {
    local commit_text line
    commit_text=$("$minigit_binary" cat-file -p "$1")
    while IFS= read -r line; do
        if [[ $line == 'parent '* ]]; then printf '%s\n' "${line#parent }"; return; fi
        [[ -n $line ]] || break
    done <<< "$commit_text"
    return 1
}
"$minigit_binary" config user.name Source
"$minigit_binary" config user.email source@example.test
printf 'alpha\nmiddle\nomega\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m base
base_id=$(head_id)
"$minigit_binary" switch -c topic
printf 'TOPIC\nmiddle\nomega\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m first-source
first_source_id=$(head_id)
printf 'topic extra\n' > extra
"$minigit_binary" add extra
"$minigit_binary" commit -m second-source
old_tip=$(head_id)
old_commit=$("$minigit_binary" cat-file -p "$old_tip")
"$minigit_binary" switch main
printf 'alpha\nmiddle\nMAIN\n' > note
"$minigit_binary" add note
"$minigit_binary" commit -m main
onto_id=$(head_id)
"$minigit_binary" switch topic
"$minigit_binary" config user.name Receiver
"$minigit_binary" config user.email receiver@example.test
"$minigit_binary" rebase main
new_tip=$(head_id)
new_first=$(parent_id "$new_tip")
[[ $(parent_id "$new_first") == "$onto_id" && $new_tip != "$old_tip" && $new_first != "$first_source_id" ]]
[[ $(< note) == $'TOPIC\nmiddle\nMAIN' && $(< extra) == 'topic extra' ]]
[[ $(< .minigit/HEAD) == 'ref: refs/heads/topic' && $(< .minigit/refs/heads/topic) == "$new_tip" ]]
[[ $(< .minigit/refs/heads/main) == "$onto_id" && ! -e .minigit/operation ]]
[[ $("$minigit_binary" cat-file -p "$old_tip") == "$old_commit" ]]
for revision in "$new_first" "$new_tip"; do
    commit_text=$("$minigit_binary" cat-file -p "$revision")
    [[ $commit_text == *'author Source <source@example.test>'* && $commit_text == *'committer Receiver <receiver@example.test>'* ]]
done
# A fork with an incompatible note demonstrates the persistent conflict path.
"$minigit_binary" switch -c conflict "$base_id"
"$minigit_binary" config user.name Source
"$minigit_binary" config user.email source@example.test
printf 'source conflict\n' > note
chmod 0640 note
"$minigit_binary" add note
"$minigit_binary" commit -m conflict-source
conflict_tip=$(head_id)
"$minigit_binary" config user.name Receiver
"$minigit_binary" config user.email receiver@example.test
cp .minigit/index "$experiment_directory/original-index"
cp .minigit/HEAD "$experiment_directory/original-head"
expect_conflict() {
    if "$minigit_binary" rebase main > "$experiment_directory/output"; then
        printf 'Expected a rebase conflict.\n' >&2; exit 1
    else
        local rebase_status=$?
        [[ $rebase_status -eq 1 ]]
    fi
    [[ $(< "$experiment_directory/output") == *'CONFLICT note'* && -f .minigit/operation ]]
    [[ $(< .minigit/refs/heads/conflict) == "$conflict_tip" ]]
}
expect_conflict
printf 'edited marker\n' > note
"$minigit_binary" rebase --abort
[[ $(< note) == 'source conflict' && $(stat -c '%a' note) == 640 && ! -e .minigit/operation ]]
cmp .minigit/index "$experiment_directory/original-index"
cmp .minigit/HEAD "$experiment_directory/original-head"
expect_conflict
printf 'staged resolution\n' > note
"$minigit_binary" add note
printf 'later unstaged edit\n' > note
"$minigit_binary" rebase --continue
[[ $("$minigit_binary" show HEAD:note) == 'staged resolution' && $(< note) == 'later unstaged edit' ]]
[[ $(< .minigit/HEAD) == 'ref: refs/heads/conflict' && ! -e .minigit/operation ]]
printf 'Rebase experiment passed: ordered replay, unchanged old objects, source author, exact abort and Index-only continuation.\n'
