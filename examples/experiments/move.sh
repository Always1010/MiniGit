#!/usr/bin/env bash
set -euo pipefail
minigit_input=${1:-./build/debug/minigit}
[[ $# -le 1 ]]
minigit_directory=$(cd -- "$(dirname -- "$minigit_input")" && pwd -P)
minigit_binary="$minigit_directory/$(basename -- "$minigit_input")"
[[ -x "$minigit_binary" ]]
experiment_directory=$(mktemp -d "${TMPDIR:-/tmp}/minigit-move.XXXXXX")
trap 'rm -rf -- "$experiment_directory"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
move_ok() {
    "$minigit_binary" mv "$@" > "$experiment_directory/output"
    [[ ! -s "$experiment_directory/output" ]]
}
move_error() {
    # Capture the metadata before the refused move, not after the error.
    for path in HEAD index refs/heads/main logs/HEAD logs/refs/heads/main identity; do
        cp ".minigit/$path" "$experiment_directory/before-${path//\//-}"
    done
    if "$minigit_binary" mv "$@" > "$experiment_directory/output" 2> "$experiment_directory/error"; then
        printf 'Move unexpectedly succeeded.\n' >&2
        exit 1
    else
        move_status=$?
        [[ $move_status -eq 1 ]]
    fi
    [[ ! -s "$experiment_directory/output" && ! -e .minigit/transaction ]]
    for path in HEAD index refs/heads/main logs/HEAD logs/refs/heads/main identity; do
        cmp ".minigit/$path" "$experiment_directory/before-${path//\//-}"
    done
}
cd -- "$experiment_directory"
"$minigit_binary" init repo
cd repo
"$minigit_binary" config user.name Learner
"$minigit_binary" config user.email learner@example.test
mkdir -p tree/sub protected
for path in source collision-source index-source index-target basename tree/one tree/sub/two protected/tracked; do
    printf 'A\n' > "$path"
done
printf 'option file\n' > ./-old
"$minigit_binary" add -A
"$minigit_binary" commit -m A
for path in HEAD refs/heads/main logs/HEAD logs/refs/heads/main identity; do
    cp ".minigit/$path" "$experiment_directory/history-${path//\//-}"
done

# An MM source has two versions. Move preserves B in Index and actual C in Work.
printf 'B\n' > source
chmod 0640 source
"$minigit_binary" add source
printf 'C\n' > source
chmod 0751 source
cp source "$experiment_directory/working-source"
"$minigit_binary" show :source > "$experiment_directory/staged-source"
move_ok source renamed
[[ ! -e source && $(stat -c '%a' renamed) == 751 ]]
cmp renamed "$experiment_directory/working-source"
"$minigit_binary" show :renamed > "$experiment_directory/moved-index"
cmp "$experiment_directory/moved-index" "$experiment_directory/staged-source"
staged_diff=$("$minigit_binary" diff --cached)
[[ $staged_diff == *'new file mode 100644'* ]]
status_output=$("$minigit_binary" status --porcelain)
[[ $status_output == *'AM renamed'* ]]

# Same bytes do not authorize an existing destination; Index-only occupancy also
# blocks moving there, even when its work file has been deleted.
cp collision-source existing-target
cp collision-source "$experiment_directory/collision-source"
cp existing-target "$experiment_directory/existing-target"
move_error collision-source existing-target
cmp collision-source "$experiment_directory/collision-source"
cmp existing-target "$experiment_directory/existing-target"
rm -- index-target
cp index-source "$experiment_directory/index-source"
move_error index-source index-target
cmp index-source "$experiment_directory/index-source"
[[ ! -e index-target ]]

# An existing destination directory appends the source basename once and keeps
# its own permissions. Newly required directories use the planner's 0755 mode.
mkdir destination
chmod 0770 destination
move_ok basename destination
[[ ! -e basename && $(< destination/basename) == A && $(stat -c '%a' destination) == 770 ]]
printf 'B-tree\n' > tree/one
"$minigit_binary" add tree/one
printf 'C-tree\n' > tree/one
chmod 0700 tree
cp tree/one "$experiment_directory/tree-one"
cp tree/sub/two "$experiment_directory/tree-two"
"$minigit_binary" show :tree/one > "$experiment_directory/tree-index"
move_ok tree new/renamed-tree
[[ ! -e tree && $(stat -c '%a' new) == 755 && $(stat -c '%a' new/renamed-tree) == 755 ]]
cmp new/renamed-tree/one "$experiment_directory/tree-one"
cmp new/renamed-tree/sub/two "$experiment_directory/tree-two"
"$minigit_binary" show :new/renamed-tree/one > "$experiment_directory/moved-tree-index"
cmp "$experiment_directory/moved-tree-index" "$experiment_directory/tree-index"

# Moving a tracked directory cannot collect ignored files or unknown empty dirs.
printf 'protected/private\n' > .minigitignore
printf 'private\n' > protected/private
cp protected/private "$experiment_directory/private-file"
cp protected/tracked "$experiment_directory/protected-tracked"
move_error protected blocked
cmp protected/private "$experiment_directory/private-file"
cmp protected/tracked "$experiment_directory/protected-tracked"
[[ ! -e blocked ]]
rm -- protected/private
mkdir protected/private-empty
move_error protected blocked
cmp protected/tracked "$experiment_directory/protected-tracked"
[[ -d protected/private-empty && ! -e blocked ]]
rmdir protected/private-empty
move_error protected protected/inside
cmp protected/tracked "$experiment_directory/protected-tracked"
[[ ! -e protected/inside ]]
move_ok protected known-only
[[ ! -e protected && $(< known-only/tracked) == A ]]

move_ok -- -old -new
[[ ! -e ./-old && $(< ./-new) == 'option file' ]]
for path in HEAD refs/heads/main logs/HEAD logs/refs/heads/main identity; do
    cmp ".minigit/$path" "$experiment_directory/history-${path//\//-}"
done
[[ ! -e .minigit/transaction ]]
printf 'Move experiment passed: separate staged/work versions, permissions, directory moves and unchanged HEAD/logs.\n'
