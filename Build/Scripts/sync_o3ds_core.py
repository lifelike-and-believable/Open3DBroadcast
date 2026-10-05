#!/usr/bin/env python3
"""Mirror the o3ds core sources that the Unreal plugin compiles into the plugin tree.

docs/adr/0003-core-library-delivery-to-plugin.md (WP-F1): the plugin compiles the
part of src/o3ds it uses as its own module, Open3DStreamCore, so a clean clone
and the Fab source zip build with RunUAT BuildPlugin alone. src/o3ds stays the
single source of truth; this script generates the plugin's copy and, with
--check, proves in CI that the copy matches.

What it writes (everything is generated; never edit these files by hand):

  <plugin>/Source/ThirdParty/Open3DStreamCore/
      o3ds/...              the include closure of the seed headers listed in
                            Build/o3ds-core-manifest.txt, plus the .cpp next to
                            each header in the closure (byte-for-byte copies)
      o3ds_generated.h      copies of the committed src/o3ds_generated.h and
      o3ds_control_generated.h  src/o3ds_control_generated.h (GENERATED_HEADERS)
      flatbuffers/...       the FlatBuffers runtime headers that
                            o3ds_generated.h needs, from thirdparty/flatbuffers
      crccpp/CRC.h          from thirdparty/crccpp, only while a core file
                            includes it (none does since WP-A2e)
      LICENSES/             Open3DStream (MIT), FlatBuffers (Apache-2.0),
                            CRCpp (BSD-3-Clause, only with CRC.h)
      SYNC_STAMP.txt        manifest and content hashes, submodule pins,
                            FlatBuffers version and O3DS_VERSION_TAG
  <plugin>/Source/Open3DStreamCore/Private/Core/O3DSCore_*.cpp
                            one small translation unit per mirrored .cpp; it
                            includes the mirrored file between
                            O3DSCoreSourceBegin.h and O3DSCoreSourceEnd.h

The mirror is outside the Open3DStreamCore module folder on purpose: UnrealBuildTool
compiles every .cpp under a module folder, and the core sources must be compiled
through the wrappers, which switch compiler warnings off for them (the core has
its own warning checks in core-tests.yml).

Checks, in both modes (any failure exits 1):
  - a quoted #include in a core file must resolve inside src/o3ds, to a
    generated header in GENERATED_HEADERS, to the FlatBuffers headers or to
    CRC.h;
  - an angle-bracket #include in a core file must be a standard header
    (no platform headers such as <windows.h>);
  - core files in the closure use no exceptions or RTTI (the module is built
    with both off);
  - every o3ds/FlatBuffers header that a plugin source file includes is in the
    mirror;
  - the thirdparty/flatbuffers and thirdparty/crccpp checkouts are the commits
    the repository pins.

Usage:
  python3 Build/Scripts/sync_o3ds_core.py           rewrite the mirror and wrappers
  python3 Build/Scripts/sync_o3ds_core.py --check   compare only; exit 1 on any
                                                     difference, missing or extra file

Requires the flatbuffers and crccpp submodules:
  git submodule update --init thirdparty/flatbuffers thirdparty/crccpp

Python 3.8 or later, standard library only. Exit codes: 0 in sync (or written);
1 out of sync or a check failed; 2 bad input (missing submodule, manifest or file).
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC_DIR = os.path.join(REPO_ROOT, "src")
FLATBUFFERS_DIR = os.path.join(REPO_ROOT, "thirdparty", "flatbuffers")
CRCPP_DIR = os.path.join(REPO_ROOT, "thirdparty", "crccpp")
DEFAULT_MANIFEST = os.path.join(REPO_ROOT, "Build", "o3ds-core-manifest.txt")
DEFAULT_PLUGIN_DIR = os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcast")
# Plugins that compile against Open3DBroadcast's core mirror without holding one (the WebRTC
# add-on, WP-F11). Their core includes are checked against the mirror too.
DEPENDENT_PLUGIN_DIRS = [os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcastWebRTC")]

# Relative to the plugin folder.
MIRROR_REL = "Source/ThirdParty/Open3DStreamCore"
WRAPPER_REL = "Source/Open3DStreamCore/Private/Core"
WRAPPER_PREFIX = "O3DSCore_"

# Include roots used to resolve a non-relative #include, with the folder each
# one maps to inside the mirror.
INCLUDE_ROOTS = [
    (SRC_DIR, ""),
    (os.path.join(FLATBUFFERS_DIR, "include"), ""),
    (os.path.join(CRCPP_DIR, "inc"), "crccpp/"),
]

# (source, mirror path, mirror prefix that must hold a file for the licence to
# be copied; None means always). Since WP-A2e no core file includes CRC.h
# (o3ds/crc32.cpp computes the CRC), so the CRCpp licence goes with it.
LICENSES = [
    (os.path.join(REPO_ROOT, "LICENSE"), "LICENSES/Open3DStream_LICENSE.txt", None),
    (os.path.join(FLATBUFFERS_DIR, "LICENSE.txt"), "LICENSES/FlatBuffers_LICENSE.txt", None),
    (os.path.join(CRCPP_DIR, "LICENSE"), "LICENSES/CRCpp_LICENSE.txt", "crccpp/"),
]

# C headers a core file may include with angle brackets. C++ standard headers
# (no extension) are always allowed.
C_STANDARD_HEADERS = {
    "assert.h", "ctype.h", "errno.h", "float.h", "inttypes.h", "limits.h", "math.h",
    "stdarg.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h", "time.h",
}

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.M)
FORBIDDEN_CONSTRUCTS = re.compile(r"\bthrow\b|\btry\s*\{|\bcatch\s*\(|\bdynamic_cast\b|\btypeid\b")
COMMENT_OR_STRING_RE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)

# flatc output committed under src/ that core files may include and the mirror
# carries (o3ds.fbs, and o3ds_control.fbs from docs/adr/0011).
GENERATED_HEADERS = ("o3ds_generated.h", "o3ds_control_generated.h")

GENERATED_NOTE = "Generated by Build/Scripts/sync_o3ds_core.py from src/ and Build/o3ds-core-manifest.txt; do not edit."


class InputError(Exception):
    pass


def rel_to_repo(path):
    return os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise InputError(f"cannot read {rel_to_repo(path)}: {e}")


def normalise(data):
    """Content compared with CRLF folded to LF (there is no .gitattributes)."""
    return data.replace(b"\r\n", b"\n")


def read_manifest(path):
    if not os.path.isfile(path):
        raise InputError(f"manifest not found: {path}")
    seeds = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                seeds.append(line.replace("\\", "/"))
    if not seeds:
        raise InputError(f"manifest {path} lists no headers")
    return seeds


def mirror_path_for(source):
    """Mirror-relative path for a source file, or None if it is outside the allowed set."""
    source = os.path.abspath(source)
    o3ds_dir = os.path.join(SRC_DIR, "o3ds") + os.sep
    if source.startswith(o3ds_dir) or source in [os.path.join(SRC_DIR, h) for h in GENERATED_HEADERS]:
        return os.path.relpath(source, SRC_DIR).replace(os.sep, "/")
    for root, prefix in INCLUDE_ROOTS[1:]:
        if source.startswith(root + os.sep):
            return prefix + os.path.relpath(source, root).replace(os.sep, "/")
    return None


def is_core_file(source):
    return mirror_path_for(source) is not None and os.path.abspath(source).startswith(SRC_DIR + os.sep)


def resolve(including_file, kind, name):
    candidates = []
    if kind == '"':
        candidates.append(os.path.join(os.path.dirname(including_file), name))
    candidates += [os.path.join(root, name) for root, _ in INCLUDE_ROOTS]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.normpath(c)
    return None


def strip_comments_and_strings(text):
    """Blank out comments and literals, keeping the newlines so line numbers stay right."""
    return COMMENT_OR_STRING_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def compute_closure(seeds, errors):
    """Return the sorted list of absolute source paths in the include closure."""
    pending = []
    for seed in seeds:
        path = os.path.normpath(os.path.join(SRC_DIR, seed))
        if not os.path.isfile(path):
            raise InputError(f"manifest header src/{seed} does not exist")
        pending.append(path)

    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        core = is_core_file(path)
        if core and path.endswith(".h"):
            cpp = path[:-2] + ".cpp"
            if os.path.isfile(cpp):
                pending.append(cpp)
        text = read_bytes(path).decode("utf-8", errors="replace")
        for kind, name in INCLUDE_RE.findall(text):
            target = resolve(path, kind, name)
            if target is not None:
                if mirror_path_for(target) is None:
                    errors.append(f"{rel_to_repo(path)} includes {kind}{name}, which resolves to "
                                  f"{rel_to_repo(target)}, outside the files the plugin may mirror")
                    continue
                pending.append(target)
                continue
            if not core:
                continue  # third-party headers: optional includes (absl, android) may not exist
            if kind == '"':
                errors.append(f"{rel_to_repo(path)}: #include \"{name}\" does not resolve inside src/o3ds, "
                              f"a generated header ({', '.join(GENERATED_HEADERS)}), FlatBuffers or CRCpp")
            elif "/" in name or ("." in name and name not in C_STANDARD_HEADERS):
                errors.append(f"{rel_to_repo(path)}: #include <{name}> is not a standard header; the core the "
                              "plugin compiles must stay platform-neutral (docs/adr/0003)")
        if core:
            code = strip_comments_and_strings(text)
            for m in FORBIDDEN_CONSTRUCTS.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                errors.append(f"{rel_to_repo(path)}:{line}: '{m.group(0).strip()}' - the Open3DStreamCore module "
                              "is built without exceptions and RTTI")
    return sorted(seen)


def git(*args):
    try:
        return subprocess.run(["git", "-C", REPO_ROOT] + list(args), check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as e:
        raise InputError(f"git {' '.join(args)} failed: {e}")


def submodule_pin(path, errors):
    """The commit the repository pins for a submodule; checks the checkout matches it."""
    rel = rel_to_repo(path)
    line = git("ls-files", "-s", "--", rel)
    parts = line.split()
    if len(parts) < 2 or parts[0] != "160000":
        raise InputError(f"{rel} is not a submodule in the git index")
    pinned = parts[1]
    if not os.path.isdir(path) or not os.listdir(path):
        raise InputError(f"{rel} is not checked out; run: git submodule update --init {rel}")
    try:
        head = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as e:
        raise InputError(f"cannot read the checked-out commit of {rel}: {e}")
    if head != pinned:
        errors.append(f"{rel} is checked out at {head[:12]}, but the repository pins {pinned[:12]}; "
                      f"run: git submodule update {rel}")
    return pinned


def flatbuffers_version():
    text = read_bytes(os.path.join(FLATBUFFERS_DIR, "include", "flatbuffers", "base.h")).decode("utf-8", "replace")
    parts = []
    for key in ("MAJOR", "MINOR", "REVISION"):
        m = re.search(r"#define FLATBUFFERS_VERSION_%s (\d+)" % key, text)
        if not m:
            raise InputError(f"FLATBUFFERS_VERSION_{key} not found in flatbuffers/base.h")
        parts.append(m.group(1))
    return ".".join(parts)


def o3ds_version_tag():
    text = read_bytes(os.path.join(REPO_ROOT, "CMakeLists.txt")).decode("utf-8", "replace")
    m = re.search(r'set\(\s*O3DS_VERSION_TAG\s+"([^"]+)"\s*\)', text)
    if not m:
        raise InputError("set(O3DS_VERSION_TAG ...) not found in CMakeLists.txt")
    return m.group(1)


def wrapper_name(mirror_rel):
    stem = mirror_rel[len("o3ds/"):-len(".cpp")].replace("/", "_")
    return f"{WRAPPER_PREFIX}{stem}.cpp"


def wrapper_text(mirror_rel):
    return (
        "// Copyright 2026 Lifelike & Believable. All Rights Reserved.\n"
        "\n"
        f"// {GENERATED_NOTE}\n"
        f"// Compiles src/{mirror_rel} from the mirror in {MIRROR_REL}\n"
        "// (docs/adr/0003-core-library-delivery-to-plugin.md).\n"
        "\n"
        "#include \"O3DSCoreSourceBegin.h\"\n"
        f"#include \"{mirror_rel}\"\n"
        "#include \"O3DSCoreSourceEnd.h\"\n"
    ).encode("utf-8")


def build_outputs(manifest_path, errors):
    """Return ({mirror-relative path: bytes}, {wrapper file name: bytes})."""
    seeds = read_manifest(manifest_path)
    flatbuffers_pin = submodule_pin(FLATBUFFERS_DIR, errors)
    crccpp_pin = submodule_pin(CRCPP_DIR, errors)
    closure = compute_closure(seeds, errors)

    mirror = {}
    for source in closure:
        mirror[mirror_path_for(source)] = read_bytes(source)
    for source, dest, needs_prefix in LICENSES:
        if needs_prefix is None or any(rel.startswith(needs_prefix) for rel in mirror):
            mirror[dest] = read_bytes(source)

    wrappers = {}
    for rel in sorted(mirror):
        if rel.startswith("o3ds/") and rel.endswith(".cpp"):
            wrappers[wrapper_name(rel)] = wrapper_text(rel)

    content = hashlib.sha256()
    for rel in sorted(mirror):
        content.update(rel.encode("utf-8") + b"\0" + normalise(mirror[rel]) + b"\0")
    for name in sorted(wrappers):
        content.update(name.encode("utf-8") + b"\0" + wrappers[name] + b"\0")
    manifest_hash = hashlib.sha256(normalise(read_bytes(manifest_path))).hexdigest()

    core_files = [r for r in mirror if r.startswith("o3ds/")]
    stamp = "\n".join([
        f"# {GENERATED_NOTE}",
        "# Nothing in this folder is edited by hand: change src/ (or the submodule",
        "# pins) and run  python3 Build/Scripts/sync_o3ds_core.py  in the same PR.",
        "# core-tests.yml runs it with --check and fails when this folder, or the",
        f"# generated translation units in {WRAPPER_REL}, differ.",
        "# See docs/adr/0003-core-library-delivery-to-plugin.md.",
        f"o3ds-version-tag: {o3ds_version_tag()}",
        f"flatbuffers-version: {flatbuffers_version()}",
        f"flatbuffers-submodule: {flatbuffers_pin}",
        f"crccpp-submodule: {crccpp_pin}",
        f"manifest-sha256: {manifest_hash}",
        f"content-sha256: {content.hexdigest()}",
        f"core-files: {len(core_files)} ({len(wrappers)} .cpp)",
        f"mirrored-files: {len(mirror)}",
        "",
    ]).encode("utf-8")
    mirror["SYNC_STAMP.txt"] = stamp
    return mirror, wrappers


PLUGIN_INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]((?:o3ds/|flatbuffers/)[^>"]+|o3ds(?:_control)?_generated\.h)[>"]', re.M)


def check_plugin_includes(plugin_dir, mirror, errors):
    """Every core or FlatBuffers header a plugin source includes must be in the mirror."""
    source_dir = os.path.join(plugin_dir, "Source")
    skip = os.path.normpath(os.path.join(plugin_dir, MIRROR_REL))
    for dirpath, dirnames, filenames in os.walk(source_dir):
        if os.path.normpath(dirpath) == skip:
            dirnames[:] = []
            continue
        for name in filenames:
            if not name.endswith((".h", ".cpp")):
                continue
            path = os.path.join(dirpath, name)
            text = read_bytes(path).decode("utf-8", errors="replace")
            for inc in PLUGIN_INCLUDE_RE.findall(text):
                if inc not in mirror:
                    errors.append(f"{rel_to_repo(path)} includes \"{inc}\", which the mirror does not contain; "
                                  "add the header to Build/o3ds-core-manifest.txt and re-run the script")


def list_files(root):
    found = {}
    if not os.path.isdir(root):
        return found
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            found[os.path.relpath(path, root).replace(os.sep, "/")] = path
    return found


def compare(expected, root, label, keep=()):
    """Differences between the expected {rel: bytes} and the files under root.

    Names in keep are hand-written files that must exist next to the
    generated ones; their content is not compared.
    """
    problems = []
    actual = list_files(root)
    for rel in sorted(expected):
        if rel not in actual:
            problems.append(f"missing: {label}/{rel}")
        elif normalise(read_bytes(actual[rel])) != normalise(expected[rel]):
            problems.append(f"differs: {label}/{rel}")
    for rel in keep:
        if rel not in actual:
            problems.append(f"missing: {label}/{rel} (hand-written, not generated)")
    for rel in sorted(set(actual) - set(expected) - set(keep)):
        problems.append(f"extra (not generated): {label}/{rel}")
    return problems


def write_tree(expected, root, keep=()):
    """Make root hold exactly the expected files (plus the names in keep). Returns the changes."""
    changes = []
    actual = list_files(root)
    for rel in sorted(set(actual) - set(expected) - set(keep)):
        os.remove(actual[rel])
        changes.append(f"removed {rel}")
    for rel, data in sorted(expected.items()):
        path = os.path.join(root, *rel.split("/"))
        if rel in actual and read_bytes(path) == data:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        changes.append(("updated " if rel in actual else "added ") + rel)
    for dirpath, dirnames, filenames in os.walk(root, topdown=False):
        if dirpath != root and not os.listdir(dirpath):
            os.rmdir(dirpath)
    return changes


# Hand-written files that live next to the generated wrappers.
WRAPPER_DIR_KEEP = ("O3DSCoreSourceBegin.h", "O3DSCoreSourceEnd.h")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="compare only; exit 1 when anything differs")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--plugin-dir", default=DEFAULT_PLUGIN_DIR)
    args = ap.parse_args(argv)

    errors = []
    try:
        mirror, wrappers = build_outputs(os.path.abspath(args.manifest), errors)
        check_plugin_includes(os.path.abspath(args.plugin_dir), mirror, errors)
        for dependent in DEPENDENT_PLUGIN_DIRS:
            if os.path.isdir(dependent):
                check_plugin_includes(dependent, mirror, errors)
    except InputError as e:
        print(f"::error::sync_o3ds_core: {e}")
        return 2

    mirror_root = os.path.join(args.plugin_dir, *MIRROR_REL.split("/"))
    wrapper_root = os.path.join(args.plugin_dir, *WRAPPER_REL.split("/"))
    core_count = sum(1 for r in mirror if r.startswith("o3ds/"))
    print(f"o3ds core closure: {core_count} files, {len(wrappers)} translation units; "
          f"{len(mirror)} files in {MIRROR_REL}")

    if args.check:
        problems = compare(mirror, mirror_root, MIRROR_REL)
        problems += compare(wrappers, wrapper_root, WRAPPER_REL, keep=WRAPPER_DIR_KEEP)
        for p in errors + problems:
            print(f"::error::sync_o3ds_core: {p}")
        if errors or problems:
            if problems:
                print("The plugin's copy of the o3ds core is out of date. Run\n"
                      "  python3 Build/Scripts/sync_o3ds_core.py\n"
                      "and commit the result. Do not edit files under "
                      f"{MIRROR_REL} or {WRAPPER_REL}/{WRAPPER_PREFIX}*.cpp by hand.")
            return 1
        print("[OK] The plugin's o3ds core mirror matches src/.")
        return 0

    if errors:
        for e in errors:
            print(f"::error::sync_o3ds_core: {e}")
        return 1
    changes = write_tree(mirror, mirror_root)
    changes += [f"{WRAPPER_REL}: {c}" for c in write_tree(wrappers, wrapper_root, keep=WRAPPER_DIR_KEEP)]
    for c in changes:
        print(f"  {c}")
    print(f"[OK] {len(changes)} change(s) written." if changes else "[OK] Already in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
