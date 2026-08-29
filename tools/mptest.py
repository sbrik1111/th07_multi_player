"""Run multiplayer instances of the game for a test, without the launcher (environment only).

    py -3.10 tools/mptest.py local --players 2 --seconds 40 --shots 15,30 --out wk/local
    py -3.10 tools/mptest.py udp --players 2 [--rollback] --seconds 60 --out wk/udp

Each instance gets run/mptest/<name>/ (the exe, th16.dat and thbgm.dat hard linked) and its
own APPDATA (never the real one) with a windowed 640x480 th16.cfg. The seat logs, the
screenshots (--shots: seconds after the start) and a summary go to --out. Extra environment:
--env NAME=VALUE (all instances), --seat-env SEAT:NAME=VALUE.
"""
import argparse
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
    shutil.copy2(exe, d / "th16.exe")
    for data in ("th16.dat", "thbgm.dat"):
        target = d / data
        if not target.exists():
            try:
                os.link(RUN / data, target)
            except OSError:
                shutil.copy2(RUN / data, target)
    appdata = d / "appdata"
    save = appdata / "ShanghaiAlice" / "th16"
    if save.exists():
        shutil.rmtree(save)
    save.mkdir(parents=True)
    # th16's defaults (GameConfig::SetDefaults), then the test's window and no startup dialog
    cfg = bytearray(bytes.fromhex(
        "020016000000010002000500ffffffffffffffffffff0300580258020001010500026450000200000001000000"
        "0000800000008000000000").ljust(100, bytes(1)))
    # th16.cfg is Config +4 (GameConfig): +0x1f window mode (3 = 640x480 windowed),
    # +0x28 bit 0x100 the startup dialog (off)
    cfg[0x1f] = 3
    flags = int.from_bytes(cfg[0x28:0x2c], "little") & ~0x100
    cfg[0x28:0x2c] = flags.to_bytes(4, "little")
    (save / "th16.cfg").write_bytes(bytes(cfg))
    return d, appdata


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["local", "udp"])
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--rollback", action="store_true")
    ap.add_argument("--delay", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--shots", default="")
    ap.add_argument("--out", default="wk/mptest")
    ap.add_argument("--exe", default=str(ROOT / "build" / "th16.exe"))
    ap.add_argument("--port", type=int, default=28020)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--seat-env", action="append", default=[])
    ap.add_argument("--no-bot", action="store_true")
    ap.add_argument("--tag", default="", help="instance folder prefix (run/mptest/<tag><a..d>), for runs side by side")
    ap.add_argument("--lag", default="", help="delay_ms,jitter_ms,loss through tools/udp_lag_proxy.py")
    args = ap.parse_args()

    out = (ROOT / args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
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
                                  "--loss", l, "--stats", str(out / "lag.json")])
        time.sleep(0.5)
    for seat in range(count):
        d, appdata = prepare(args.tag + chr(ord("a") + seat), args.exe)
        env = dict(os.environ)
        env["APPDATA"] = str(appdata)
        env["TH16_MP_MODE"] = args.mode
        env["TH16_MP_PLAYERS"] = str(args.players)
        env["TH16_MP_SEAT"] = str(seat)
        env["TH16_MP_LOG"] = str(out / f"seat{seat + 1}.log")
        env["TH16_MP_TEST_TITLE_BOT"] = "1"
        if not args.no_bot:
            env["TH16_MP_TEST_BOT"] = "1"
        if args.mode == "udp":
            env["TH16_MP_SESSION"] = "0x20260913"
            env["TH16_MP_ROLLBACK"] = "1" if args.rollback else "0"
            env["TH16_MP_TEST_DELAY"] = "0" if args.rollback else str(args.delay)
            env["TH16_MP_BIND"] = f"127.0.0.1:{args.port + seat}"
            # guests talk to the host; the host finds the guests at their bind ports
            if proxy is not None:
                env["TH16_MP_PEER"] = f"127.0.0.1:{proxy_base + (seat if seat else 1)}"
            else:
                env["TH16_MP_PEER"] = f"127.0.0.1:{args.port}" if seat else f"127.0.0.1:{args.port + 1}"
        for kv in args.env:
            k, v = kv.split("=", 1)
            env[k] = v
        for skv in args.seat_env:
            s, kv = skv.split(":", 1)
            if int(s) == seat:
                k, v = kv.split("=", 1)
                env[k] = v
        procs.append(subprocess.Popen([str(d / "th16.exe")], cwd=str(d), env=env))
    start = time.time()
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
                codes.append("killed")
            else:
                codes.append(str(p.returncode))
    if proxy is not None:
        proxy.kill()
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
