"""Check whole-byte snapshots and benchmark a repeatable 40 MiB rollback workload.

--source can point at a saved RollbackMemory.cpp with its original headers beside it
to compare the same workload with an earlier backend. Output stays under wk/.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from build import MSVC, toolchain_env


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=ROOT / "src/th07/multi/RollbackMemory.cpp")
    ap.add_argument("--out", type=Path, default=ROOT / "wk/rollback_memory_test")
    args = ap.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / "wk") or out == ROOT / "wk":
        ap.error("--out must be below wk/")
    out.mkdir(parents=True, exist_ok=True)
    source = args.source.resolve()
    exe = out / "memory_test.exe"
    for checked in (True, False):
        flags = [] if checked else ["/DNDEBUG"]
        subprocess.run([str(MSVC / "bin/cl.exe"), "/nologo", "/EHsc", "/O2", "/MT", *flags,
                        "/I" + str(source.parent), "/Fo" + str(out) + "\\", "/Fe" + str(exe),
                        str(ROOT / "tools/rollback_memory_test.cpp"), str(source)],
                       env=toolchain_env(), cwd=out, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        name = "checked" if checked else "release"
        print(name, result.stdout, result.stderr, flush=True)
        (out / (name + ".txt")).write_text(result.stdout + result.stderr, encoding="utf-8")
        result.check_returncode()


if __name__ == "__main__":
    main()
