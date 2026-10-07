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
  archives
         The Publish job's files (ADR 0014: one zip per engine for each
         plugin). Reads the tested packages the release's build-and-test
         matrix uploaded, one folder per engine, and checks that each carries
         --version, that the add-on was built for the same engine, and that
         there is exactly one package for each engine in --engines. Writes
         Open3DBroadcast-Plugin-X.Y.Z-UE<engine>-Win64.zip (laid out as
         UE_<engine>/Plugins/Open3DBroadcast), the add-on's zip the same way,
         and release_notes.md (the CHANGELOG section and install steps).

Y and Z must be 0..99 so the integer is unique and grows with the version.

Python 3.8 or later, standard library only. Exit codes: 0 ok; 1 a check
failed; 2 bad input (unreadable file, bad arguments). --self-test runs both
commands against generated files and exits 0 only if every case behaves.
"""

import argparse
import json
import os
import re
import sys
import tempfile
import zipfile

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


def read_descriptor(path):
    """Returns (engine X.Y, VersionName) from a packaged .uplugin."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            d = json.load(f)
        return ".".join(str(d["EngineVersion"]).split(".")[:2]), d["VersionName"]
    except (OSError, ValueError, KeyError) as e:
        raise InputError(f"cannot read the engine and version from {path}: {e!r}")


def zip_tree(source, zip_path, prefix):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(source):
            for name in sorted(files):
                full = os.path.join(root, name)
                z.write(full, prefix + "/" + os.path.relpath(full, source).replace(os.sep, "/"))


def cmd_archives(args):
    engines = args.engines.split()
    prefix = f"Open3DBroadcast-Win64-{args.sha}"
    errors, found = [], {}
    for artifact in sorted(os.listdir(args.plugin_packages)):
        if not artifact.startswith(prefix):
            continue
        suffix = artifact[len(prefix):]
        plugin = os.path.join(args.plugin_packages, artifact)
        addon = os.path.join(args.addon_packages, f"Open3DBroadcastWebRTC-Win64-{args.sha}{suffix}", "Open3DBroadcastWebRTC")
        engine, version = read_descriptor(os.path.join(plugin, "Open3DBroadcast.uplugin"))
        addon_engine, addon_version = read_descriptor(os.path.join(addon, "Open3DBroadcastWebRTC.uplugin"))
        # The packages are what the tests ran against, so they must carry the release's version.
        if version != args.version or addon_version != args.version:
            errors.append(f"{artifact}: the packages have VersionName '{version}' and '{addon_version}', not '{args.version}'")
        if addon_engine != engine:
            errors.append(f"{artifact}: the add-on was built for UE {addon_engine}, the plugin for UE {engine}")
        if engine not in engines:
            errors.append(f"{artifact}: UE {engine} is not one of the released engines ({args.engines})")
        elif engine in found:
            errors.append(f"{artifact}: a second package for UE {engine}")
        found[engine] = (plugin, addon)
    errors += [f"no tested package for UE {engine}" for engine in engines if engine not in found]
    if errors:
        for error in errors:
            print(f"::error::{error}")
        return 1

    os.makedirs(args.out_dir, exist_ok=True)
    plugin_zips, addon_zips = [], []
    for engine in engines:
        plugin, addon = found[engine]
        plugin_zips.append(f"Open3DBroadcast-Plugin-{args.version}-UE{engine}-Win64.zip")
        addon_zips.append(f"Open3DBroadcastWebRTC-Plugin-{args.version}-UE{engine}-Win64.zip")
        zip_tree(plugin, os.path.join(args.out_dir, plugin_zips[-1]), f"UE_{engine}/Plugins/Open3DBroadcast")
        zip_tree(addon, os.path.join(args.out_dir, addon_zips[-1]), f"UE_{engine}/Plugins/Open3DBroadcastWebRTC")
        print(f"UE {engine}: {plugin_zips[-1]}, {addon_zips[-1]}")

    listed = " and ".join(f"Unreal Engine {engine}" for engine in engines)
    notes = [read_text(args.notes).rstrip("\n"), "", "### Installation", "",
             "1. Download the zip for your engine:"]
    notes += [f"   - Unreal Engine {engine}: {name}" for engine, name in zip(engines, plugin_zips)]
    notes += ["2. Copy its `UE_<engine>/Plugins/Open3DBroadcast` folder into your project's `Plugins` folder.",
              "3. Restart Unreal Editor and enable **Open3DBroadcast**.", "",
              f"{listed}, Windows (Win64) only. This is the GitHub build, not the Fab package."]
    if args.publish_addon == "true":
        notes += ["", "WebRTC (LiveKit) is the separate add-on, one zip per engine: " + ", ".join(addon_zips) + ". "
                  "Copy its `UE_<engine>/Plugins/Open3DBroadcastWebRTC` folder next to Open3DBroadcast."]
    write_text(os.path.join(args.out_dir, "release_notes.md"), "\n".join(notes) + "\n")
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

    # archives: the layout the release's download steps produce, one artifact folder per engine.
    sha = "abc123"

    def package(root, artifact, plugin, engine, version):
        folder = os.path.join(root, artifact, *([plugin] if plugin.endswith("WebRTC") else []))
        os.makedirs(os.path.join(folder, "Binaries", "Win64"))
        write_text(os.path.join(folder, f"{plugin}.uplugin"),
                   json.dumps({"EngineVersion": f"{engine}.0", "VersionName": version}))
        write_text(os.path.join(folder, "Binaries", "Win64", "a.dll"), engine)

    def archives(tmp, engines="5.7 5.8", plugin_versions=None, addon_engines=None, publish_addon="false"):
        plugins, addons = os.path.join(tmp, "Plugins"), os.path.join(tmp, "AddOns")
        for i, (engine, suffix) in enumerate((("5.7", ""), ("5.8", "-UE5.8"))):
            package(plugins, f"Open3DBroadcast-Win64-{sha}{suffix}", "Open3DBroadcast", engine,
                    (plugin_versions or ["1.2.3", "1.2.3"])[i])
            package(addons, f"Open3DBroadcastWebRTC-Win64-{sha}{suffix}", "Open3DBroadcastWebRTC",
                    (addon_engines or ["5.7", "5.8"])[i], "1.2.3")
        notes = os.path.join(tmp, "notes.md")
        write_text(notes, "- thing\n")
        out = os.path.join(tmp, "Release")
        rc = cmd_archives(build_parser().parse_args(
            ["archives", "--version", "1.2.3", "--engines", engines, "--sha", sha, "--plugin-packages", plugins,
             "--addon-packages", addons, "--notes", notes, "--publish-addon", publish_addon, "--out-dir", out]))
        return rc, out

    with tempfile.TemporaryDirectory() as tmp:
        rc, out = archives(tmp)
        names = sorted(os.listdir(out)) if os.path.isdir(out) else []
        expect("archives: a zip per engine for each plugin, and the notes", rc == 0 and names == [
            "Open3DBroadcast-Plugin-1.2.3-UE5.7-Win64.zip", "Open3DBroadcast-Plugin-1.2.3-UE5.8-Win64.zip",
            "Open3DBroadcastWebRTC-Plugin-1.2.3-UE5.7-Win64.zip", "Open3DBroadcastWebRTC-Plugin-1.2.3-UE5.8-Win64.zip",
            "release_notes.md"])
        if rc == 0 and len(names) == 5:
            with zipfile.ZipFile(os.path.join(out, names[1])) as z:
                expect("archives: the 5.8 zip holds the 5.8 package under UE_5.8/Plugins",
                       z.read("UE_5.8/Plugins/Open3DBroadcast/Binaries/Win64/a.dll") == b"5.8")
            with zipfile.ZipFile(os.path.join(out, names[2])) as z:
                expect("archives: the add-on zip is laid out the same way",
                       "UE_5.7/Plugins/Open3DBroadcastWebRTC/Open3DBroadcastWebRTC.uplugin" in z.namelist())
            notes = read_text(os.path.join(out, "release_notes.md"))
            expect("archives: the notes keep the section and name each engine's zip",
                   notes.startswith("- thing") and "Open3DBroadcast-Plugin-1.2.3-UE5.7-Win64.zip" in notes
                   and "Open3DBroadcast-Plugin-1.2.3-UE5.8-Win64.zip" in notes and "WebRTC" not in notes)
    with tempfile.TemporaryDirectory() as tmp:
        rc, out = archives(tmp, publish_addon="true")
        notes = os.path.join(out, "release_notes.md")
        expect("archives: the notes name the add-on when it is published", rc == 0 and os.path.isfile(notes)
               and "Open3DBroadcastWebRTC-Plugin-1.2.3-UE5.8-Win64.zip" in read_text(notes))
    for name, kwargs in (("a package with another version", {"plugin_versions": ["1.2.3", "1.2.2"]}),
                         ("an add-on built for another engine", {"addon_engines": ["5.7", "5.7"]}),
                         ("an engine with no package", {"engines": "5.7 5.8 5.9"}),
                         ("a package for an engine not released", {"engines": "5.7"})):
        with tempfile.TemporaryDirectory() as tmp:
            expect(f"archives refuses {name}", archives(tmp, **kwargs)[0] == 1)

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
    arch = sub.add_parser("archives", help="the release's per-engine zips and notes")
    arch.add_argument("--version", required=True, help="X.Y.Z")
    arch.add_argument("--engines", required=True, help="the released engines, space-separated, e.g. '5.7 5.8'")
    arch.add_argument("--sha", required=True, help="the commit the artifact names carry")
    arch.add_argument("--plugin-packages", required=True, help="folder of Open3DBroadcast-Win64-<sha>* artifacts")
    arch.add_argument("--addon-packages", required=True, help="folder of Open3DBroadcastWebRTC-Win64-<sha>* artifacts")
    arch.add_argument("--notes", required=True, help="the CHANGELOG section (check --notes-out)")
    arch.add_argument("--publish-addon", choices=("true", "false"), default="false")
    arch.add_argument("--out-dir", required=True)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.command is None:
        parser.error("a command is required (check, stamp or archives), or --self-test")
    try:
        return {"check": cmd_check, "stamp": cmd_stamp, "archives": cmd_archives}[args.command](args)
    except InputError as e:
        print(f"::error::{e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
