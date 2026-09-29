#!/usr/bin/env bash
# Compiler-warning ratchet (CORE-20). Counts the unique warnings in a build
# log and fails if there are more than test/warning-baseline.txt allows.
#
#   test/check_warning_baseline.sh <build-log> [baseline-file] [source-root]
#
# The log must come from a clean build (a no-op rebuild prints no
# warnings). A warning is counted once per file:line:column and message, so
# a header included by many translation units counts once. Paths are made
# relative to <source-root> (default: the current directory) before
# deduplication, so the count doesn't depend on where the tree is checked
# out.
#
# When the count drops below the baseline the check still passes, and says
# so: lower the number in the baseline file in the same PR that fixed the
# warnings, so they can't come back.
set -euo pipefail

log="$1"
baseline_file="${2:-$(dirname "$0")/warning-baseline.txt}"
root="${3:-$(pwd)}"

baseline="$(grep -v '^#' "$baseline_file" | tr -d '[:space:]')"
if ! [[ "$baseline" =~ ^[0-9]+$ ]]; then
	echo "error: no warning count found in $baseline_file" >&2
	exit 2
fi

warnings="$(grep -E ':[0-9]+:[0-9]+: warning: ' "$log" | sed -e "s#^${root%/}/##" | sort -u || true)"
count=0
if [ -n "$warnings" ]; then
	count="$(printf '%s\n' "$warnings" | wc -l | tr -d ' ')"
	printf '%s\n' "$warnings"
	echo
fi

echo "Unique compiler warnings: $count (baseline: $baseline)"

if [ "$count" -gt "$baseline" ]; then
	echo "error: $((count - baseline)) new warning(s) above the baseline in $baseline_file. Fix them (preferred) or justify raising the baseline in the PR." >&2
	exit 1
fi

if [ "$count" -lt "$baseline" ]; then
	echo "Warning count is below the baseline: lower $baseline_file to $count to lock in the improvement."
fi
