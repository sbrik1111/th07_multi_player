import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from build import MSVC, toolchain_env

out = ROOT / "wk/replay_file_test"
out.mkdir(parents=True, exist_ok=True)
exe = out / "replay_file_test.exe"
subprocess.run([str(MSVC / "bin/cl.exe"), "/nologo", "/EHsc", "/O2", "/MT", "/DNDEBUG",
                "/I" + str(ROOT / "src/th07"), "/Fo" + str(out) + "\\", "/Fe" + str(exe),
                str(ROOT / "tools/replay_file_test.cpp"), str(ROOT / "src/th07/multi/ReplayFile.cpp")],
               cwd=out, env=toolchain_env(), check=True)
subprocess.run([str(exe)], cwd=out, check=True)
