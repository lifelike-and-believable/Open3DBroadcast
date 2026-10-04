#!/usr/bin/env python3
"""Check that no transport module records into the default runtime context directly.

The rule (ADR 0012 item 3, SHR-38): a transport records its metrics into the
runtime context its config names, FO3DRuntimeContext::OrDefault(Config.Context),
resolved in Initialize. It never calls FO3DPerformanceMetrics::Get(), which is
always the default context, so a transport given another context would count
into the wrong one.

Checked: the sources (.h, .cpp, .inl) of every module directory named
Open3DTransport* under Source/ of the plugins below, ThirdParty/ excluded.
Comments are removed first, so a comment that names the call is fine.

Plugins checked by default: ProjectSandbox/Plugins/Open3DBroadcast and the
WebRTC add-on ProjectSandbox/Plugins/Open3DBroadcastWebRTC. Pass --plugin-dir
(repeatable) to check others.

Python 3.8 or later, standard library only. Exit codes: 0 no violation; 1 at
least one violation; 2 bad input (a plugin directory without Source/).
--self-test runs the check against small generated plugins, one clean and one
with the call, and exits 0 only if it passes the clean one and fails the bad one.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_PLUGIN_DIRS = [
    os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcast"),
    os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcastWebRTC"),
]

SOURCE_EXTENSIONS = (".h", ".cpp", ".inl")
FORBIDDEN_RE = re.compile(r"\bFO3DPerformanceMetrics\s*::\s*Get\s*\(")


class InputError(Exception):
    pass


def strip_comments(text):
    """Removes // and /* */ comments, keeping string literals and line breaks."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def transport_modules(plugin_dir):
    source_dir = os.path.join(plugin_dir, "Source")
    if not os.path.isdir(source_dir):
        raise InputError("no Source directory in %s" % plugin_dir)
    return sorted(
        os.path.join(source_dir, name)
        for name in os.listdir(source_dir)
        if name.startswith("Open3DTransport") and os.path.isdir(os.path.join(source_dir, name))
    )


def check(plugin_dir):
    violations = []
    for module_dir in transport_modules(plugin_dir):
        for root, dirs, files in os.walk(module_dir):
            dirs[:] = sorted(d for d in dirs if d != "ThirdParty")
            for name in sorted(files):
                if not name.endswith(SOURCE_EXTENSIONS):
                    continue
                path = os.path.join(root, name)
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = strip_comments(f.read())
                for number, line in enumerate(text.splitlines(), 1):
                    if FORBIDDEN_RE.search(line):
                        rel = os.path.relpath(path, plugin_dir).replace(os.sep, "/")
                        violations.append("%s:%d: FO3DPerformanceMetrics::Get() in a transport; use the config's runtime context (ADR 0012 item 3)" % (rel, number))
    return violations


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def self_test():
    root = tempfile.mkdtemp(prefix="o3d-transport-metrics-")
    try:
        clean = os.path.join(root, "Clean")
        write(os.path.join(clean, "Source", "Open3DTransportX", "Private", "X.cpp"),
              "// FO3DPerformanceMetrics::Get() in a comment is fine\n"
              "/* and FO3DPerformanceMetrics::Get() here */\n"
              "const char* S = \"// not a comment\";\n"
              "void F() { Context->GetMetrics().RecordFrameCaptured(); }\n")
        write(os.path.join(clean, "Source", "Open3DReceiver", "Private", "R.cpp"),
              "void G() { FO3DPerformanceMetrics::Get().RecordFrameReceived(); }\n")
        write(os.path.join(clean, "Source", "Open3DTransportX", "ThirdParty", "T.h"),
              "inline void H() { FO3DPerformanceMetrics::Get(); }\n")
        bad = os.path.join(root, "Bad")
        write(os.path.join(bad, "Source", "Open3DTransportY", "Private", "Y.cpp"),
              "void F()\n{\n    FO3DPerformanceMetrics :: Get ().RecordFrameCaptured();\n}\n")
        clean_violations = check(clean)
        bad_violations = check(bad)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if clean_violations:
        print("self-test FAILED: the clean plugin was rejected:\n  " + "\n  ".join(clean_violations))
        return 1
    if len(bad_violations) != 1 or ":3:" not in bad_violations[0]:
        print("self-test FAILED: expected one violation on line 3 of the bad plugin, got %r" % bad_violations)
        return 1
    print("self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--plugin-dir", action="append", help="plugin directory to check (repeatable)")
    parser.add_argument("--self-test", action="store_true", help="check the check against generated plugins")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()

    violations = []
    try:
        for plugin_dir in args.plugin_dir or DEFAULT_PLUGIN_DIRS:
            violations.extend(check(plugin_dir))
    except InputError as error:
        print("error: %s" % error)
        return 2
    if violations:
        print("\n".join(violations))
        print("%d violation(s)." % len(violations))
        return 1
    print("No transport calls FO3DPerformanceMetrics::Get().")
    return 0


if __name__ == "__main__":
    sys.exit(main())
