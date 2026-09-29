"""Record real co-op sessions, then verify every saved hash in offline playback."""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

from mptest import ROOT, prepare


def clean_environment():
    return {k: v for k, v in os.environ.items() if not k.startswith(("TH07_MP_", "TH07_ROLLBACK_", "TH07_BOT_"))}


def records(path):
    data = path.read_bytes()
    header = struct.unpack_from("<I", data, 12)[0]
    assert header == 4608 and data[:8] == b"T07MPR2\0"
    frames = []
    for offset in range(header, len(data) - 83, 84):
        frame = data[offset:offset + 84]
        if struct.unpack_from("<I", frame, 12)[0] == 2:
            break
        assert struct.unpack_from("<I", frame)[0] == len(frames)
        frames.append(frame)
    return data[:header], frames


def checksum(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--players", type=int, default=4, choices=[1, 2, 3, 4])
    ap.add_argument("--seconds", type=int, default=300)
    campaign = ap.add_mutually_exclusive_group()
    campaign.add_argument("--campaign", action="store_true", help="cycle Normal/Normal/Extra/Phantasm with short stages")
    campaign.add_argument("--natural-campaign", action="store_true", help="same route, without forced stage clears")
    ap.add_argument("--campaign-offset", type=int, default=0, choices=range(4))
    ap.add_argument("--local", action="store_true")
    ap.add_argument("--lockstep", action="store_true")
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=29100)
    ap.add_argument("--lag", default="75,25,0.02")
    ap.add_argument("--lag-schedule", default="60:130,40,0.03;120:35,15,0.02;180:100,30,0.02")
    args = ap.parse_args()
    out = (ROOT / args.out).resolve()
    if not out.is_relative_to(ROOT / "wk") or out == ROOT / "wk" or out.exists():
        ap.error("--out must be a new directory below wk/")
    out.mkdir(parents=True)
    count = 1 if args.local else args.players
    tag = "rpr_" + str(time.time_ns()) + "_"
    # Keep the recording and every playback on the exact same binary.
    exe = out / "th07.exe"
    shutil.copy2(ROOT / "build/th07.exe", exe)
    command = [sys.executable, str(ROOT / "tools/mptest.py"), "local" if args.local else "udp",
               "--players", str(args.players), "--seconds", str(args.seconds), "--tag", tag,
               "--port", str(args.port), "--out", str(out / "live"), "--exe", str(exe),
               "--env", "TH07_MP_TEST_KEEP_ALIVE=1", "--env", "TH07_MP_TEST_BOT_MASH=1"]
    if not args.local:
        command += ["--lag", args.lag, "--lag-schedule", args.lag_schedule, "--progress-timeout", "45"]
        if not args.lockstep:
            command += ["--rollback", "--delay", "0", "--env", "TH07_MP_TEST_DELAY_CYCLE=180"]
            if args.players > 2:
                command += ["--seat-env", "2:TH07_ROLLBACK_CHECKPOINT_EVERY=1"]
            if args.players > 3:
                command += ["--seat-env", "3:TH07_ROLLBACK_DRAW_REPLAY=1"]
    if args.campaign or args.natural_campaign:
        command += ["--env", "TH07_MP_TEST_CAMPAIGN=1", "--env", f"TH07_MP_TEST_CAMPAIGN_OFFSET={args.campaign_offset}"]
        if args.campaign:
            command += ["--env", "TH07_MP_TEST_STAGE_CLEAR_FRAME=90", "--env", "TH07_MP_TEST_STAGE_CLEAR_LAST=8"]
    replay_paths = [out / f"seat{seat + 1}.mpr" for seat in range(count)]
    for seat, path in enumerate(replay_paths):
        command += ["--seat-env", f"{seat}:TH07_MP_RECORD={path}"]
    report = dict(arguments=vars(args), executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    report["game_data_sha256"] = hashlib.sha256((ROOT / "run/th07.dat").read_bytes()).hexdigest()
    report["live"] = []
    report["playback"] = []

    def save_report(phase):
        report["phase"] = phase
        (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    save_report("recording")
    print("Recording real session...", flush=True)
    with (out / "record.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, cwd=ROOT, env=clean_environment(), stdout=log, stderr=subprocess.STDOUT, check=True)
    run = json.loads((out / "live/run.json").read_text())
    assert run["test_seconds"] >= args.seconds - 1, "session exited before the requested duration"
    assert run["exit_codes"] == ["killed"] * count, run["exit_codes"]
    report["record_seconds"] = run["test_seconds"]
    if not args.local:
        report["network"] = json.loads((out / "live/lag.json").read_text())
    frame_sets = []
    games_by_seat = []
    for seat, path in enumerate(replay_paths):
        log = (out / "live" / f"seat{seat + 1}.log").read_text(encoding="cp932", errors="replace")
        assert not re.search(r"^(?:FAIL|CRASH|DESYNC\S*|REPLAY_(?:FAIL|SAVE_FAIL|DESYNC))\b", log, re.M), log[-2000:]
        assert all(int(x) == 0 for x in re.findall(r"hash_mismatches=(\d+)", log))
        def maximum(key):
            return max(map(int, re.findall(r"\b" + key + r"=(\d+)", log)), default=0)
        if not args.local and not args.lockstep:
            rollbacks = [int(x) for x in re.findall(r"rollbacks=(\d+)", log)]
            assert rollbacks and max(rollbacks) > 0, "test must exercise actual rollbacks"
        if args.campaign or args.natural_campaign:
            games = re.findall(r"GAME_END difficulty=(\d+)", log)
            if args.campaign:
                route = ["1", "1", "4", "5"]
                assert games[:4] == route[args.campaign_offset:] + route[:args.campaign_offset], games
        if args.natural_campaign:
            assert "TEST_STAGE_CLEAR" not in log
        report["live"].append(dict(seat=seat + 1, rollbacks=maximum("rollbacks"),
                                   replayed_frames=maximum("replayed"), max_rollback=maximum("max_rollback"),
                                   hash_mismatches=maximum("hash_mismatches"),
                                   completed_games=[int(x) for x in re.findall(r"GAME_END difficulty=(\d+)", log)],
                                   stages=[int(x) for x in re.findall(r"STAGE_START stage=(\d+)", log)]))
        games = sorted(out.glob(f"seat{seat + 1}*.mpr"))
        assert games, "no gameplay recordings"
        games_by_seat.append(games)
        frame_sets.append([records(game)[1] for game in games])
        assert sum(map(len, frame_sets[-1])) > 600
        for game in games:
            header, _ = records(game)
            difficulty = struct.unpack_from("<I", header, 268)[0]
            stages = [i + 1 for i in range(8) if struct.unpack_from("<I", header, 304 + i * 540)[0]]
            assert stages and all((difficulty < 4 and stage <= 6) or
                                  (difficulty == 4 and stage == 7) or
                                  (difficulty == 5 and stage == 8) for stage in stages)
    common = 0
    for game in range(min(map(len, frame_sets))):
        shared = min(len(seat[game]) for seat in frame_sets)
        def simulation_frame(frame):
            normalized = bytearray(frame[:80])
            struct.pack_into("<I", normalized, 12, struct.unpack_from("<I", frame, 12)[0] & 1)
            return normalized
        expected = [simulation_frame(f) for f in frame_sets[0][game][:shared]]
        assert all([simulation_frame(f) for f in seat[game][:shared]] == expected for seat in frame_sets), "peer replay streams differ"
        common += shared
    report.update(frames=[sum(map(len, seat)) for seat in frame_sets], common_frames=common,
                  games=[[path.name for path in games] for games in games_by_seat])
    save_report("playback")
    print(f"Recorded {report['frames']} frames; all peers share {common} identical confirmed frames.", flush=True)

    def launch(path, name, stage=0):
        folder, _ = prepare(tag + name, exe)
        env = clean_environment()
        env.update(TH07_MP_REPLAY=str(path), TH07_MP_REPLAY_TEST="1", TH07_MP_REPLAY_FAST="1",
                   TH07_MP_LOG=str(out / (name + ".log")))
        if stage:
            env["TH07_MP_REPLAY_STAGE"] = str(stage)
        return subprocess.Popen([str(folder / "th07.exe")], cwd=folder, env=env)

    for seat, games in enumerate(games_by_seat):
        for path in games:
            header, frames = records(path)
            for stage in range(1, 9):
                entry = 284 + (stage - 1) * 540
                if not struct.unpack_from("<I", header, entry + 20)[0]:
                    continue
                name = path.stem + f"_stage{stage}"
                process = launch(path, name, stage)
                try:
                    assert process.wait(timeout=max(120, len(frames) // 20)) == 0
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                log = (out / (name + ".log")).read_text(encoding="cp932", errors="replace")
                assert not re.search(r"REPLAY_(FAIL|DESYNC)|CRASH", log), log[-2000:]
                done = re.search(r"REPLAY_DONE frames=(\d+) hashes=(\d+) complete=(\d)", log)
                start = struct.unpack_from("<I", header, entry)[0]
                expected_hashes = sum(bool(struct.unpack_from("<I", f, 12)[0] & 1) for f in frames[start:])
                assert done and int(done[1]) == len(frames) and int(done[2]) == expected_hashes, log[-1000:]
                observed_stages = re.findall(r"STAGE_START stage=(\d+)", log)
                assert observed_stages and int(observed_stages[0]) == stage, "playback must start at chosen stage"
                report["playback"].append(dict(file=path.name, stage=stage, frames=len(frames) - start,
                                              matching_hashes=int(done[2]), complete=bool(int(done[3])), exit_code=0))
                save_report("playback")
                print(f"{name}: {len(frames) - start} frames, {done[2]} matching hashes.", flush=True)

    # A plausible, checksummed edit must still fail the simulation hash check.
    corrupt = bytearray(replay_paths[0].read_bytes())
    _, frames = records(replay_paths[0])
    index = next(i for i, f in enumerate(frames) if struct.unpack_from("<I", f, 4)[0] == 1 and
                 struct.unpack_from("<I", f, 12)[0] & 1)
    offset = 4608 + index * 84
    corrupt[offset + 16] ^= 0x10
    struct.pack_into("<I", corrupt, offset + 80, checksum(corrupt[offset:offset + 80]))
    bad_path = out / "modified_input.mpr"
    bad_path.write_bytes(corrupt)
    process = launch(bad_path, "negative")
    try:
        assert process.wait(timeout=60) != 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert "REPLAY_DESYNC" in (out / "negative.log").read_text(encoding="cp932", errors="replace")
    report["playback_passed"] = report["modified_input_rejected"] = True
    save_report("passed")
    print("PASS multiplayer replay integration", flush=True)


if __name__ == "__main__":
    main()
