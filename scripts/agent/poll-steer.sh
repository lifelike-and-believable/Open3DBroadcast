#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   REPO=owner/repo PR=123 scripts/agent/poll-steer.sh
# Requirements: gh CLI (authenticated), jq

REPO="${REPO:-owner/repo}"
PR="${PR:?Set PR number (e.g., 123)}"
STATE_FILE=".agent/last_seen"
mkdir -p .agent

last_seen="$(cat "$STATE_FILE" 2>/dev/null || echo 0)"

# Fetch all PR comments (Issues API covers PR comments)
json="$(gh api "/repos/$REPO/issues/$PR/comments?per_page=100" --paginate -H "Accept: application/vnd.github+json")"

# If any new /stop comment appears, print STOP and exit
if echo "$json" | jq -e --argjson last "$last_seen" '
  any(.[]?; .id > $last and (.body | startswith("/stop")) and
      (.user.login != "github-actions" and .user.login != "github-actions[bot]"))
' >/dev/null; then
  echo "STOP"
  # Advance watermark to newest comment id
  echo "$json" | jq -r 'max_by(.id).id' > "$STATE_FILE"
  exit 0
fi

# Print new /steer directives (oldest first), authored by a human user
echo "$json" | jq -r --argjson last "$last_seen" '
  [ .[] | select(.id > $last)
        | select(.user.type == "User")
        | select(.body | startswith("/steer")) ]
  | sort_by(.id) | .[].body
'

# Advance watermark to newest comment id (if any)
new_last="$(echo "$json" | jq -r 'max_by(.id).id // empty')"
if [[ -n "$new_last" ]]; then
  echo "$new_last" > "$STATE_FILE"
fi
