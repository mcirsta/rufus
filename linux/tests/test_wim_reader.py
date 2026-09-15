# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare system/extended upstream WIM metadata in files and selected ISO/UDF views."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

from test_udf_images import make_image, rename_udf_entry
from test_udf_embedded_efi import fragment_file

runs = 0


def run(command, status=0):
    global runs
    result = subprocess.run([str(part) for part in command], capture_output=True, timeout=15)
    runs += 1
    assert result.returncode == status, (command, result.returncode, result.stdout, result.stderr)
    metadata = json.loads(result.stdout) if result.stdout else None
    metrics = dict((key.decode(), int(value)) for key, value in
                   re.findall(rb"(ERROR|READS|MAX_BLOCKS|ISO_READS)=(\d+)", result.stderr))
    if "MAX_BLOCKS" in metrics:
        assert metrics["MAX_BLOCKS"] <= 16, metrics
        if command[2] == "udf":
            assert metrics["ISO_READS"] == 0, metrics
    return metadata, metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reader", type=Path)
    parser.add_argument("system", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-wim-reader-") as temporary:
        directory = Path(temporary)
        contents, expected = {}, {}
        for name, version, mode in (("install", "win11", None), ("otherxx", "win7", None),
                                     ("pipable", "win11", "pipable"), ("solid", "win11", "solid"),
                                     ("integrity", "win11", "integrity")):
            file = directory / (name + ".wim")
            command = [args.fixture, file, version]
            if mode:
                command.append(mode)
            subprocess.run([str(part) for part in command], check=True, timeout=15)
            system, _ = run([args.system, file])
            upstream, _ = run([args.upstream, file])
            assert upstream == system, (name, upstream, system)
            contents[name] = file.read_bytes()
            expected[name] = system
        entries = {"/Sources/" + name + ".wim": data for name, data in contents.items()}
        entries.update({"/Sources/corrupt.wim": b"not a WIM", "/Sources/empty.wim": b"",
                        "/Sources/truncated.wim": contents["install"][:-200],
                        "/ș-é/Install.wim": contents["install"]})
        image = make_image(directory, "WIM_READER", entries, args.mkisofs)
        for view in ("rr", "joliet", "udf"):
            for name in contents:
                command = [args.reader, image, view, "/Sources/" + name + ".wim"]
                if name == "integrity":
                    command.append("integrity")
                actual, _ = run(command)
                assert actual == expected[name], (view, name, actual)
            actual, _ = run([args.reader, image, view, "/ș-é/Install.wim"])
            assert actual == expected["install"]
            for name in ("corrupt", "empty", "truncated"):
                actual, metrics = run([args.reader, image, view, "/Sources/" + name + ".wim"], status=1)
                assert actual is None and metrics["ERROR"] != 0, metrics
            for path in ("/Sources/missing.wim", "/Sources"):
                run([args.reader, image, view, path], status=3)
            command = [args.reader, image, view, "/Sources/install.wim"]
            _, metrics = run(command)
            reads = metrics["READS"]
            assert reads >= 3, metrics  # The XML crosses cache boundaries.
            for point in range(reads + 1):
                actual, metrics = run(command + ["cancel", point], status=2)
                assert actual is None and metrics["ERROR"] != 0, metrics
            for point in range(1, reads + 1):
                for fault in ("fail", "short"):
                    actual, metrics = run(command + [fault, point], status=1)
                    assert actual is None and metrics["ERROR"] != 0, metrics
            # A later open remains usable after each kind of injected failure.
            actual, _ = run(command)
            assert actual == expected["install"]

        rename_udf_entry(image, "install.wim", "movedxx.wim")
        rename_udf_entry(image, "otherxx.wim", "install.wim")
        actual, _ = run([args.reader, image, "udf", "/Sources/install.wim"])
        assert actual == expected["otherxx"]
        actual, _ = run([args.reader, image, "rr", "/Sources/install.wim"])
        assert actual == expected["install"]
        rename_udf_entry(image, "install.wim", "missing.wim")
        run([args.reader, image, "udf", "/Sources/install.wim"], status=3)

        padded = contents["install"] + bytes(-len(contents["install"]) % 2048)
        for long_ad in (False, True):
            image = make_image(directory, "FRAGMENT_" + str(int(long_ad)),
                               {"/Sources/install.wim": padded}, args.mkisofs)
            fragment_file(image, padded, long_ad)
            actual, _ = run([args.reader, image, "udf", "/Sources/install.wim"])
            assert actual == expected["install"]
            run([args.reader, image, "rr", "/Sources/install.wim"], status=1)
    print(f"WIM reader: {runs} probes pass; file/ISO/UDF metadata matches, selected views,")
    print("pipable/solid/integrity WIMs, fragmented UDF, failed reads and cancellation verified.")


if __name__ == "__main__":
    main()
