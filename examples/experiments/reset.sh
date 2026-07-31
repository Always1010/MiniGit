#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-reset.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
reset_ok() {
    "$minigit_binary" reset "$@" > "$experiment_directory/output"
    [[ ! -s "$experiment_directory/output" ]]
}
reset_error() {
    if "$minigit_binary" reset "$@" > "$experiment_directory/output" 2> "$experiment_directory/error"; then
        printf 'Reset unexpectedly succeeded.\n' >&2
        exit 1
    else
        reset_status=$?
        [[ $reset_status -eq 1 ]]
    fi
    [[ ! -s "$experiment_directory/output" ]]
}
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
printf 'D\n' > note
printf 'parent file\n' > dir
: > empty
"$minigit_binary" add -A
"$minigit_binary" commit -m D
target_commit=$(< .minigit/refs/heads/main)
printf 'A\n' > note
rm -- dir
mkdir dir
printf 'child\n' > dir/old
printf 'extra\n' > extra
"$minigit_binary" add -A
"$minigit_binary" commit -m A
head_commit=$(< .minigit/refs/heads/main)
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
cp .minigit/index "$experiment_directory/staged-index"
cp note "$experiment_directory/working-note"

# Soft moves history alone; mixed also resets the Index, leaving actual work.
reset_ok --soft "$target_commit"
[[ $(< .minigit/HEAD) == 'ref: refs/heads/main' && $(< .minigit/refs/heads/main) == "$target_commit" ]]
cmp .minigit/index "$experiment_directory/staged-index"
cmp note "$experiment_directory/working-note"
"$minigit_binary" reflog > "$experiment_directory/events"
IFS= read -r event < "$experiment_directory/events"
[[ $event == "HEAD@{0} $head_commit -> $target_commit reset:soft" ]]
reset_ok --soft "$head_commit"
reset_ok --mixed "$target_commit"
[[ $("$minigit_binary" show :note) == D && $(< dir/old) == child && $(< extra) == extra ]]
cmp note "$experiment_directory/working-note"
"$minigit_binary" reflog main > "$experiment_directory/events"
IFS= read -r event < "$experiment_directory/events"
[[ $event == "refs/heads/main@{0} $head_commit -> $target_commit reset:mixed" ]]

# Default mode is mixed. A same-ID reset must still discard staged content.
printf 'B\n' > note
"$minigit_binary" add note
printf 'C2\n' > note
cp .minigit/logs/HEAD "$experiment_directory/same-head-log"
cp .minigit/logs/refs/heads/main "$experiment_directory/same-branch-log"
reset_ok
[[ $("$minigit_binary" show :note) == D && $(< note) == C2 ]]
cmp .minigit/logs/HEAD "$experiment_directory/same-head-log"
cmp .minigit/logs/refs/heads/main "$experiment_directory/same-branch-log"

# Restore tracked ownership for A before the next independent hard scenario.
"$minigit_binary" restore -s "$head_commit" -S .
reset_ok --hard "$head_commit"
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
printf 'private\n' > dir/private
for path in HEAD index refs/heads/main logs/HEAD logs/refs/heads/main; do
    cp ".minigit/$path" "$experiment_directory/before-${path//\//-}"
done
cp note "$experiment_directory/working-note"
cp dir/old "$experiment_directory/working-child"
cp dir/private "$experiment_directory/private-child"
cp extra "$experiment_directory/working-extra"
reset_error --hard "$target_commit"
for path in HEAD index refs/heads/main logs/HEAD logs/refs/heads/main; do
    cmp ".minigit/$path" "$experiment_directory/before-${path//\//-}"
done
cmp note "$experiment_directory/working-note"
cmp dir/old "$experiment_directory/working-child"
cmp dir/private "$experiment_directory/private-child"
cmp extra "$experiment_directory/working-extra"
[[ ! -e .minigit/transaction ]]
rm -- dir/private
reset_ok --hard "$target_commit"
[[ $(< note) == D && $("$minigit_binary" show :note) == D && $(< dir) == 'parent file' ]]
[[ ! -e extra && -f empty && ! -s empty ]]
"$minigit_binary" reflog > "$experiment_directory/events"
IFS= read -r event < "$experiment_directory/events"
[[ $event == "HEAD@{0} $head_commit -> $target_commit reset:hard" ]]

# Same-ID hard restores dirty work without a new reference movement event.
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
cp .minigit/logs/HEAD "$experiment_directory/same-head-log"
cp .minigit/logs/refs/heads/main "$experiment_directory/same-branch-log"
reset_ok --hard
[[ $(< note) == D && $("$minigit_binary" show :note) == D ]]
cmp .minigit/logs/HEAD "$experiment_directory/same-head-log"
cmp .minigit/logs/refs/heads/main "$experiment_directory/same-branch-log"

# Detached resets move raw HEAD and its log, preserving the branch and its log.
"$minigit_binary" switch --detach "$head_commit"
cp .minigit/refs/heads/main "$experiment_directory/detached-ref"
cp .minigit/logs/refs/heads/main "$experiment_directory/detached-branch-log"
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
reset_ok --soft "$target_commit"
[[ $(< .minigit/HEAD) == "$target_commit" && $("$minigit_binary" show :note) == B && $(< note) == C ]]
reset_ok --mixed -- "$head_commit"
[[ $(< .minigit/HEAD) == "$head_commit" && $("$minigit_binary" show :note) == A && $(< note) == C ]]
reset_ok --hard "$target_commit"
[[ $(< .minigit/HEAD) == "$target_commit" && $(< note) == D ]]
cmp .minigit/refs/heads/main "$experiment_directory/detached-ref"
cmp .minigit/logs/refs/heads/main "$experiment_directory/detached-branch-log"

# A default unborn target is empty; explicit HEAD must still resolve a Commit.
cd -- "$experiment_directory"
"$minigit_binary" init unborn
cd unborn
printf 'B\n' > tracked
"$minigit_binary" add tracked
printf 'C\n' > tracked
printf 'private\n' > unknown
cp .minigit/HEAD "$experiment_directory/unborn-head"
cp .minigit/index "$experiment_directory/unborn-index"
cp tracked "$experiment_directory/unborn-work"
reset_ok --soft
cmp .minigit/index "$experiment_directory/unborn-index"
cmp tracked "$experiment_directory/unborn-work"
reset_error HEAD
cmp .minigit/HEAD "$experiment_directory/unborn-head"
cmp .minigit/index "$experiment_directory/unborn-index"
cmp tracked "$experiment_directory/unborn-work"
[[ ! -e .minigit/transaction ]]
reset_ok
cmp tracked "$experiment_directory/unborn-work"
if "$minigit_binary" show :tracked > "$experiment_directory/output" 2> "$experiment_directory/error"; then
    printf 'Default unborn mixed reset retained a staged file.\n' >&2
    exit 1
fi
"$minigit_binary" add tracked
reset_ok --hard
[[ ! -e tracked && $(< unknown) == private ]]
cmp .minigit/HEAD "$experiment_directory/unborn-head"
[[ ! -e .minigit/refs/heads/main && ! -e .minigit/logs/HEAD && ! -e .minigit/logs/refs/heads/main ]]
printf 'Reset experiment passed: modes, local edits, D/F protection, detached/unborn HEAD and reference events.\n'
