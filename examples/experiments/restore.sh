#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-restore.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
mkdir dir
printf 'A\n' > note
printf 'child\n' > dir/old
printf 'obsolete\n' > obsolete
printf 'option name\n' > ./-note
: > empty
"$minigit_binary" add -A
"$minigit_binary" commit -m A
first_commit=$(< .minigit/refs/heads/main)
printf 'D\n' > note
rm -- dir/old obsolete
rmdir dir
printf 'parent file\n' > dir
"$minigit_binary" add -A
"$minigit_binary" commit -m D
cp .minigit/HEAD "$experiment_directory/head"
cp .minigit/refs/heads/main "$experiment_directory/ref"
cp .minigit/logs/HEAD "$experiment_directory/head-log"
cp .minigit/logs/refs/heads/main "$experiment_directory/branch-log"
cp .minigit/identity "$experiment_directory/identity"

# Default source is the Index; staged-only defaults to HEAD and leaves work alone.
printf 'B\n' > note
"$minigit_binary" add note
printf 'C\n' > note
"$minigit_binary" restore note
[[ $(< note) == B && $("$minigit_binary" show :note) == B ]]
printf 'C\n' > note
"$minigit_binary" restore --staged note
[[ $(< note) == C && $("$minigit_binary" show :note) == D ]]
"$minigit_binary" restore --staged --worktree note
[[ $(< note) == D && $("$minigit_binary" show :note) == D ]]

# An explicit source can update either destination independently.
"$minigit_binary" restore --source "$first_commit" --worktree note
[[ $(< note) == A && $("$minigit_binary" show :note) == D ]]
printf 'C\n' > note
"$minigit_binary" restore -s "$first_commit" -S note
[[ $(< note) == C && $("$minigit_binary" show :note) == A ]]
"$minigit_binary" restore -s "$first_commit" -S -W note
[[ $(< note) == A && $("$minigit_binary" show :note) == A ]]
printf 'edited\n' > ./-note
"$minigit_binary" restore -- -note
[[ $(< ./-note) == 'option name' ]]

# Prefix selection records both sides of a file/directory conversion.
"$minigit_binary" restore -s "$first_commit" -S -W .
[[ -d dir && $(< dir/old) == child && $(< obsolete) == obsolete && -f empty && ! -s empty ]]
printf 'private\n' > dir/private
cp .minigit/index "$experiment_directory/blocked-index"
if "$minigit_binary" restore -s HEAD -S -W dir > "$experiment_directory/output" 2> "$experiment_directory/error"; then
    printf 'Restore unexpectedly removed an unknown directory child.\n' >&2
    exit 1
fi
[[ $(< dir/private) == private && $(< dir/old) == child ]]
cmp .minigit/index "$experiment_directory/blocked-index"
[[ ! -e .minigit/transaction ]]
rm -- dir/private
"$minigit_binary" restore --source=HEAD --staged --worktree .
[[ -f dir && $(< dir) == 'parent file' && ! -e obsolete && -f empty && ! -s empty ]]

# A history-only path is not permission to overwrite an untracked file.
printf 'private\n' > obsolete
cp .minigit/index "$experiment_directory/blocked-index"
if "$minigit_binary" restore -s "$first_commit" obsolete > "$experiment_directory/output" 2> "$experiment_directory/error"; then
    printf 'Restore unexpectedly overwrote an untracked file.\n' >&2
    exit 1
fi
[[ $(< obsolete) == private ]]
cmp .minigit/index "$experiment_directory/blocked-index"
rm -- obsolete
[[ ! -e .minigit/transaction ]]

# Restore never changes historical position or appends reference events.
cmp .minigit/HEAD "$experiment_directory/head"
cmp .minigit/refs/heads/main "$experiment_directory/ref"
cmp .minigit/logs/HEAD "$experiment_directory/head-log"
cmp .minigit/logs/refs/heads/main "$experiment_directory/branch-log"
cmp .minigit/identity "$experiment_directory/identity"
[[ -z $("$minigit_binary" status -s) ]]
printf 'Restore experiment passed: sources, destinations, deletions, D/F protection and unchanged HEAD/logs.\n'
