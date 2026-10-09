"""Serial high-latency comparisons, to avoid multiple games distorting CPU/frame pacing.

Use a fresh --out inside wk. Optional --wait-for mptest directories delay the
benchmark until those runs have recorded their final process status.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from rollback_report import audit

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--wait-for", action="append", default=[])
    args = ap.parse_args()
    out = (ROOT / args.out).resolve()
    if not out.is_relative_to(ROOT / "wk") or out == ROOT / "wk" or out.exists():
        ap.error("choose a fresh subdirectory of wk")
    out.mkdir(parents=True)
    deadline = time.monotonic() + 3600
    while args.wait_for:
        remaining = []
        for d in args.wait_for:
            try:
                finished = "exit_codes" in json.loads((ROOT / d / "run.json").read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                finished = False
            if not finished:
                remaining.append(d)
        args.wait_for = remaining
        if time.monotonic() > deadline:
            raise RuntimeError("earlier test did not finish")
        if remaining:
            time.sleep(.5)
    profiles = [("2p_100ms_d0", 2, "50,15,0.01", 0),
                ("2p_300ms_d0", 2, "150,25,0.01", 0),
                ("2p_300ms_d4", 2, "150,25,0.01", 4),
                ("4p_360ms_d0", 4, "90,30,0.02", 0),
                ("4p_360ms_d4", 4, "90,30,0.02", 4),
                ("4p_360ms_d6", 4, "90,30,0.02", 6)]
    results = []
    for index, (name, players, lag, delay) in enumerate(profiles):
        target = out / name
        print("BENCHMARK", name, flush=True)
        subprocess.run([sys.executable, str(ROOT / "tools/mptest.py"), "udp", "--players", str(players),
                        "--rollback", "--seconds", str(args.seconds), "--lag", lag,
                        "--env", f"TH07_MP_TEST_DELAY={delay}", "--port", str(28030 + index * 10),
                        "--tag", f"rbbench{index}", "--out", str(target)], cwd=ROOT, check=True)
        report = audit(target, minimum_seconds=args.seconds)
        results.append(dict(name=name, ok=report["ok"], errors=report["errors"],
                            seats=[s["gameplay"] for s in report["seats"]]))
        (out / "summary.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        if not report["ok"]:
            raise RuntimeError(report["errors"])
    print(json.dumps(results, indent=2), flush=True)


if __name__ == "__main__":
    main()
