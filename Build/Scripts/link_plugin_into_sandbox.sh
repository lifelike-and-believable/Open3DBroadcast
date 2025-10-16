#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

PLUGIN_PATH="$REPO_ROOT/plugins/unreal/Open3DStream"
SANDBOX_PATH="$REPO_ROOT/ProjectSandbox"
DST="$SANDBOX_PATH/Plugins/Open3DStream"

if [ ! -d "$PLUGIN_PATH" ]; then
  echo "Error: Plugin not found at: $PLUGIN_PATH"
  exit 1
fi

echo "Linking plugin into sandbox..."
echo "  From: $PLUGIN_PATH"
echo "  To:   $DST"

rm -rf "$DST"
mkdir -p "$(dirname "$DST")"
ln -s "$PLUGIN_PATH" "$DST"

echo "✓ Linked: $DST -> $PLUGIN_PATH"
