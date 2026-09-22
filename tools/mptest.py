"""Run multiplayer instances of the game for a test, without the launcher (environment only).

    py -3.10 tools/mptest.py local --players 2 --seconds 40 --shots 15,30 --out wk/local
    py -3.10 tools/mptest.py udp --players 2 [--rollback] --seconds 60 --out wk/udp

Each instance gets run/mptest/<name>/ (the exe, th07.dat and thbgm.dat hard linked) and its
own windowed th07.cfg (th07 keeps its cfg and score file beside the exe). The seat logs, the
screenshots (--shots: seconds after the start) and a summary go to --out. Extra environment:
--env NAME=VALUE (all instances), --seat-env SEAT:NAME=VALUE.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run"


def prepare(name, exe):
    d = RUN / "mptest" / name
    d.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, d / "th07.exe")
    for data in ("th07.dat", "thbgm.dat"):
        target = d / data
        if not target.exists():
            try:
                os.link(RUN / data, target)
            except OSError:
                shutil.copy2(RUN / data, target)
    for old in ("score.dat", "log.txt"):
        if (d / old).exists():
            (d / old).unlink()
    # th07.cfg (GameConfiguration, 0x38 bytes): the user's from run/th07.cfg, windowed (+0x22)
    cfg = bytearray((RUN / "th07.cfg").read_bytes())
    cfg[0x22] = 1
    (d / "th07.cfg").write_bytes(bytes(cfg))
    return d, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["local", "udp"])
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--rollback", action="store_true")
    ap.add_argument("--delay", type=int, help="initial gameplay delay (rollback default 0, lockstep default 4)")
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--shots", default="")
    ap.add_argument("--out", default="wk/mptest")
    ap.add_argument("--exe", default=str(ROOT / "build" / "th07.exe"))
    ap.add_argument("--port", type=int, default=28020)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--seat-env", action="append", default=[])
    ap.add_argument("--no-bot", action="store_true")
    ap.add_argument("--tag", default="", help="instance folder prefix (run/mptest/<tag><a..d>), for runs side by side")
    ap.add_argument("--lag", default="", help="delay_ms,jitter_ms,loss through tools/udp_lag_proxy.py")
    ap.add_argument("--lag-schedule", default="", help="seconds:delay_ms,jitter_ms,loss;... (during a connection)")
    args = ap.parse_args()

    out = (ROOT / args.out).resolve()
    if not out.is_relative_to((ROOT / "wk").resolve()) or out == (ROOT / "wk").resolve():
        ap.error("--out must be a subdirectory of this repository's wk directory")
    if not (1 if args.mode == "local" else 2) <= args.players <= 4:
        ap.error("--players must be 1..4 (local) or 2..4 (udp)")
    if not all(c.isalnum() or c in "_-" for c in args.tag):
        ap.error("--tag may only contain letters, digits, underscores and hyphens")
    if out.exists():
        ap.error("--out already exists; choose a new directory to preserve earlier evidence")
    out.mkdir(parents=True)
    count = 1 if args.mode == "local" else args.players
    procs = []
    proxy = None
    proxy_base = args.port + 1000
    if args.lag and args.mode == "udp":
        d, j, l = args.lag.split(",")
        proxy = subprocess.Popen([sys.executable, str(ROOT / "tools" / "udp_lag_proxy.py"), "--host", str(args.port),
                                  "--proxy-base", str(proxy_base), "--guests",
                                  ",".join(str(s) for s in range(1, args.players)), "--delay-ms", d, "--jitter-ms", j,
                                  "--loss", l, "--stats", str(out / "lag.json"),
                                  "--schedule", args.lag_schedule])
        time.sleep(0.5)
    for seat in range(count):
        d, appdata = prepare(args.tag + chr(ord("a") + seat), args.exe)
        env = dict(os.environ)
        env["TH07_MP_MODE"] = args.mode
        env["TH07_MP_PLAYERS"] = str(args.players)
        env["TH07_MP_SEAT"] = str(seat)
        env["TH07_MP_LOG"] = str(out / f"seat{seat + 1}.log")
        env["TH07_MP_TEST_TITLE_BOT"] = "1"
        if not args.no_bot:
            env["TH07_MP_TEST_BOT"] = "1"
        if args.mode == "udp":
            env["TH07_MP_SESSION"] = "0x20260913"
            env["TH07_MP_ROLLBACK"] = "1" if args.rollback else "0"
            env["TH07_MP_TEST_DELAY"] = str(args.delay if args.delay is not None else (0 if args.rollback else 4))
            env["TH07_MP_BIND"] = f"127.0.0.1:{args.port + seat}"
            # guests talk to the host; the host finds the guests at their bind ports
            if proxy is not None:
                env["TH07_MP_PEER"] = f"127.0.0.1:{proxy_base + (seat if seat else 1)}"
            else:
                env["TH07_MP_PEER"] = f"127.0.0.1:{args.port}" if seat else f"127.0.0.1:{args.port + 1}"
        for kv in args.env:
            k, v = kv.split("=", 1)
            env[k] = v
        for skv in args.seat_env:
            s, kv = skv.split(":", 1)
            if int(s) == seat:
                k, v = kv.split("=", 1)
                env[k] = v
        procs.append(subprocess.Popen([str(d / "th07.exe")], cwd=str(d), env=env))
    start = time.time()
    metadata = dict(arguments=vars(args), started=start,
                    executable_sha256=hashlib.sha256(Path(args.exe).read_bytes()).hexdigest(),
                    pids=[p.pid for p in procs])
    (out / "run.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    shots = sorted(float(s) for s in args.shots.split(",") if s)
    try:
        while time.time() - start < args.seconds:
            if any(p.poll() is not None for p in procs):
                break
            now = time.time() - start
            while shots and shots[0] <= now:
                t = shots.pop(0)
                for seat, p in enumerate(procs):
                    subprocess.run([sys.executable, str(ROOT / "tools" / "grab_window.py"), str(p.pid),
                                    str(out / f"shot_{int(t):03d}_seat{seat + 1}.png")])
            time.sleep(0.2)
    finally:
        codes = []
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
                codes.append("killed")
            else:
                codes.append(str(p.returncode))
        if proxy is not None:
            proxy.kill()
            proxy.wait()
        metadata.update(elapsed_seconds=time.time() - start, exit_codes=codes)
        (out / "run.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print("exit codes:", " ".join(codes))
    for seat in range(count):
        log = out / f"seat{seat + 1}.log"
        if log.exists():
            lines = log.read_text(encoding="cp932", errors="replace").splitlines()
            print(f"--- seat {seat + 1}: {len(lines)} lines")
            for line in lines[-8:]:
                print("   ", line)


if __name__ == "__main__":
    main()
