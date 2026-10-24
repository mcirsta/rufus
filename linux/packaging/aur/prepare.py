#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD
"""Prepare the AUR submission files from the exact reviewed release source."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
from urllib.parse import urlsplit


def source_identity(archive):
    with archive.open("rb") as source_file:
        digest = hashlib.file_digest(source_file, "sha256").hexdigest()
    with tarfile.open(archive, "r:*") as source:
        members = source.getmembers()
        roots = set()
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or not path.parts:
                raise ValueError("source archive has an unsafe member path")
            roots.add(path.parts[0])
        if len(roots) != 1:
            raise ValueError("source archive must contain one top-level directory")
        root = roots.pop()
        if not re.fullmatch(r"[A-Za-z0-9._+-]+", root):
            raise ValueError("source directory must use a plain release name")
        required = ("linux/CMakeLists.txt", "linux/wimlib/native-input.patch",
                    "linux/wimlib/native-ntfs.patch", "src/rufus.rc",
                    "res/loc/rufus.loc", "docs/linux-support.md", "LICENSE.txt")
        for name in required:
            if not source.getmember(f"{root}/{name}").isfile():
                raise ValueError(f"missing source file: {name}")
        cmake = source.extractfile(f"{root}/linux/CMakeLists.txt").read().decode("utf-8")
        resource = source.extractfile(f"{root}/src/rufus.rc").read()
        revision = re.search(r"set\(RUFUS_PREVIEW_REVISION ([0-9]+)\)", cmake)
        upstream = re.search(rb"FILEVERSION ([0-9]+),([0-9]+),([0-9]+),", resource)
        if not revision or not upstream:
            raise ValueError("cannot identify the application's release version")
        version = ".".join(part.decode("ascii") for part in upstream.groups())
        version += ".preview" + revision.group(1)
    return {"sha256": digest, "root": root, "version": version}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", required=True, type=Path)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--maintainer", help="AUR maintainer name and contact, as approved by the owner")
    args = parser.parse_args()
    url = urlsplit(args.source_url)
    plain_url = re.fullmatch(r"[A-Za-z0-9:/?&=._%+~#@-]+", args.source_url)
    if url.scheme != "https" or not url.netloc or not plain_url:
        parser.error("--source-url must be a plain HTTPS release URL")
    if url.username or url.password or url.fragment:
        parser.error("--source-url cannot contain credentials or a fragment")
    if not re.fullmatch(r"[A-Za-z0-9._+-]+\.tar\.(gz|xz|zst)", args.source_archive.name):
        parser.error("source archive must have a plain release tarball filename")
    if args.maintainer and ("\n" in args.maintainer or "\r" in args.maintainer):
        parser.error("--maintainer must fit on one comment line")
    try:
        identity = source_identity(args.source_archive)
    except (ValueError, KeyError, OSError, tarfile.TarError, UnicodeError) as error:
        parser.error(str(error))
    location = Path(__file__).resolve().parent
    recipe = (location / "PKGBUILD.in").read_text()
    substitutions = {
        "PACKAGE_VERSION": identity["version"],
        "SOURCE_ROOT": identity["root"],
        "SOURCE_FILENAME": args.source_archive.name,
        "SOURCE_URL": args.source_url,
        "SOURCE_SHA256": identity["sha256"],
    }
    for name, value in substitutions.items():
        recipe = recipe.replace(f"@{name}@", value)
    if args.maintainer:
        recipe = recipe.replace(
            "Set the release owner's name and contact before AUR submission.", args.maintainer)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "PKGBUILD").write_text(recipe)
    shutil.copyfile(location / "LICENSE", args.output / "LICENSE")
    subprocess.run(["bash", "-n", "PKGBUILD"], cwd=args.output, check=True)
    result = subprocess.run(["makepkg", "--printsrcinfo"], cwd=args.output,
                            check=True, capture_output=True, text=True)
    (args.output / ".SRCINFO").write_text(result.stdout)
    identity["source_url"] = args.source_url
    identity["maintainer_set"] = bool(args.maintainer)
    print(json.dumps(identity, indent=2))


if __name__ == "__main__":
    main()
