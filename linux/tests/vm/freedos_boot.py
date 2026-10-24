# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot helper-created FreeDOS media under BIOS without changing its files."""
import json
import os
import re
import subprocess
import time

from boot_linux import firmware_options
from fat_extract import digest
from syslinux_alpine import qmp_read, qmp_command, read_screen, type_keys

BOOT_TIMEOUT_SECONDS = 90
COMMAND_TIMEOUT_SECONDS = 15
MARKER = "RUFUSDOSOK"


def wait_for_screen(process, output, pattern, timeout, name):
    deadline = time.monotonic() + timeout
    previous = None
    with (output / (name + "-screens.txt")).open("w") as log:
        while process.poll() is None and time.monotonic() < deadline:
            screen = read_screen(process, output)
            if screen != previous:
                log.write(screen + "\n\n")
                log.flush()
                previous = screen
            if re.search(pattern, screen, re.MULTILINE | re.IGNORECASE):
                (output / (name + ".txt")).write_text(screen + "\n")
                qmp_command(process, "screendump", {"filename": str(output / (name + ".png")), "format": "png"})
                return screen
            time.sleep(.1)
    raise RuntimeError("BIOS guest did not reach " + name)


def qualify_boot(args, base, disk):
    output = base / "freedos-bios"
    output.mkdir()
    before = digest(disk)
    command = ["qemu-system-x86_64", "-accel", args.accel,
               "-cpu", "host" if args.accel == "kvm" else "max", "-m", "64", "-vga", "std",
               "-display", "none", "-monitor", "none", "-serial", "none", "-no-reboot", "-nic", "none",
               "-qmp", "stdio", "-snapshot", "-device", "qemu-xhci,id=xhci",
               "-drive", "if=none,id=stick,format=raw,file=" + str(disk),
               "-device", "usb-storage,drive=stick,removable=on,bootindex=1"]
    command.extend(firmware_options(output, "bios", "freedos"))
    (output / "qemu-command.json").write_text(json.dumps(command, indent=2) + "\n")
    checks = ["ver", "echo " + MARKER]
    (output / "commands.txt").write_text("\n".join(checks) + "\n")
    with (output / "qemu.log").open("wb") as log:
        process = subprocess.Popen(command, env=dict(os.environ, TMPDIR=str(output)),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, bufsize=0)
        try:
            assert "QMP" in qmp_read(process)
            qmp_command(process, "qmp_capabilities")
            prompt = wait_for_screen(process, output, r"^C:\\>\s*$", BOOT_TIMEOUT_SECONDS, "prompt")
            assert "FreeCom version" in prompt, "Expected the bundled DOS command interpreter"
            selected = r"^Using US-English keyboard with US-English codepage \[437\]$" if args.freedos_win7 else \
                r"^Keyboard layout\s+: .+:RO \[852\]"
            assert re.search(selected, prompt, re.MULTILINE), "Explicit DOS startup choice was not loaded"
            type_keys(process, checks[0] + "\n")
            # Require the command, its response and the next prompt in order,
            # rather than accepting the same banner printed during startup.
            version = wait_for_screen(process, output, r"^C:\\>ver\n\nFreeCom version [^\n]+\nC:\\>\s*$",
                                      COMMAND_TIMEOUT_SECONDS, "version")
            # A separate output line is required; the typed command's echo does
            # not prove that the shell actually executed it.
            type_keys(process, checks[1] + "\n")
            wait_for_screen(process, output, "^" + MARKER + "$", COMMAND_TIMEOUT_SECONDS, "command")
            qmp_command(process, "quit")
            assert process.wait(timeout=5) == 0, "QEMU did not exit cleanly"
        finally:
            if process.poll() is None:
                try:
                    qmp_command(process, "screendump", {"filename": str(output / "stopped.png"), "format": "png"})
                except (OSError, RuntimeError, ValueError):
                    pass  # Failure evidence must never prevent VM cleanup.
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdin.close()
            process.stdout.close()
    assert digest(disk) == before, "BIOS boot changed the verified backing image"
    result = {"firmware": "bios", "fat_bits": args.fat_bits,
              "bootstrap": "win7" if args.freedos_win7 else "rufus", "disk_sha256": before,
              "version_screen": version, "command_output": MARKER, "passed": True}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS: helper-created FreeDOS USB reached its DOS prompt and executed VER/ECHO under BIOS.", flush=True)
    return result
