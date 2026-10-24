# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare shared DOS content with frozen Rufus output, using explicit host facts."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

FILES = ("CONFIG.SYS", "AUTOEXEC.BAT")


def invoke(probe, directory, keyboard, codepage, freedos, error=0):
    result = subprocess.run([probe, str(directory), keyboard, str(codepage), str(int(freedos))],
                            capture_output=True, timeout=5)
    assert result.returncode == error, (keyboard, codepage, freedos, result.stdout, result.stderr)
    return result.stdout.decode()


def read_files(directory, reference=False):
    files = {}
    for name in FILES:
        path = Path(str(directory) + "\\" + name) if reference else directory / name
        if path.exists():
            data = path.read_bytes()
            if reference:
                # The original uses Windows text streams. Its frozen native
                # oracle emits LF; emulate only the CRT's LF->CRLF translation.
                assert b"\r" not in data
                data = data.replace(b"\n", b"\r\n")
            files[name] = data
    return files


def compare(args, root, number, keyboard, codepage, freedos):
    directory = root / str(number)
    directory.mkdir()
    output = directory / "shared"
    output.mkdir()
    oracle = directory / "reference"
    selection = invoke(args.probe, output, keyboard, codepage, freedos)
    invoke(args.reference, oracle, keyboard, codepage, freedos)
    expected = read_files(oracle, reference=True)
    actual = read_files(output)
    corrected = freedos and codepage in (30003, 30029, 30030, 58335)
    if corrected:
        old_command = b"\\locale\\ega12) > NUL\r\n"
        assert old_command in expected["AUTOEXEC.BAT"]
        expected["AUTOEXEC.BAT"] = expected["AUTOEXEC.BAT"].replace(old_command, b"\\locale\\ega12.cpx) > NUL\r\n")
    assert actual == expected, (keyboard, codepage, freedos, actual, expected)
    for data in actual.values():
        without_endings = data.replace(b"\r\n", b"")
        assert b"\n" not in without_endings and b"\r" not in without_endings and b"\0" not in data
    if "CONFIG.SYS" in actual:
        assert b"MENUDEFAULT=1,5\r\n" in actual["CONFIG.SYS"]
        assert b"GOTO %CONFIG%\r\n" in actual["AUTOEXEC.BAT"]
        if freedos:
            assert b"\xcd" * 31 in actual["CONFIG.SYS"]
            assert b"display con=(ega,,1)\r\n" in actual["AUTOEXEC.BAT"]
            payload_root = Path(__file__).resolve().parents[2] / "res/freedos"
            for field in ("DRIVER", "EGA"):
                name = re.search(rf"{field}=([^ ]+)", selection)[1]
                assert (payload_root / name.upper()).is_file(), name
    return actual


def check_fallbacks(args, root, keyboards, codepages):
    # Driver order and the distinction between defaulting to 850 and upgrading
    # a requested 850 to 858 are observable in these independently fixed cases.
    cases = [("us", 437, True, "us", 437, "keyboard.sys", False, False),
             ("ro", 850, True, "ro", 858, "keybrd2.sys", False, False),
             ("ro", 65001, True, "ro", 850, "keybrd2.sys", False, True),
             ("ur", 0, True, "ur", 850, "keybrd3.sys", False, True),
             ("ar", 0, True, "ar", 853, "keybrd4.sys", False, True),
             ("unknown", 850, True, "us", 858, "keyboard.sys", True, False),
             ("sl", 852, True, "us", 852, "keyboard.sys", True, False),
             ("sl", 852, False, "sl", 852, "keyboard.sys", False, False),
             ("ro", 850, False, "ro", 850, "keybrd2.sys", False, False)]
    for number, (kb, cp, freedos, expected_kb, expected_cp, driver, kb_fallback, cp_fallback) in enumerate(cases):
        directory = root / f"fallback-{number}"
        directory.mkdir()
        report = invoke(args.probe, directory, kb, cp, freedos)
        assert report.startswith(f"KB={expected_kb} CP={expected_cp} DRIVER={driver} "), report
        assert f"KB_FALLBACK={int(kb_fallback)} CP_FALLBACK={int(cp_fallback)} " in report, report
        assert (directory / "CONFIG.SYS").exists() == (expected_kb != "us" or expected_cp != 437)
    # Some original driver-supported codes lack human-readable labels. Display
    # their canonical ID instead of asserting or formatting a NULL pointer.
    for freedos, missing in keyboards.items():
        for keyboard in sorted(missing):
            directory = root / f"missing-label-{freedos}-{keyboard}"
            directory.mkdir()
            report = invoke(args.probe, directory, keyboard, 437, freedos)
            assert f"KB={keyboard} CP=437" in report and f"NAME={keyboard}\n" in report, report
            assert f"Use {keyboard} keyboard".encode() in (directory / "CONFIG.SYS").read_bytes()
    assert len(codepages) > 100  # Frozen oracle inventory, not the new implementation.
    return len(cases) + sum(len(missing) for missing in keyboards.values())


def check_write_errors(args, root):
    subprocess.run([args.probe, "--errors"], check=True, timeout=5)
    for name in FILES:
        directory = root / ("error-" + name)
        directory.mkdir()
        # A buffered write can appear successful until fclose. The caller must
        # reject that error as well as immediate stream errors (/dev/full).
        (directory / name).symlink_to("/dev/full")
        invoke(args.probe, directory, "ro", 852, True, error=1)
        if name == "CONFIG.SYS":
            assert not (directory / "AUTOEXEC.BAT").exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    parser.add_argument("reference")
    args = parser.parse_args()
    original = Path(__file__).with_name("dos_locale_reference.c").read_text()
    # Keep the regression input domain independent of the extracted policy.
    labels = set(re.findall(r'^\s*\{"([a-z]{2})",', original, flags=re.M))
    codepages = sorted(set(int(cp) for cp in re.findall(r'^\s*\{\s*(\d+),', original, flags=re.M)))
    keyboards = {False: set(), True: set()}
    for match in re.finditer(r'static const char\* (ms|fd)_kb\d\[\] = \{(.*?)\};', original, flags=re.S):
        keyboards[match[1] == "fd"].update(re.findall(r'"([a-z]{2})"', match[2]))
    for freedos in keyboards:
        keyboards[freedos] -= labels
    cases = set()
    for freedos in (False, True):
        for keyboard in sorted(labels | {"unknown", "", "$(touch bad)"}):
            for codepage in (437, 0):
                cases.add((keyboard, codepage, freedos))
        for codepage in codepages + [0, 65535, 4294967295]:
            cases.add(("ro", codepage, freedos))
    with tempfile.TemporaryDirectory(prefix="rufus-dos-locale-") as temporary:
        root = Path(temporary)
        digest = hashlib.sha256()
        for number, (keyboard, codepage, freedos) in enumerate(sorted(cases)):
            files = compare(args, root, number, keyboard, codepage, freedos)
            for name, data in sorted(files.items()):
                digest.update(name.encode() + b"\0" + data)
        fallbacks = check_fallbacks(args, root, keyboards, codepages)
        check_write_errors(args, root)
    print(f"{len(cases)} DOS byte comparisons passed (four EGA12 filename corrections), "
          f"{fallbacks} explicit fallback/label cases, plus stream/close/argument errors. "
          f"Output SHA256={digest.hexdigest()}; temporary files removed.")


if __name__ == "__main__":
    main()
