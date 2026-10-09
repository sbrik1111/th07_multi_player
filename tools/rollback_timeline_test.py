"""Build and execute the portable input timeline regression with the repo's compiler."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from build import MSVC, toolchain_env

out = ROOT / "wk" / "timeline_test"
out.mkdir(parents=True, exist_ok=True)
exe = out / "timeline_test.exe"
subprocess.run([str(MSVC / "bin" / "cl.exe"), "/nologo", "/EHsc", "/O2", "/MT",
                "/I" + str(ROOT / "src" / "th07"),
                "/Fo" + str(out / "timeline_test.obj"), "/Fe" + str(exe),
                str(ROOT / "tools" / "rollback_timeline_test.cpp")],
               env=toolchain_env(), cwd=out, check=True)
subprocess.run([str(exe)], check=True)
