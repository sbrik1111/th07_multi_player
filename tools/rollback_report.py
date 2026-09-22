"""Audit mptest evidence. Fails on crashes/desync, early exit, no hash checks,
or a missing requested Normal -> Normal -> Extra campaign.

    py -3.10 tools/rollback_report.py wk/rollback_soak4 --campaign --minimum-seconds 1800
"""
import argparse
import json
import re
import statistics
from pathlib import Path


def fields(line):
    result = dict(re.findall(r"(\w+)=([^ ]+)", line))
    return {k: int(v) if re.fullmatch(r"-?\d+", v) else v for k, v in result.items()}


def summarize_perf(rows):
    if not rows:
        return {}
    fps = sorted(row["fps_milli"] / 1000 for row in rows)
    frames = sum(row["next"] - row["from"] for row in rows)
    return dict(samples=len(rows), seconds=sum(row["dt_ms"] for row in rows) / 1000,
                frames=frames, fps=frames * 1000 / sum(row["dt_ms"] for row in rows),
                fps_p05=fps[int((len(fps) - 1) * .05)], fps_median=statistics.median(fps),
                longest_idle_ms=max(row["longest_idle_ms"] for row in rows),
                rollbacks=sum(row["rollbacks"] for row in rows),
                replayed=sum(row["replayed"] for row in rows),
                rtt_median_ms=statistics.median(row["rtt_us"] / 1000 for row in rows))


def audit(directory, minimum_seconds=0, campaign=False):
    directory = Path(directory)
    errors, seats = [], []
    metadata = json.loads((directory / "run.json").read_text(encoding="utf-8"))
    expected = metadata["arguments"]["players"]
    if metadata.get("elapsed_seconds", 0) < minimum_seconds:
        errors.append("run shorter than requested minimum")
    if any(code != "killed" for code in metadata.get("exit_codes", [])):
        errors.append("game exited before the harness stopped it")
    if len(metadata.get("exit_codes", [])) != expected:
        errors.append("missing final process status")
    for seat in range(1, expected + 1):
        path = directory / f"seat{seat}.log"
        if not path.exists():
            errors.append(f"missing seat {seat} log")
            continue
        lines = path.read_text(encoding="cp932", errors="replace").splitlines()
        faults = [line for line in lines if re.match(r"^(DESYNC|FAULT|FAIL|CRASH)\b|^DESYNC_", line)]
        errors.extend(f"seat {seat}: {line}" for line in faults)
        net = [fields(line) for line in lines if line.startswith("NET ")]
        ends = [fields(line) for line in lines if line.startswith("SEGMENT_END ")]
        if not net or not any(n.get("hash_checked", 0) > 0 for n in net):
            errors.append(f"seat {seat}: no confirmed hash comparisons")
        if any(n.get("hash_mismatches", 0) for n in net + ends):
            errors.append(f"seat {seat}: hash mismatch counter is nonzero")
        stages = [fields(line) for line in lines if line.startswith("STAGE_START ")]
        new_games = [s for s in stages if s["new_game"] == 1]
        routes = [s["difficulty"] for s in new_games]
        completed = [e for e in ends if e["index"] % 2 == 1]
        game_ends = [fields(line) for line in lines if line.startswith("GAME_END ")]
        if campaign and (routes[:3] != [1, 1, 4] or len(completed) < 3):
            errors.append(f"seat {seat}: two Normal games and Extra were not all completed")
        perf = [fields(line) for line in lines if line.startswith("NET_PERF ")]
        if not perf:
            errors.append(f"seat {seat}: no wall-clock progress measurements")
        elif metadata.get("elapsed_seconds", 0) * 1000 - perf[-1]["elapsed_ms"] > 20000:
            errors.append(f"seat {seat}: progress logging stopped before the test ended")
        elif sum(p["next"] - p["from"] for p in perf[-6:]) < 30:
            errors.append(f"seat {seat}: simulation stopped progressing near the end")
        game_perf = [p for p in perf if p["phase"] == "gameplay"]
        seats.append(dict(seat=seat, stages=stages, routes=routes, completed=completed, game_ends=game_ends,
                          latest_net=net[-1] if net else {},
                          ready=sum(line.startswith("READY ") for line in lines),
                          input_delay_changes=sum(line.startswith("INPUT_DELAY ") for line in lines),
                          gameplay=summarize_perf(game_perf),
                          by_delay={str(d): summarize_perf([p for p in game_perf if p["delay"] == d])
                                    for d in sorted(set(p["delay"] for p in game_perf))}))
    # Stage entry identity must match in every process; the last partial stage
    # can appear first on the leading PC when the harness ends.
    if seats:
        common = min(len(s["stages"]) for s in seats)
        reference = seats[0]["stages"][:common]
        for s in seats[1:]:
            if s["stages"][:common] != reference:
                errors.append(f"seat {s['seat']}: stage entry states differ")
    result = dict(ok=not errors, errors=errors, run=metadata, seats=seats)
    (directory / "report.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory")
    ap.add_argument("--minimum-seconds", type=float, default=0)
    ap.add_argument("--campaign", action="store_true")
    args = ap.parse_args()
    result = audit(args.directory, args.minimum_seconds, args.campaign)
    print("PASS" if result["ok"] else "FAIL", args.directory)
    for seat in result["seats"]:
        print(json.dumps({k: seat[k] for k in ("seat", "routes", "ready", "input_delay_changes", "gameplay")}, ensure_ascii=False))
    for error in result["errors"]:
        print(error)
    raise SystemExit(0 if result["ok"] else 1)


if __name__ == "__main__":
    main()
