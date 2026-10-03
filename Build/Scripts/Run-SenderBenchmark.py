#!/usr/bin/env python3
"""Record and summarize the ADR 0008 sender pipeline measurement (Decision item 11).

Runs the automation benchmark Open3DBroadcast.Bench.SenderPipeline
(Open3DBroadcastTests/Private/Bench/O3DSenderPipelineBench.cpp) in the editor with
O3DB_BENCH=1 and Unreal Insights tracing on, then has UnrealInsights export the sender's
timers for each case's region, and prints one table:

  case                     one of Loopback|UDP x 1|10 senders x o3d.Sender.AsyncPipeline 0|1
  game thread per sender   O3D.Sender.Sample on the game thread: median and p99 (ms). With the
                           pipeline off it also contains serialization and the send.
  worker serialize         O3D.Sender.Pipeline.Serialize (one frame, including the send):
                           median and p99 (ms); empty with the pipeline off
  capture-to-send          the pipeline's LastCaptureToSendSeconds sampled once per frame per
                           sender: p50, p99 and the largest seen (ms)
  dropped                  frames the pipeline dropped (queue full), out of submitted
  accepted/refused         payloads the transport accepted and refused. Loopback has no receiver
                           in the benchmark, so it refuses almost everything: its rows time the
                           refusal path, not a delivered send. UDP sends to a port nobody binds,
                           which still counts as sent.

Budgets (ADR 0008 item 11): game thread <= 0.1 ms median per sender; worker serialize <= 0.2 ms
per frame; capture-to-send p99 <= 2 frames at 60 Hz (33.3 ms); no pipeline drops.

Usage (Windows, a built editor for the project):
  python Build/Scripts/Run-SenderBenchmark.py --ue "C:/Program Files/Epic Games/UE_5.7" \
      --project ProjectSandbox/ProjectSandbox.uproject --out <short output folder>

Writes into --out: bench.utrace, editor.log, insights.log, one CSV per region and summary.md.
Keep --out short: the editor and UBT fail on long paths.
"""

import argparse
import csv
import glob
import os
import re
import statistics
import subprocess
import sys

TIMERS = ["O3D.Sender.Sample", "O3D.Sender.Pipeline.Serialize", "O3D.Sender.Pipeline.Send"]
# The steps inside one frame's worker time, in order (the second table). Validate to Copy are the
# default (legacy) encoding's steps; Filter runs on the worker only with the pipeline on.
STEPS = ["O3D.Sender.Pipeline.Filter", "O3D.Sender.Serializer.Validate", "O3D.Sender.Serializer.Build",
         "O3D.Sender.Serializer.CalcMatrices", "O3D.Sender.Serializer.Core", "O3D.Sender.Serializer.Copy",
         "O3D.Sender.Pipeline.Send"]
ALL_TIMERS = TIMERS + [step for step in STEPS if step not in TIMERS]
RESULT_RE = re.compile(r"O3D_BENCH (.*)$")


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(-(-fraction * len(ordered) // 1)) - 1))
    return ordered[index]


def fmt(value, digits=3):
    return "-" if value is None else f"{value:.{digits}f}"


def run(cmd, env=None):
    print("+", " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    return subprocess.call(cmd, env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ue", required=True, help="Unreal Engine root (the folder that holds Engine/)")
    ap.add_argument("--project", required=True, help=".uproject whose editor binaries are built")
    ap.add_argument("--out", required=True, help="Output folder (short path)")
    ap.add_argument("--skip-run", action="store_true", help="Reuse bench.utrace and editor.log in --out; export and summarize only")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    binaries = os.path.join(args.ue, "Engine", "Binaries", "Win64")
    editor = os.path.join(binaries, "UnrealEditor-Cmd.exe")
    insights = os.path.join(binaries, "UnrealInsights.exe")
    trace = os.path.join(out, "bench.utrace")
    editor_log = os.path.join(out, "editor.log")

    if not args.skip_run:
        if os.path.exists(trace):
            os.remove(trace)
        env = dict(os.environ, O3DB_BENCH="1")
        code = run([editor, os.path.abspath(args.project),
                    "-unattended", "-nop4", "-NullRHI", "-NoSound", "-NoSplash",
                    f"-abslog={editor_log}",
                    f"-ReportExportPath={os.path.join(out, 'report')}",
                    "-ExecCmds=Automation RunTests Open3DBroadcast.Bench.SenderPipeline; Quit",
                    "-TestExit=Automation Test Queue Empty",
                    "-trace=cpu,frame,region", f"-tracefile={trace}"], env=env)
        if code != 0:
            print(f"warning: the editor exited with {code}; see {editor_log}", file=sys.stderr)
        if not os.path.exists(trace):
            print(f"error: no trace was written to {trace}", file=sys.stderr)
            return 1

    # The export runs as a response file of Insights commands (one per line), the form the
    # engine's own export tests use (TraceInsights ExportCommandsTests.cpp): no nested quoting.
    rsp = os.path.join(out, "export.rsp")
    with open(rsp, "w", encoding="utf-8") as handle:
        handle.write(f'TimingInsights.ExportTimingEvents {os.path.join(out, "{region}.csv")} '
                     f'-columns=ThreadName,TimerName,StartTime,Duration '
                     f'-timers={",".join(ALL_TIMERS)} -region=O3D.Bench.*\n')
    for stale in glob.glob(os.path.join(out, "O3D.Bench.*.csv")):
        os.remove(stale)
    run([insights, f"-OpenTraceFile={trace}", "-AutoQuit", "-NoUI",
         f"-ABSLOG={os.path.join(out, 'insights.log')}",
         f"-ExecOnAnalysisCompleteCmd=@={rsp}", "-log"])

    results = {}
    with open(editor_log, encoding="utf-8", errors="replace") as log:
        for line in log:
            match = RESULT_RE.search(line)
            if match:
                fields = dict(pair.split("=", 1) for pair in match.group(1).split())
                results[fields["case"]] = fields

    rows = []
    step_rows = []
    for case in sorted(results):
        fields = results[case]
        senders = int(fields["senders"])
        frames = int(fields["frames"])
        durations = {timer: [] for timer in ALL_TIMERS}
        game_thread = []
        csv_path = os.path.join(out, f"O3D.Bench.{case}.csv")
        if os.path.exists(csv_path):
            with open(csv_path, newline="", encoding="utf-8", errors="replace") as handle:
                for event in csv.DictReader(handle):
                    timer = event.get("TimerName", "")
                    if timer not in durations:
                        continue
                    ms = float(event["Duration"]) * 1000.0
                    durations[timer].append(ms)
                    if timer == "O3D.Sender.Sample" and event.get("ThreadName", "") == "GameThread":
                        game_thread.append(ms)
        serialize = durations["O3D.Sender.Pipeline.Serialize"]
        rows.append([
            case,
            f"{len(game_thread)}/{senders * frames}",
            fmt(statistics.median(game_thread) if game_thread else None),
            fmt(percentile(game_thread, 0.99)),
            fmt(statistics.median(serialize) if serialize else None),
            fmt(percentile(serialize, 0.99)),
            fmt(float(fields["latency_ms_p50"]), 2),
            fmt(float(fields["latency_ms_p99"]), 2),
            fmt(float(fields["latency_ms_max"]), 2),
            f'{fields["dropped"]}/{fields["submitted"]}',
            fields["max_queued"],
            f'{fields["accepted"]}/{fields["refused"]}',
        ])
        step_rows.append([case] + [fmt(statistics.median(durations[step]) if durations[step] else None) for step in STEPS])

    header = ["case", "samples", "GT median ms", "GT p99 ms", "worker median ms", "worker p99 ms",
              "c2s p50 ms", "c2s p99 ms", "c2s max ms", "dropped", "max queued", "accepted/refused"]
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    step_header = ["case (median ms per frame)"] + [step.split(".")[-1] for step in STEPS]
    lines += ["", "| " + " | ".join(step_header) + " |", "|" + "---|" * len(step_header)]
    lines += ["| " + " | ".join(row) + " |" for row in step_rows]
    table = "\n".join(lines)
    print(table)
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as summary:
        summary.write(table + "\n")
    if not rows:
        print(f"error: no O3D_BENCH lines in {editor_log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
