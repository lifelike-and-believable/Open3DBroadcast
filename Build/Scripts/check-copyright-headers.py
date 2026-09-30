#!/usr/bin/env python3
"""Check that every first-party plugin source file starts with the copyright header.

The rule (FAB-5, FAB-9, WP-F4): every git-tracked .h, .cpp and .cs file under
ProjectSandbox/Plugins/Open3DBroadcast/Source/ starts with exactly

    // Copyright Lifelike & Believable. All Rights Reserved.

followed by a blank line (or the end of the file). A leading UTF-8 BOM is
ignored. Line endings may be LF or CRLF.

Not checked:
  - anything under a ThirdParty/ directory (vendored libraries keep their own
    notices);
  - generated files: names ending in .generated.h, _generated.h or .gen.cpp,
    or a file whose first lines say it is generated ("@generated",
    "automatically generated", "auto-generated", "DO NOT EDIT");
  - files listed in Build/Fab/copyright-allowlist.txt (genuine third-party
    code outside a ThirdParty/ directory). Each entry needs a reason, and an
    entry for a file git does not track is an error.

Python 3.8 or later, standard library only. Exit codes: 0 every checked file
has the header; 1 at least one file is missing it, or the allowlist has a
stale entry; 2 bad input (not a git checkout, unreadable allowlist).
"""

import argparse
import os
import re
import subprocess
import sys

HEADER = "// Copyright Lifelike & Believable. All Rights Reserved."

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_PLUGIN_DIR = os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcast")
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
    if first != HEADER:
        return "first line is {!r}".format(first[:80]) if first else "first line is empty"
    if len(lines) > 1 and lines[1].rstrip("\r").strip():
        return "no blank line after the header"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--plugin-dir", default=DEFAULT_PLUGIN_DIR)
    parser.add_argument("--allowlist", default=DEFAULT_ALLOWLIST)
    parser.add_argument("-v", "--verbose", action="store_true", help="list every skipped file")
    args = parser.parse_args()

    try:
        files = tracked_sources(args.plugin_dir)
        allowlist = read_allowlist(args.allowlist)
    except InputError as e:
        print("error: {}".format(e), file=sys.stderr)
        return 2
    if not files:
        print("error: git tracks no .h/.cpp/.cs files under {}/Source".format(args.plugin_dir), file=sys.stderr)
        return 2

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
        with open(os.path.join(args.plugin_dir, rel), "rb") as f:
            data = f.read()
        if is_generated(rel, data.decode("utf-8", errors="replace")):
            skipped["generated"].append(rel)
            continue
        checked += 1
        problem = header_problem(data)
        if problem:
            offenders.append((rel, problem))

    stale = sorted(set(allowlist) - set(files))

    print("Copyright header check: {} checked, {} ThirdParty, {} generated, {} allowlisted.".format(
        checked, len(skipped["ThirdParty"]), len(skipped["generated"]), len(skipped["allowlist"])))
    for kind in ("generated", "allowlist") + (("ThirdParty",) if args.verbose else ()):
        for rel in skipped[kind]:
            print("  skipped ({}): {}".format(kind, rel))

    in_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    plugin_rel = os.path.relpath(args.plugin_dir, REPO_ROOT).replace(os.sep, "/")
    for rel in stale:
        print("allowlist entry for a file git does not track: {}".format(rel))
    if offenders:
        print("")
        print("{} file(s) do not start with the header line".format(len(offenders)))
        print("  {}".format(HEADER))
        print("followed by a blank line:")
        for rel, problem in offenders:
            print("  {}: {}".format(rel, problem))
            if in_actions:
                print("::error file={}/{},line=1::Missing copyright header ({})".format(plugin_rel, rel, problem))
    if offenders or stale:
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
