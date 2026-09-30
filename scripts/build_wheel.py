"""Build the Windows Runtime wheel using only the Python standard library."""

from __future__ import annotations

import base64
import csv
import hashlib
import io
import tomllib
import zipfile
from pathlib import Path

NAME = "project_oreo_runtime"
TAG = "py3-none-win_amd64"

ROOT = Path(__file__).resolve().parents[1]


def _version() -> str:
    """One source of truth, read rather than repeated.

    The version used to be written out here as well as in pyproject.toml, __init__.py and
    package_release.ps1 - four copies, which is four chances to update three. That is not a
    hypothetical: 0.1.1 shipped reporting itself as 0.1.0 for exactly this reason, and the
    CHANGELOG still says so.
    """
    with open(ROOT / "pyproject.toml", "rb") as handle:
        return tomllib.load(handle)["project"]["version"]


VERSION = _version()
PACKAGE = ROOT / "src" / "oreo_runtime"
DIST = ROOT / "dist"
WHEEL_NAME = f"{NAME}-{VERSION}-{TAG}.whl"
DIST_INFO = f"{NAME}-{VERSION}.dist-info"

REQUIRED = (
    PACKAGE / "bin" / "oreo_overlay.dll",
    PACKAGE / "bin" / "oreo_controller_support.exe",
    PACKAGE / "bin" / "SDL2.dll",
    PACKAGE / "assets" / "fonts" / "gamefont.ttf",
    PACKAGE / "THIRD_PARTY_NOTICES.md",
)


def digest(data: bytes) -> str:
    encoded = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=")
    return "sha256=" + encoded.decode("ascii")


def main() -> None:
    missing = [str(path) for path in REQUIRED if not path.is_file()]
    license_dir = PACKAGE / "licenses"
    if not license_dir.is_dir() or not any(license_dir.iterdir()):
        missing.append(str(license_dir))
    if missing:
        raise SystemExit("Missing staged package files:\n" + "\n".join(missing))

    DIST.mkdir(parents=True, exist_ok=True)
    wheel_path = DIST / WHEEL_NAME
    if wheel_path.exists():
        wheel_path.unlink()

    files: dict[str, bytes] = {}
    for path in sorted(PACKAGE.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            archive_name = Path("oreo_runtime", *path.relative_to(PACKAGE).parts).as_posix()
            files[archive_name] = path.read_bytes()

    files[f"{DIST_INFO}/METADATA"] = (
        "Metadata-Version: 2.1\n"
        "Name: project-oreo-runtime\n"
        f"Version: {VERSION}\n"
        "Summary: Native overlay and controller runtime for No Man's Sky pyMHF mods\n"
        "Author: Carbonster\n"
        "Requires-Python: >=3.10\n"
    ).encode("utf-8")
    files[f"{DIST_INFO}/WHEEL"] = (
        "Wheel-Version: 1.0\n"
        "Generator: Project Oreo Runtime standard-library builder\n"
        "Root-Is-Purelib: false\n"
        f"Tag: {TAG}\n"
    ).encode("utf-8")
    files[f"{DIST_INFO}/top_level.txt"] = b"oreo_runtime\n"

    rows = [[name, digest(data), str(len(data))] for name, data in sorted(files.items())]
    record_name = f"{DIST_INFO}/RECORD"
    rows.append([record_name, "", ""])
    output = io.StringIO(newline="")
    csv.writer(output, lineterminator="\n").writerows(rows)
    files[record_name] = output.getvalue().encode("utf-8")

    with zipfile.ZipFile(wheel_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(name, data)

    print(wheel_path)


if __name__ == "__main__":
    main()
