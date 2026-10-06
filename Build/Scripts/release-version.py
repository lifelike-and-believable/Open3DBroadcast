#!/usr/bin/env python3
"""Release version checks and stamping for the Open3DBroadcast release workflow.

WP-R2 (mid-project review BC-4, BC-5). A release is made from a tag
open3dbroadcast-vX.Y.Z; this script turns the tag into the plugin version and
checks what the release needs before the UE runner is used.

  check  Validates the tag (or --version X.Y.Z) and prints version=X.Y.Z and
         version_code=<X*10000 + Y*100 + Z>, the integer .uplugin Version,
         also appended to --github-output. Requires exactly one CHANGELOG
         heading "## [X.Y.Z]" (a " - date" may follow) whose section names the
         versions the release carries: the wire protocol ("protocol 2"), the
         transport API ("transport API 5") and the core library
         ("core 1.1.0"). --notes-out writes that section, without its
         heading, for the release notes. --allow-missing-section reports a
         missing or incomplete section as a warning instead (dry runs of a
         version not yet in CHANGELOG).
  stamp  Writes VersionName "X.Y.Z" and Version <code> into each .uplugin
         given. The edit is textual: only the two values change, so the file
         keeps its layout, encoding and line endings. Each key must appear
         exactly once.

Y and Z must be 0..99 so the integer is unique and grows with the version.

Python 3.8 or later, standard library only. Exit codes: 0 ok; 1 a check
failed; 2 bad input (unreadable file, bad arguments). --self-test runs both
commands against generated files and exits 0 only if every case behaves.
"""

import argparse
import os
import re
import sys
import tempfile

TAG_PREFIX = "open3dbroadcast-v"
NUMBER = r"(0|[1-9][0-9]*)"
VERSION_RE = re.compile(rf"^{NUMBER}\.{NUMBER}\.{NUMBER}$")
# What a release section must name (BC-5: nothing mapped plugin versions to the wire and API versions).
CARRIED_VERSIONS = (
    ("the wire protocol, e.g. \"protocol 2\"", re.compile(r"\bprotocol\s+\d+", re.I)),
    ("the transport API, e.g. \"transport API 5\"", re.compile(r"\btransport\s+API\s+\d+", re.I)),
    ("the core library, e.g. \"core 1.1.0\"", re.compile(r"\bcore(?:\s+library)?\s+\d+\.\d+\.\d+", re.I)),
)


class InputError(Exception):
    pass


def parse_version(tag=None, version=None):
    """Returns (X, Y, Z) from a tag or a bare version; raises ValueError with the reason."""
    if tag is not None:
        if not tag.startswith(TAG_PREFIX):
            raise ValueError(f"tag '{tag}' does not start with '{TAG_PREFIX}'")
        version = tag[len(TAG_PREFIX):]
    m = VERSION_RE.match(version or "")
    if not m:
        raise ValueError(f"'{version}' is not X.Y.Z (digits, no leading zeros)")
    x, y, z = (int(g) for g in m.groups())
    if y > 99 or z > 99:
        raise ValueError(f"'{version}': minor and patch must be 0..99 so Version = X*10000 + Y*100 + Z stays unique")
    return x, y, z


def version_code(x, y, z):
    return x * 10000 + y * 100 + z


def changelog_section(text, version):
    """Returns (body, errors): the section under "## [version]" and what is wrong with it."""
    heading = re.compile(rf"^## \[{re.escape(version)}\](\s+-\s+\S.*)?\s*$")
    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if heading.match(line)]
    if not starts:
        return "", [f"CHANGELOG.md has no '## [{version}]' section"]
    if len(starts) > 1:
        return "", [f"CHANGELOG.md has {len(starts)} '## [{version}]' sections"]
    body = []
    for line in lines[starts[0] + 1:]:
        if line.startswith("## "):
            break
        body.append(line)
    body_text = "\n".join(body).strip()
    if not body_text:
        return "", [f"the '## [{version}]' section is empty"]
    errors = [f"the '## [{version}]' section does not name {what}" for what, rx in CARRIED_VERSIONS if not rx.search(body_text)]
    return body_text + "\n", errors


def stamp_text(text, version, code):
    """Returns text with VersionName and Version replaced; raises InputError unless each occurs once."""
    for key, rx in (("VersionName", r'("VersionName"\s*:\s*)"[^"]*"'), ("Version", r'("Version"\s*:\s*)\d+')):
        count = len(re.findall(rx, text))
        if count != 1:
            raise InputError(f'"{key}" appears {count} times; expected exactly once')
    text = re.sub(r'("VersionName"\s*:\s*)"[^"]*"', lambda m: f'{m.group(1)}"{version}"', text)
    return re.sub(r'("Version"\s*:\s*)\d+', lambda m: f"{m.group(1)}{code}", text)


def read_text(path):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            return f.read()
    except OSError as e:
        raise InputError(f"cannot read {path}: {e}")


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def cmd_check(args):
    try:
        x, y, z = parse_version(args.tag, args.version)
    except ValueError as e:
        print(f"::error::{e}")
        return 1
    version = f"{x}.{y}.{z}"
    code = version_code(x, y, z)
    body, errors = changelog_section(read_text(args.changelog), version)
    for error in errors:
        print(f"::{'warning' if args.allow_missing_section else 'error'}::{error}")
    if errors and not args.allow_missing_section:
        return 1
    outputs = f"version={version}\nversion_code={code}\n"
    print(outputs, end="")
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as f:
            f.write(outputs)
    if args.notes_out:
        write_text(args.notes_out, body if not errors else f"Dry run: CHANGELOG.md has no complete '## [{version}]' section.\n")
    return 0


def cmd_stamp(args):
    try:
        x, y, z = parse_version(version=args.version)
    except ValueError as e:
        print(f"::error::{e}")
        return 1
    code = version_code(x, y, z)
    for path in args.uplugin:
        try:
            write_text(path, stamp_text(read_text(path), args.version, code))
        except InputError as e:
            raise InputError(f"{path}: {e}")
        print(f"{path}: VersionName {args.version}, Version {code}")
    return 0


def self_test():
    failures = []

    def expect(name, condition):
        if not condition:
            failures.append(name)

    for tag, ok in (("open3dbroadcast-v1.2.3", True), ("open3dbroadcast-v0.10.0", True), ("open3dbroadcast-v1.2", False),
                    ("open3dbroadcast-v1.02.3", False), ("open3dbroadcast-v1.100.0", False), ("v1.2.3", False),
                    ("open3dbroadcast-v1.2.3-rc1", False)):
        try:
            parse_version(tag=tag)
            expect(f"tag {tag} accepted", ok)
        except ValueError:
            expect(f"tag {tag} rejected", not ok)
    expect("version code", version_code(1, 2, 3) == 10203 and version_code(0, 99, 99) < version_code(1, 0, 0))

    carried = "Protocol 2, transport API 5, core 1.1.0.\n"
    good = f"# Changelog\n\n## Unreleased\n\n- next\n\n## [1.2.3] - 2026-10-07\n\n- thing\n\n{carried}\n## [1.2.2]\n\n- old\n"
    body, errors = changelog_section(good, "1.2.3")
    expect("dated section found", not errors and "- thing" in body and "- old" not in body and "## [1.2.3]" not in body)
    expect("undated section found", not changelog_section("## [2.0.0]\n\n- x\n" + carried, "2.0.0")[1])
    expect("missing section", changelog_section(good, "9.9.9")[1] == ["CHANGELOG.md has no '## [9.9.9]' section"])
    expect("duplicate section", "2 '## [1.2.3]' sections" in changelog_section(good + "## [1.2.3]\n- again\n", "1.2.3")[1][0])
    expect("empty section", "is empty" in changelog_section("## [1.0.0]\n\n## [0.9.0]\n- x\n", "1.0.0")[1][0])
    expect("prefix is not the version", changelog_section("## [1.2.30]\n- x\n" + carried, "1.2.3")[1] != [])
    expect("carried versions required", len(changelog_section("## [1.0.0]\n\n- only notes\n", "1.0.0")[1]) == 3)

    uplugin = '﻿{\r\n\t"FileVersion": 3,\r\n\t"EngineVersion": "5.7.0",\r\n\t"Version": 1,\r\n\t"VersionName": "1.0",\r\n\t"FriendlyName": "A & B"\r\n}\r\n'
    stamped = stamp_text(uplugin, "1.2.3", 10203)
    expect("stamp edits only the two values",
           stamped == uplugin.replace('"Version": 1,', '"Version": 10203,').replace('"VersionName": "1.0"', '"VersionName": "1.2.3"'))
    for name, text in (("missing Version", uplugin.replace('\t"Version": 1,\r\n', "")),
                       ("two VersionName", uplugin.replace('"FriendlyName"', '"VersionName": "x",\r\n\t"FriendlyName"'))):
        try:
            stamp_text(text, "1.2.3", 10203)
            expect(f"stamp refuses {name}", False)
        except InputError:
            pass

    with tempfile.TemporaryDirectory() as tmp:
        changelog = os.path.join(tmp, "CHANGELOG.md")
        write_text(changelog, good)
        notes = os.path.join(tmp, "notes.md")
        output = os.path.join(tmp, "output.txt")
        parser = build_parser()
        rc = cmd_check(parser.parse_args(["check", "--tag", "open3dbroadcast-v1.2.3", "--changelog", changelog,
                                          "--notes-out", notes, "--github-output", output]))
        expect("check passes a released version", rc == 0 and "version_code=10203" in read_text(output) and "- thing" in read_text(notes))
        expect("check fails a missing section",
               cmd_check(parser.parse_args(["check", "--version", "9.9.9", "--changelog", changelog])) == 1)
        expect("dry run tolerates a missing section",
               cmd_check(parser.parse_args(["check", "--version", "9.9.9", "--changelog", changelog, "--allow-missing-section"])) == 0)
        path = os.path.join(tmp, "A.uplugin")
        write_text(path, uplugin)
        expect("stamp writes the file", cmd_stamp(parser.parse_args(["stamp", "--version", "1.2.3", path])) == 0
               and read_text(path) == stamped)

    for name in failures:
        print(f"self-test FAILED: {name}")
    print("self-test passed" if not failures else f"self-test: {len(failures)} failure(s)")
    return 0 if not failures else 1


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true", help="run the built-in tests and exit")
    sub = parser.add_subparsers(dest="command")
    check = sub.add_parser("check", help="validate the tag and the CHANGELOG section")
    which = check.add_mutually_exclusive_group(required=True)
    which.add_argument("--tag", help="release tag, open3dbroadcast-vX.Y.Z")
    which.add_argument("--version", help="bare version, X.Y.Z")
    check.add_argument("--changelog", default="CHANGELOG.md")
    check.add_argument("--notes-out", help="write the version's CHANGELOG section here")
    check.add_argument("--github-output", help="append version= and version_code= to this file")
    check.add_argument("--allow-missing-section", action="store_true", help="warn instead of failing (dry runs)")
    stamp = sub.add_parser("stamp", help="write the version into .uplugin files")
    stamp.add_argument("--version", required=True, help="X.Y.Z")
    stamp.add_argument("uplugin", nargs="+")
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.command is None:
        parser.error("a command is required (check or stamp), or --self-test")
    try:
        return cmd_check(args) if args.command == "check" else cmd_stamp(args)
    except InputError as e:
        print(f"::error::{e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
