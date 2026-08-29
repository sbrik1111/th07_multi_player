"""Build build/th07.exe (the multiplayer fork) with cl 19.10 (VS2017, prefix/msvc1410),
the Windows 10 SDK and the DirectX 8 SDK (thirdparty/dx8).

Usage:
    python scripts/build.py            # incremental build
    python scripts/build.py --clean    # rebuild everything

The matching build with ZUN's compiler (VS.NET 2002, ninja) is scripts/build_vc7.py. This
fork does not aim at matching: src/multi is C++14, which VC7 cannot compile (the same choice
as th16_multi / th14_multi).

Toolchain: prefix/msvc1410 (cl 19.10.25017, copied from th16_multi's prefix, or
TH07_PREFIX=<another checkout's prefix>), the DirectX 8 SDK from scripts/download_deps.py.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

SRC = ROOT / "src" / "th07"
BUILD = ROOT / "build"
OBJ = BUILD / "obj"
MSVC = Path(os.environ.get("TH07_PREFIX", str(ROOT / "prefix"))) / "msvc1410"
DX8 = ROOT / "thirdparty" / "dx8"
KITS = Path(r"C:\Program Files (x86)\Windows Kits\10")
KITS_VER = "10.0.19041.0"

EXE = BUILD / "th07.exe"
PDB = BUILD / "th07.pdb"

CFLAGS = [
    "/nologo",
    "/c",
    "/O2",
    "/Oy-",
    "/GL",
    "/MT",
    "/EHsc",
    "/Zi",
    "/W3",
    "/wd4996",
    "/wd4060",
    "/wd4101",
    "/wd4244",
    "/wd4838",
    "/wd4068",  # #pragma var_order (the matching build's patched VC7)
    "/DDIRECTINPUT_VERSION=0x0800",
    "/DWIN32",
    "/D_WINDOWS",
    "/DNDEBUG",
    "/DNON_MATCHING",
    # Narrow string literals in Shift-JIS as the original's (the game's are escaped bytes;
    # src/multi's new sources are UTF-8 with a BOM).
    "/execution-charset:.932",
    f"/Fd{BUILD / 'th07_cl.pdb'}",
    f"/I{SRC}",
]

LDFLAGS = [
    "/nologo",
    "/LTCG",
    "/DEBUG",
    f"/PDB:{PDB}",
    f"/OUT:{EXE}",
    "/SUBSYSTEM:WINDOWS",
    "/MACHINE:X86",
    "/INCREMENTAL:NO",
    "/OPT:REF",
    "/DYNAMICBASE:NO",
    "/LARGEADDRESSAWARE",
    f"/MAP:{BUILD / 'th07.map'}",
]

LIBS = [
    "d3d8.lib",
    "d3dx8.lib",
    "dinput8.lib",
    "dsound.lib",
    "dxguid.lib",
    "winmm.lib",
    "kernel32.lib",
    "user32.lib",
    "gdi32.lib",
    "ole32.lib",
    "shell32.lib",
    "advapi32.lib",
    "ws2_32.lib",  # the launcher's lobby socket and the session (src/th07/multi)
    "legacy_stdio_definitions.lib",  # d3dx8.lib was built with an older CRT
]


def toolchain_env():
    if not (MSVC / "bin" / "cl.exe").is_file():
        sys.exit(f"cl 19.10.25017 not found in {MSVC} (copy th16_multi's prefix/msvc1410 or set TH07_PREFIX)")
    if not (DX8 / "include" / "d3d8.h").is_file():
        from download_deps import download_dx8

        download_dx8(DX8)
    env = os.environ.copy()
    env["PATH"] = f"{MSVC / 'bin'};{env['PATH']}"
    inc = KITS / "Include" / KITS_VER
    lib = KITS / "Lib" / KITS_VER
    # The DirectX 8 SDK last: its old copies of the SDK's headers (basetsd.h, dinput.h, ...)
    # lose to the Windows 10 SDK's; d3d8.h and d3dx8.h are only there.
    env["INCLUDE"] = ";".join(map(str, [MSVC / "include", inc / "ucrt", inc / "um", inc / "shared", DX8 / "include"]))
    env["LIB"] = ";".join(map(str, [MSVC / "lib", lib / "ucrt" / "x86", lib / "um" / "x86", DX8 / "lib"]))
    env.pop("LIBPATH", None)
    return env


def run(cmd, env, show=True):
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding="mbcs", errors="replace")
    out = (proc.stdout + proc.stderr).strip()
    if out and show:
        print(out)
    return proc.returncode, out


def must(cmd, env):
    code, _ = run(cmd, env)
    if code:
        sys.exit(code)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--keep-going", action="store_true", help="compile every file even after errors")
    args = ap.parse_args()

    if args.clean and BUILD.exists():
        shutil.rmtree(BUILD)
    OBJ.mkdir(parents=True, exist_ok=True)
    env = toolchain_env()

    i18n = SRC / "i18n.hpp"
    csv = ROOT / "resources" / "csv" / "i18n.csv"
    if csv.exists() and (not i18n.exists() or i18n.stat().st_mtime < csv.stat().st_mtime):
        subprocess.check_call([sys.executable, str(ROOT / "scripts" / "generate_i18n.py"), str(csv), str(i18n)])

    sources = sorted(p for ext in ("*.cpp", "*.c") for p in SRC.rglob(ext))
    headers = sorted(p for ext in ("*.h", "*.hpp", "*.inl", "*.inc") for p in SRC.rglob(ext))
    headers_mtime = max((p.stat().st_mtime for p in headers), default=0)

    objs = []
    failed = []
    for src in sources:
        obj = OBJ / src.relative_to(SRC).with_suffix(".obj")
        obj.parent.mkdir(parents=True, exist_ok=True)
        objs.append(obj)
        flags = list(CFLAGS)
        stamp = obj.with_suffix(".flags")
        if (
            obj.exists()
            and obj.stat().st_mtime >= max(src.stat().st_mtime, headers_mtime)
            and stamp.exists()
            and stamp.read_text() == " ".join(flags)
        ):
            continue
        code, _ = run([str(MSVC / "bin" / "cl.exe"), *flags, f"/Fo{obj}", str(src)], env)
        if code:
            failed.append(src.name)
            if not args.keep_going:
                sys.exit(code)
            continue
        stamp.write_text(" ".join(flags))
    if failed:
        sys.exit(f"build: {len(failed)} file(s) failed: {' '.join(failed)}")

    rc_src = SRC / "th07.rc"
    if rc_src.exists():
        res = BUILD / "th07.res"
        rc_inputs = [rc_src, *SRC.glob("*.manifest")]
        if not res.exists() or res.stat().st_mtime < max(p.stat().st_mtime for p in rc_inputs):
            must([str(KITS / "bin" / KITS_VER / "x86" / "rc.exe"), "/nologo", f"/fo{res}", str(rc_src)], env)
        objs.append(res)

    if EXE.exists():
        EXE.unlink()
    code, out = run([str(MSVC / "bin" / "link.exe"), *LDFLAGS, *map(str, objs), *LIBS], env, show=False)
    shown = [l for l in out.splitlines() if l.strip() and "Generating code" not in l and "Finished generating code" not in l]
    if code or not EXE.exists():
        print("\n".join(shown))
        sys.exit("build: link failed")
    for l in shown:
        if "warning" in l:
            print(l)
    print(f"built {EXE.relative_to(ROOT).as_posix()} ({len(sources)} source files)")


if __name__ == "__main__":
    main()
