#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-remove.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
remove_ok() {
    "$minigit_binary" rm "$@" > "$experiment_directory/output"
    [[ ! -s "$experiment_directory/output" ]]
}
remove_error() {
    # Keep a fresh baseline before every refused batch, including reference logs.
    for path in HEAD index refs/heads/main logs/HEAD logs/refs/heads/main; do
        cp ".minigit/$path" "$experiment_directory/before-${path//\//-}"
    done
    if "$minigit_binary" rm "$@" > "$experiment_directory/output" 2> "$experiment_directory/error"; then
        printf 'Remove unexpectedly succeeded.\n' >&2
        exit 1
    else
        remove_status=$?
        [[ $remove_status -eq 1 ]]
    fi
    [[ ! -s "$experiment_directory/output" && ! -e .minigit/transaction ]]
    for path in HEAD index refs/heads/main logs/HEAD logs/refs/heads/main; do
        cmp ".minigit/$path" "$experiment_directory/before-${path//\//-}"
    done
}
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
mkdir dir
for path in clean gone staged dirty batch cached-head cached-staged cached-all cached-link shape dir/one dir/two; do
    printf 'A\n' > "$path"
done
printf 'option file\n' > ./-note
"$minigit_binary" add -A
"$minigit_binary" commit -m A
for path in HEAD refs/heads/main logs/HEAD logs/refs/heads/main identity; do
    cp ".minigit/$path" "$experiment_directory/history-${path//\//-}"
done

# Clean tracked files and genuinely missing work files can be removed normally.
remove_ok clean
[[ ! -e clean ]]
rm -- gone
remove_ok gone

# A staged-only version is protected; force explicitly permits discarding it.
printf 'B\n' > staged
"$minigit_binary" add staged
cp staged "$experiment_directory/staged-file"
remove_error staged
cmp staged "$experiment_directory/staged-file"
remove_ok --force staged
[[ ! -e staged ]]

# A later dirty path must not cause an earlier safe path to be partly deleted.
printf 'C\n' > dirty
cp dirty "$experiment_directory/dirty-file"
cp batch "$experiment_directory/batch-file"
remove_error batch dirty
cmp dirty "$experiment_directory/dirty-file"
cmp batch "$experiment_directory/batch-file"
remove_ok -f dirty
remove_ok batch

# Cached removal keeps work. B==A short-circuits even a physical symlink.
printf 'C\n' > cached-head
remove_ok --cached cached-head
[[ $(< cached-head) == C ]]
printf 'outside\n' > "$experiment_directory/outside"
rm -- cached-link
ln -s "$experiment_directory/outside" cached-link
remove_ok --cached cached-link
[[ -L cached-link && $(< "$experiment_directory/outside") == outside ]]
rm -- cached-link

# If B differs from A, cached may still remove it when actual work equals B.
printf 'B\n' > cached-staged
"$minigit_binary" add cached-staged
remove_ok --cached cached-staged
[[ $(< cached-staged) == B ]]
printf 'B\n' > cached-all
"$minigit_binary" add cached-all
printf 'C\n' > cached-all
cp cached-all "$experiment_directory/cached-work"
remove_error --cached cached-all
cmp cached-all "$experiment_directory/cached-work"
remove_ok --cached -f cached-all
cmp cached-all "$experiment_directory/cached-work"

# Directory prefixes need -r; deletion of tracked leaves retains unknown siblings.
printf 'dir/private\n' > .minigitignore
printf 'private\n' > dir/private
mkdir dir/private-empty
cp dir/one "$experiment_directory/dir-one"
cp dir/two "$experiment_directory/dir-two"
remove_error dir
cmp dir/one "$experiment_directory/dir-one"
cmp dir/two "$experiment_directory/dir-two"
remove_ok --recursive dir
[[ ! -e dir/one && ! -e dir/two && $(< dir/private) == private && -d dir/private-empty ]]

# A tracked file changed into a directory is not ordinary absence; force cannot
# extend exact ownership to its untracked child.
rm -- shape
mkdir shape
printf 'private\n' > shape/private
cp shape/private "$experiment_directory/shape-private"
remove_error shape
cmp shape/private "$experiment_directory/shape-private"
remove_error -f shape
cmp shape/private "$experiment_directory/shape-private"
remove_error -f dir/private
[[ $(< dir/private) == private ]]
remove_ok -- -note
[[ ! -e ./-note ]]

for path in HEAD refs/heads/main logs/HEAD logs/refs/heads/main identity; do
    cmp ".minigit/$path" "$experiment_directory/history-${path//\//-}"
done
[[ ! -e .minigit/transaction ]]
printf 'Remove experiment passed: safety matrix, cached/force, recursive leaves and unchanged HEAD/logs.\n'
