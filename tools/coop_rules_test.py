"""Run the in-game cooperative rule tests and require successful logs from every seat."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def check_logs(directory, players):
    errors = []
    for seat in range(1, players + 1):
        path = directory / f"seat{seat}.log"
        if not path.is_file():
            errors.append(f"seat {seat}: missing log")
            continue
        log = path.read_text(encoding="cp932", errors="replace")
        results = re.findall(r"RULE_TEST_RESULT checks=(\d+) failures=(\d+) players=(\d+)", log)
        if len(results) != 1:
            errors.append(f"seat {seat}: expected one completed rule test")
        elif int(results[0][0]) == 0 or int(results[0][1]) != 0 or int(results[0][2]) != players:
            errors.append(f"seat {seat}: invalid or failed result {results[0]}")
        if re.search(r"RULE_TEST FAIL|HASH_MISMATCH|CRASH|hash_mismatches=[1-9]", log):
            errors.append(f"seat {seat}: rule, crash or synchronization failure")
        if not re.search(r"hash_checked=[1-9]", log):
            errors.append(f"seat {seat}: no confirmed synchronization checks")
        if not re.search(r"phase=gameplay .*rollback=1", log):
            errors.append(f"seat {seat}: rollback gameplay never started")
        if results:
            print(f"seat {seat}: checks={results[0][0]} failures={results[0][1]}")
    for error in errors:
        print(error, file=sys.stderr)
    return not errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--players", type=int, choices=(2, 3, 4), default=3)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--out", default="wk/coop_rules")
    parser.add_argument("--logs", type=Path, help="only inspect logs from an existing run")
    args = parser.parse_args()
    directory = (ROOT / (args.logs if args.logs is not None else args.out)).resolve()
    if args.logs is None:
        # mptest replaces its output directory. Keep this runner's output within ignored wk/.
        workspace = (ROOT / "wk").resolve()
        if not directory.is_relative_to(workspace) or directory == workspace:
            parser.error("--out must be a subdirectory of wk/")
        subprocess.run([
            sys.executable, str(ROOT / "tools" / "mptest.py"), "udp",
            "--players", str(args.players), "--rollback", "--seconds", str(args.seconds),
            "--lag", "30,10,0.01", "--env", "TH07_MP_TEST_RULES=1",
            "--tag", f"coop_rules{args.players}", "--out", str(directory),
        ], cwd=ROOT, check=True)
    return 0 if check_logs(directory, args.players) else 1


if __name__ == "__main__":
    sys.exit(main())
