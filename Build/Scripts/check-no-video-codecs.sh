#!/usr/bin/env bash
# Fails if a vendored FFI binary contains ffmpeg or OpenH264.
#
# Neither is reachable from this plugin - the livekit_ffi C ABI has no video
# path at all - but both are copyleft/patent-encumbered and we ship the bytes,
# which is what creates the obligation. See docs/webrtc-codec-removal-plan.md.
#
# ⚠️ THIS SCRIPT CURRENTLY FAILS BY DESIGN.
# livekit_ffi.dll still contains both. It will pass once that DLL is rebuilt
# against a libwebrtc built with H.264 disabled (step 1-2 of the plan). Wire it
# into CI at that point - step 5 - so a future SDK bump can't silently
# reintroduce them. Running it before then is how you verify the fix worked.
#
# Usage: Build/Scripts/check-no-video-codecs.sh [binary ...]
#        (defaults to every vendored FFI binary in the plugin)

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLUGIN="$REPO_ROOT/ProjectSandbox/Plugins/Open3DBroadcast"

# Substrings that indicate ffmpeg or OpenH264 code is linked in. Deliberately
# not VP8/VP9/AV1: libvpx, dav1d and libaom are permissively licensed and are
# not what we are trying to remove.
PATTERNS=(avcodec avutil avformat swscale swresample openh264 WelsEnc WelsDec)

if [ "$#" -gt 0 ]; then
	BINARIES=("$@")
else
	BINARIES=()
	while IFS= read -r f; do BINARIES+=("$f"); done < <(
		find "$PLUGIN" -type f \( -name '*.dll' -o -name '*.so' -o -name '*.dylib' \) 2>/dev/null | sort
	)
fi

if [ "${#BINARIES[@]}" -eq 0 ]; then
	echo "No binaries found to scan under $PLUGIN" >&2
	exit 1
fi

if ! command -v strings >/dev/null 2>&1; then
	echo "ERROR: 'strings' not found (install binutils)" >&2
	exit 1
fi

status=0
for bin in "${BINARIES[@]}"; do
	if [ ! -f "$bin" ]; then
		echo "ERROR: not found: $bin" >&2
		status=1
		continue
	fi

	hits=""
	for pat in "${PATTERNS[@]}"; do
		n=$(strings -a "$bin" | grep -ic -- "$pat" || true)
		[ "$n" -gt 0 ] && hits+="    $pat: $n
"
	done

	rel="${bin#"$REPO_ROOT"/}"
	if [ -n "$hits" ]; then
		echo "FAIL  $rel"
		printf '%s' "$hits"
		status=1
	else
		echo "ok    $rel"
	fi
done

echo
if [ "$status" -ne 0 ]; then
	echo "Video codec code found in a shipped binary."
	echo "See docs/webrtc-codec-removal-plan.md - this is expected until"
	echo "livekit_ffi.dll is rebuilt against a libwebrtc with H.264 disabled."
else
	echo "No ffmpeg/OpenH264 found in any scanned binary."
fi
exit "$status"
