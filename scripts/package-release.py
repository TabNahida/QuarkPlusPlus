#!/usr/bin/env python3
"""Package an installed tree; only public distribution files are included."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "build" / "release"
PLATFORMS = {("windows", "x64"), ("linux", "x64"), ("macos", "x64"), ("macos", "arm64")}


def project_version() -> str:
    match = re.search(r'^set_version\("(\d+\.\d+\.\d+)"\)', (ROOT / "xmake.lua").read_text(), re.M)
    if not match:
        raise ValueError("xmake.lua must declare a numeric release version")
    version = match.group(1)
    ref = os.environ.get("RELEASE_REF", "")
    if ref.startswith("refs/tags/") and ref != f"refs/tags/v{version}":
        raise ValueError(f"Release tag does not match project version {version}")
    return version


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file() or source.is_symlink():
        raise ValueError(f"Missing or non-regular release input: {source.relative_to(ROOT)}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def package(platform: str, arch: str) -> None:
    if (platform, arch) not in PLATFORMS:
        raise ValueError("Unsupported release platform/architecture")
    version = project_version()
    prefix = f"quarkpp-{version}-{platform}-{arch}"
    commit = os.environ.get("SOURCE_COMMIT") or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("SOURCE_COMMIT must be a full Git commit identifier")
    epoch = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", commit], cwd=ROOT, text=True))
    installed = ROOT / "dist"
    example = json.loads((installed / "share/quarkpp/config/quarkpp.example.json").read_text(encoding="utf-8-sig"))
    if example.get("cookie"):
        raise ValueError("The distributed example configuration must not contain a cookie")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-", dir=ROOT / "build") as temporary:
        stage = Path(temporary) / prefix
        executable = "bin/quarkpp.exe" if platform == "windows" else "bin/quarkpp"
        library = "lib/quarkpp.lib" if platform == "windows" else "lib/libquarkpp.a"
        files = [installed / executable, installed / library]
        files += sorted((installed / "include/quarkpp").glob("*.hpp"))
        for required in ("client.hpp", "support.hpp", "auth.hpp"):
            if installed / "include/quarkpp" / required not in files:
                raise ValueError(f"Missing installed public header: {required}")
        files += [installed / "share/quarkpp" / name for name in ("README.md", "README.zh-CN.md", "LICENSE")]
        files += sorted((installed / "share/quarkpp/docs").glob("*.md"))
        files.append(installed / "share/quarkpp/config/quarkpp.example.json")
        for source in files:
            copy_file(source, stage / source.relative_to(installed))
        license_dir = stage / "share/quarkpp/licenses"
        for source in sorted((ROOT / "scripts/release-licenses").glob("*.txt")):
            copy_file(source, license_dir / source.name)
        for source in sorted((ROOT / "third_party").glob("**/LICENSE*")):
            copy_file(source, license_dir / (source.parent.name + "-" + source.name))
        build_info = {
            "project": "QuarkPlusPlus", "version": version, "commit": commit,
            "platform": platform, "architecture": arch,
            "source": f"https://github.com/TabNahida/QuarkPlusPlus/tree/{commit}",
            "dependencies": {"cpp-httplib": "0.34.0", "OpenSSL": "3.6.1", "nlohmann/json": "3.12.0"},
            "library_usage": "C++20; link the static library with OpenSSL and system threading/socket libraries; provide nlohmann/json headers. Match the build compiler ABI and runtime (Windows: MSVC /MT).",
            "runtime": "Windows x64 / macOS 15+ / Linux glibc 2.39+ with GCC 13 libstdc++ as indicated by the platform. OpenSSL is linked statically into the CLI.",
        }
        (stage / "BUILD-INFO.json").write_text(json.dumps(build_info, indent=2) + "\n", encoding="utf-8")
        archive = OUTPUT / (prefix + (".zip" if platform == "windows" else ".tar.gz"))
        if platform == "windows":
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
                for source in sorted(stage.rglob("*")):
                    if source.is_file():
                        output.write(source, source.relative_to(stage.parent).as_posix())
        else:
            with archive.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as zipped:
                with tarfile.open(fileobj=zipped, mode="w") as output:
                    for source in sorted(stage.rglob("*")):
                        info = output.gettarinfo(str(source), source.relative_to(stage.parent).as_posix())
                        info.uid = info.gid = 0
                        info.uname = info.gname = ""
                        info.mtime = epoch
                        if source.is_file():
                            with source.open("rb") as contents:
                                output.addfile(info, contents)
                        else:
                            output.addfile(info)
    checksum = f"{digest(archive)}  {archive.name}\n"
    archive.with_name(archive.name + ".sha256").write_text(checksum, encoding="ascii")
    print(checksum, end="")


def checksums() -> None:
    version = project_version()
    lines = []
    for platform, arch in sorted(PLATFORMS):
        extension = ".zip" if platform == "windows" else ".tar.gz"
        archive = OUTPUT / f"quarkpp-{version}-{platform}-{arch}{extension}"
        expected = f"{digest(archive)}  {archive.name}\n"
        if archive.with_name(archive.name + ".sha256").read_text(encoding="ascii") != expected:
            raise ValueError(f"Checksum mismatch: {archive.name}")
        lines.append(expected)
    (OUTPUT / "SHA256SUMS").write_text("".join(lines), encoding="ascii")
    print("Verified all four release archives")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    pack = subcommands.add_parser("package")
    pack.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    pack.add_argument("--arch", choices=("x64", "arm64"), required=True)
    subcommands.add_parser("checksums")
    args = parser.parse_args()
    if args.command == "package":
        package(args.platform, args.arch)
    else:
        checksums()
