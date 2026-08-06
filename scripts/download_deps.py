import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PureWindowsPath
from typing import Iterator, Optional
from zipfile import ZipFile

import requests

DX8_URL = "https://archive.org/download/dx8sdk/dx8sdk.exe"
MSVC_URL = "https://archive.org/download/en_vs.net_pro_full/en_vs.net_pro_full.exe"
HACKERY_URL = "https://gist.githubusercontent.com/EstexNT/e98a1384b906a3eedaaa3eeb7e58cd9d/raw/822536a26025f0df8763f1112d89bb1514f6209c/hackery.cpp"
DX8_SIZE = 144441256
MSVC_SIZE = 1706945024
# from archive.org's own <item>_files.xml metadata; a size check alone does not
# catch an archive that got stitched together wrong
DX8_SHA1 = "79935e264d969941ed74c37392f54e6bd1cc399c"
MSVC_SHA1 = "c4a52ea9a7986a47965c561d9589cc49ddef14d3"


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


class IncompleteDownload(Exception):
    pass


class CorruptDownload(Exception):
    pass


def sha1_of(path: Path) -> str:
    digest = hashlib.sha1()
    with open(path, "rb") as file:
        while chunk := file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def download(
    url: str,
    dest_path: Path,
    expected_size: Optional[int] = None,
    expected_sha1: Optional[str] = None,
    attempts: int = 10,
):
    """Download `url` to `dest_path`.

    The archives live on archive.org, which frequently drops multi-hundred-MB
    transfers partway through. The download therefore goes to a `.part` file
    that is resumed with a Range request on failure, and only renamed into
    place once it is complete and matches `expected_sha1`.

    Returns immediately if `dest_path` already holds a verified copy.
    """
    part_path = dest_path.with_name(dest_path.name + ".part")

    if expected_sha1 is not None and dest_path.exists():
        print(f"Verifying {dest_path.name} ...")
        if sha1_of(dest_path) == expected_sha1:
            return
        # Wrong contents. If it is short, it is a partial transfer worth
        # resuming; otherwise it is damaged and has to be fetched again.
        if expected_size is not None and dest_path.stat().st_size < expected_size:
            print(f"{dest_path.name} is incomplete, resuming")
            if not part_path.exists():
                _ = dest_path.replace(part_path)
        else:
            print(f"{dest_path.name} is corrupt, downloading again")
            dest_path.unlink()
            part_path.unlink(missing_ok=True)
    elif (
        not part_path.exists()
        and dest_path.exists()
        and expected_size is not None
        and dest_path.stat().st_size < expected_size
    ):
        # Pick up a partial file left behind by an older, non-resuming download.
        _ = dest_path.replace(part_path)

    for attempt in range(1, attempts + 1):
        downloaded = part_path.stat().st_size if part_path.exists() else 0
        headers = {"Range": f"bytes={downloaded}-"} if downloaded else {}
        try:
            with requests.get(
                url, stream=True, headers=headers, timeout=(30, 60)
            ) as response:
                if response.status_code == 206:
                    mode = "ab"
                elif response.status_code == 200:
                    # server ignored the range request, start over
                    downloaded = 0
                    mode = "wb"
                else:
                    raise Exception(
                        f"Failed to download {url} (HTTP {response.status_code})"
                    )

                total = expected_size or (
                    int(response.headers.get("content-length", 0)) + downloaded
                )
                with open(part_path, mode) as file:
                    chunk_iter: Iterator[bytes] = response.iter_content(
                        chunk_size=1024 * 1024
                    )
                    for data in chunk_iter:
                        downloaded += file.write(data)
                        if total:
                            percent = (downloaded / total) * 100
                            print(
                                f"\rDownloading {url} ... {percent:.2f}%",
                                end="",
                            )
                        else:
                            print(
                                f"\rDownloading {url} ... {downloaded} bytes",
                                end="",
                            )

            if total and downloaded < total:
                raise IncompleteDownload(f"got {downloaded} of {total} bytes")

            print()
            if expected_sha1 is not None:
                print(f"Verifying {dest_path.name} ...")
                actual = sha1_of(part_path)
                if actual != expected_sha1:
                    # resuming would only preserve the damage
                    part_path.unlink()
                    raise CorruptDownload(
                        f"sha1 {actual}, expected {expected_sha1}"
                    )

            _ = part_path.replace(dest_path)
            return
        except (
            requests.RequestException,
            IncompleteDownload,
            CorruptDownload,
            OSError,
        ) as e:
            if attempt == attempts:
                raise
            delay = min(2**attempt, 30)
            print(f"\nTransfer failed ({e}); retrying in {delay}s "
                  f"[attempt {attempt}/{attempts}]")
            time.sleep(delay)


def download_dx8(path: Path):
    if not path.exists():
        os.makedirs(path)
    # A marker rather than a directory check: an extraction interrupted partway
    # still leaves `include` behind, and that would never be repaired.
    marker = path / ".extracted"
    if marker.exists():
        return
    # Machines that installed before the marker existed have no way to prove it
    # any more, and re-extracting over a working toolchain fails on the
    # read-only files it laid down. Accept the old evidence once and record it.
    if (path / "include").exists() and (path / "lib").exists():
        marker.touch()
        return
    archive_path = path / "dx8sdk.exe"
    download(DX8_URL, archive_path, DX8_SIZE, DX8_SHA1)

    # these happen to be valid zips, too
    with ZipFile(archive_path, "r") as zip:
        zip.extractall(path)
    marker.touch()


def download_msvc(path: Path, vs_path: Path, vc_path: Path):
    if not path.exists():
        os.makedirs(path)
    # cl.exe appears before the fixups below have run, so it cannot stand in
    # for "the toolchain is ready" -- see the note in download_dx8.
    marker = path / ".installed"
    if marker.exists():
        return
    # See download_dx8: an install that predates the marker is still a good
    # install. The lowercased include directory and the three relocated DLLs
    # are what the fixups below produce, so their presence means those ran.
    if (vc_path / "bin" / "cl.exe").exists() and all(
        (vc_path / "bin" / name).exists()
        for name in ("mspdb70.dll", "msobj10.dll", "msvcr70.dll")
    ):
        marker.touch()
        return
    archive_path = path / "en_vs.net_pro_full.exe"
    download(MSVC_URL, archive_path, MSVC_SIZE, MSVC_SHA1)
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
                dirs_exist_ok=True,
            )

    for file in (vc_path / "include").iterdir():
        _ = shutil.move(file, file.with_name(file.name.lower()))
    for file in (vc_path / "PlatformSDK" / "Include").iterdir():
        _ = shutil.move(file, file.with_name(file.name.lower()))
    # cl dll dependencies
    for src, name in [
        (vs_path / "Common7" / "IDE" / "mspdb70.dll", "mspdb70.dll"),
        (vs_path / "Common7" / "IDE" / "msobj10.dll", "msobj10.dll"),
        (
            vs_path / "Common7" / "Packages" / "Debugger" / "msvcr70.dll",
            "msvcr70.dll",
        ),
    ]:
        # skip the ones a previous interrupted run already moved
        if src.exists():
            _ = src.rename(vc_path / "bin" / name)

    marker.touch()


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
