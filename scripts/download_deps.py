import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PureWindowsPath
from typing import Iterator
from zipfile import ZipFile

import requests

DX8_URL = "https://archive.org/download/dx8sdk/dx8sdk.exe"
MSVC_URL = "https://archive.org/download/en_vs.net_pro_full/en_vs.net_pro_full.exe"
HACKERY_URL = "https://gist.githubusercontent.com/EstexNT/e98a1384b906a3eedaaa3eeb7e58cd9d/raw/822536a26025f0df8763f1112d89bb1514f6209c/hackery.cpp"
DX8_SIZE = 144441256
MSVC_SIZE = 1706945024


def conv_path(path: Path) -> str:
    """Convert a Unix path to a Windows path, if needed."""
    if sys.platform == "win32":
        return str(PureWindowsPath(path))

    return "Z:" + str(path.resolve())


def conv_path_backslash(path: Path) -> str:
    """Convert a Unix path to a Windows path, if needed, and convert forward
    slashes to backslashes."""
    if sys.platform == "win32":
        return str(PureWindowsPath(path))

    return "Z:" + str(path.resolve()).replace("/", "\\")


def run_program(name: str, *args: str):
    env = os.environ.copy()
    if sys.platform == "win32":
        cmd = [name] + list(args)
    else:
        env["LANG"] = "ja_JP.UTF-8"
        env["WINEDEBUG"] = "fixme-all"
        cmd = ["wine", name] + list(args)
    return subprocess.check_call(cmd, env=env)


def download(url: str, dest_path: Path):
    response = requests.get(url, stream=True)
    total = int(response.headers.get("content-length", 0))
    if response.status_code == 200:
        with open(dest_path, "wb") as file:
            downloaded = 0
            chunk_iter: Iterator[bytes] = response.iter_content(chunk_size=1024 * 1024)
            for data in chunk_iter:
                downloaded += file.write(data)
                percent = (downloaded / total) * 100
                print(f"\rDownloading {url} ... {percent:.2f}%", end="")
    else:
        raise Exception(f"Failed to download {url}")


def download_dx8(path: Path):
    if not path.exists():
        os.makedirs(path)
    if (path / "include").exists():
        return
    archive_path = path / "dx8sdk.exe"
    if not os.path.exists(archive_path) or archive_path.stat().st_size != DX8_SIZE:
        download(DX8_URL, archive_path)

    # these happen to be valid zips, too
    with ZipFile(archive_path, "r") as zip:
        zip.extractall(path)


def download_msvc(path: Path, vs_path: Path, vc_path: Path):
    if not path.exists():
        os.makedirs(path)
    if (vc_path / "bin" / "cl.exe").exists():
        return
    archive_path = path / "en_vs.net_pro_full.exe"
    if not os.path.exists(archive_path) or archive_path.stat().st_size != MSVC_SIZE:
        download(MSVC_URL, archive_path)
    with tempfile.TemporaryDirectory() as zipdir:
        zipdir = Path(zipdir)
        with ZipFile(archive_path, "r") as zip:
            _ = zip.extractall(zipdir)
        with tempfile.TemporaryDirectory() as tempdir:
            tempdir = Path(tempdir)
            if sys.platform == "win32":
                _ = subprocess.check_call(
                    [
                        "msiexec",
                        "/a",
                        zipdir / "VS_SETUP.MSI",
                        "/qb",
                        f"TARGETDIR={tempdir}",
                    ]
                )
                for file in (tempdir / "Program Files" / "Microsoft Visual Studio .NET" / "Vc7" / "PlatformSDK" / "common").iterdir():
                    _ = shutil.move(file, tempdir / "Program Files" / "Microsoft Visual Studio .NET" / "Vc7" / "PlatformSDK")
            else:
                _ = subprocess.check_call(
                    [
                        "wine",
                        "msiexec",
                        "/a",
                        conv_path_backslash(zipdir / "VS_SETUP.MSI"),
                        "/qb",
                        f"TARGETDIR={conv_path_backslash(tempdir)}",
                    ]
                )
            _ = shutil.copytree(
                tempdir / "Program Files",
                path / "Program Files",
            )

    for file in (vc_path / "include").iterdir():
        _ = shutil.move(file, file.with_name(file.name.lower()))
    for file in (vc_path / "PlatformSDK" / "Include").iterdir():
        _ = shutil.move(file, file.with_name(file.name.lower()))
    # cl dll dependencies
    _ = (vs_path / "Common7" / "IDE" / "mspdb70.dll").rename(
        vc_path / "bin" / "mspdb70.dll"
    )
    _ = (vs_path / "Common7" / "IDE" / "msobj10.dll").rename(
        vc_path / "bin" / "msobj10.dll"
    )
    _ = (vs_path / "Common7" / "Packages" / "Debugger" / "msvcr70.dll").rename(
        vc_path / "bin" / "msvcr70.dll"
    )


# provides pragma var_order
def install_hackery(cl_path: Path, msvc_path: Path, vc_path: Path):
    c1xx_path = vc_path / "bin" / "c1xx.dll"
    orig_path = vc_path / "bin" / "c1xxorig.dll"

    if orig_path.exists() and c1xx_path.exists():
        return
    os.chdir(msvc_path)
    download(HACKERY_URL, msvc_path / "hackery.cpp")
    if not orig_path.exists():
        _ = shutil.copy(c1xx_path, orig_path)
    _ = run_program(
        str(cl_path),
        f'/I"{vc_path / "include"}"',
        f'/I"{vc_path / "PlatformSDK" / "Include"}"',
        "/LD",
        conv_path(msvc_path / "hackery.cpp"),
        "/link",
        f"/LIBPATH:{vc_path / 'lib'}",
        f"/LIBPATH:{vc_path / 'PlatformSDK' / 'lib'}",
        f"/OUT:{msvc_path / 'c1xx.dll'}",
    )
    _ = (msvc_path / "c1xx.dll").replace(c1xx_path)
