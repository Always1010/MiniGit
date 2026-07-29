#!/usr/bin/env bash
set -euo pipefail
minigit=$(realpath "${1:-./build/debug/minigit}")
lab=$(realpath "${2:-./build/debug/minigit_transaction_lab}")
task_dir=$(mktemp -d "${TMPDIR:-/tmp}/minigit-transaction.XXXXXX")
trap 'rm -rf "$task_dir"' EXIT
"$lab" "$task_dir/repo"
cd "$task_dir/repo"
printf 'new A\n' > "$task_dir/new-a"
printf 'old A\n' > "$task_dir/old-a"
printf 'old B\n' > "$task_dir/old-b"
cmp a "$task_dir/new-a"
cmp b "$task_dir/old-b"
if "$minigit" status > "$task_dir/status.out" 2> "$task_dir/status.err"; then
    printf 'Expected status to require recovery\n' >&2
    exit 1
fi
test ! -s "$task_dir/status.out"
test -f .minigit/transaction
"$minigit" recover
cmp a "$task_dir/old-a"
cmp b "$task_dir/old-b"
test ! -e .minigit/transaction
test ! -e .minigit/transaction-staging
"$minigit" recover
cmp a "$task_dir/old-a"
cmp b "$task_dir/old-b"
printf 'Verified partial update, recovery protection and repeatable rollback.\n'
