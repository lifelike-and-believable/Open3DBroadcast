#!/usr/bin/env python3
"""Check that every first-party plugin source file starts with a copyright header.

The rule (FAB-5, FAB-9, WP-F4; Fab TR 4.3.6.1.b asks for the publisher's
name and the year): every git-tracked .h, .cpp and .cs file under the Source/
folder of each plugin in this repository starts with

    // Copyright <year> Lifelike & Believable. All Rights Reserved.

where <year> is four digits, followed by a blank line (or the end of the
file). Files that came from Open3DStream carry its notice on the second line,

    // Portions Copyright (c) Open3DStream Contributors

and what follows that line is not checked (maintainer, 2026-10-05). A
leading UTF-8 BOM is ignored. Line endings may be LF or CRLF.

Not checked:
  - anything under a ThirdParty/ directory (vendored libraries keep their own
    notices);
  - generated files: names ending in .generated.h, _generated.h or .gen.cpp,
    or a file whose first lines say it is generated ("@generated",
    "automatically generated", "auto-generated", "DO NOT EDIT");
  - files listed in Build/Fab/copyright-allowlist.txt (genuine third-party
    code outside a ThirdParty/ directory). Each entry needs a reason, and an
    entry for a file git does not track in any checked plugin is an error.

Plugins checked by default: ProjectSandbox/Plugins/Open3DBroadcast and the
WebRTC add-on ProjectSandbox/Plugins/Open3DBroadcastWebRTC (WP-F11). Pass
--plugin-dir (repeatable) to check others.

Python 3.8 or later, standard library only. Exit codes: 0 every checked file
has the header; 1 at least one file is missing it, or the allowlist has a
stale entry; 2 bad input (not a git checkout, unreadable allowlist).
"""

import argparse
import os
import re
import subprocess
import sys

HEADER_RE = re.compile(r"// Copyright \d{4} Lifelike & Believable\. All Rights Reserved\.")
HEADER_EXAMPLE = "// Copyright 2026 Lifelike & Believable. All Rights Reserved."
# Second line of files that came from Open3DStream (maintainer, 2026-10-05). Not for new files.
PORTIONS_LINE = "// Portions Copyright (c) Open3DStream Contributors"

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_PLUGIN_DIRS = [
    os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcast"),
    os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcastWebRTC"),
]
DEFAULT_ALLOWLIST = os.path.join(REPO_ROOT, "Build", "Fab", "copyright-allowlist.txt")

EXTENSIONS = (".h", ".cpp", ".cs")
GENERATED_SUFFIXES = (".generated.h", "_generated.h", ".gen.cpp")
GENERATED_MARKER = re.compile(r"@generated|automatically generated|auto-generated|do not edit", re.IGNORECASE)
GENERATED_MARKER_LINES = 10
UTF8_BOM = b"\xef\xbb\xbf"


class InputError(Exception):
    pass


def tracked_sources(plugin_dir):
    """Git-tracked .h/.cpp/.cs files under <plugin>/Source, relative to the plugin root."""
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "--", "Source"],
            cwd=plugin_dir, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise InputError("git ls-files failed in {}: {}".format(plugin_dir, e))
    paths = [p for p in out.decode("utf-8").split("\0") if p]
    return sorted(p for p in paths if p.endswith(EXTENSIONS))


def read_allowlist(path):
    """Return {plugin-relative path: reason}. Format: '<path> <reason>' per line."""
    entries = {}
    if not os.path.exists(path):
        return entries
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise InputError("cannot read {}: {}".format(path, e))
    for number, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2 or not parts[1].strip():
            raise InputError("{}:{}: entry '{}' has no reason".format(path, number, parts[0]))
        entries[parts[0].replace("\\", "/")] = parts[1].strip()
    return entries


def is_third_party(rel_path):
    return "ThirdParty" in rel_path.split("/")[:-1]


def is_generated(rel_path, text):
    if rel_path.endswith(GENERATED_SUFFIXES):
        return True
    head = "\n".join(text.splitlines()[:GENERATED_MARKER_LINES])
    return bool(GENERATED_MARKER.search(head))


def header_problem(data):
    """Return None when the header is correct, else a short description."""
    if data.startswith(UTF8_BOM):
        data = data[len(UTF8_BOM):]
    text = data.decode("utf-8", errors="replace")
    lines = text.split("\n", 2)
    first = lines[0].rstrip("\r")
    if not HEADER_RE.fullmatch(first):
        return "first line is {!r}".format(first[:80]) if first else "first line is empty"
    second = lines[1].rstrip("\r") if len(lines) > 1 else ""
    if second == PORTIONS_LINE:
        return None
    if second.strip():
        return "no blank line after the header"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--plugin-dir", action="append", dest="plugin_dirs",
                        help="plugin root to check; repeatable (default: Open3DBroadcast and Open3DBroadcastWebRTC)")
    parser.add_argument("--allowlist", default=DEFAULT_ALLOWLIST)
    parser.add_argument("-v", "--verbose", action="store_true", help="list every skipped file")
    args = parser.parse_args()
    plugin_dirs = [os.path.abspath(d) for d in (args.plugin_dirs or DEFAULT_PLUGIN_DIRS)]

    try:
        allowlist = read_allowlist(args.allowlist)
        per_plugin = []
        for plugin_dir in plugin_dirs:
            files = tracked_sources(plugin_dir)
            if not files:
                raise InputError("git tracks no .h/.cpp/.cs files under {}/Source".format(plugin_dir))
            per_plugin.append((plugin_dir, files))
    except InputError as e:
        print("error: {}".format(e), file=sys.stderr)
        return 2

    in_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    failed = False
    all_files = set()
    for plugin_dir, files in per_plugin:
        all_files.update(files)
        if not check_plugin(plugin_dir, files, allowlist, args.verbose, in_actions):
            failed = True

    # An allowlist entry is relative to a plugin root; it is stale when no checked plugin tracks it.
    stale = sorted(set(allowlist) - all_files)
    for rel in stale:
        print("allowlist entry for a file git does not track: {}".format(rel))
    if failed or stale:
        return 1
    print("OK")
    return 0


def check_plugin(plugin_dir, files, allowlist, verbose, in_actions):
    """Checks one plugin and prints its report. Returns True when every checked file passes."""
    checked = 0
    skipped = {"ThirdParty": [], "generated": [], "allowlist": []}
    offenders = []
    for rel in files:
        if is_third_party(rel):
            skipped["ThirdParty"].append(rel)
            continue
        if rel in allowlist:
            skipped["allowlist"].append("{} ({})".format(rel, allowlist[rel]))
            continue
        with open(os.path.join(plugin_dir, rel), "rb") as f:
            data = f.read()
        if is_generated(rel, data.decode("utf-8", errors="replace")):
            skipped["generated"].append(rel)
            continue
        checked += 1
        problem = header_problem(data)
        if problem:
            offenders.append((rel, problem))

    plugin_rel = os.path.relpath(plugin_dir, REPO_ROOT).replace(os.sep, "/")
    print("Copyright header check ({}): {} checked, {} ThirdParty, {} generated, {} allowlisted.".format(
        plugin_rel, checked, len(skipped["ThirdParty"]), len(skipped["generated"]), len(skipped["allowlist"])))
    for kind in ("generated", "allowlist") + (("ThirdParty",) if verbose else ()):
        for rel in skipped[kind]:
            print("  skipped ({}): {}".format(kind, rel))

    if offenders:
        print("")
        print("{} file(s) do not start with the header line".format(len(offenders)))
        print("  {}".format(HEADER_EXAMPLE))
        print("(any four-digit year) followed by a blank line (or, on files from Open3DStream, {!r}):".format(PORTIONS_LINE))
        for rel, problem in offenders:
            print("  {}/{}: {}".format(plugin_rel, rel, problem))
            if in_actions:
                print("::error file={}/{},line=1::Missing copyright header ({})".format(plugin_rel, rel, problem))
    return not offenders


if __name__ == "__main__":
    sys.exit(main())
