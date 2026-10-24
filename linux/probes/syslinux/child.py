#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fault installer/mcopy: keep descendants writing until supervision stops them."""
import os
from pathlib import Path
import signal
import time


def writer():
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    if os.environ["TEST_MODE"] == "cancel-closed":
        os.close(1)
        os.close(2)
    target = os.open("/proc/self/fd/3", os.O_RDWR)
    with open("pids", "a") as stream:
        stream.write(f"{os.getpid()}\n")
    Path("ready").touch()
    while True:
        os.pwrite(target, b"child write\n", 1024)
        time.sleep(0.005)


def main():
    assert os.getcwd() == os.environ["TMPDIR"] == os.environ["HOME"]
    assert os.environ["LC_ALL"] == "C.UTF-8"
    assert "RUFUS_POISON" not in os.environ
    assert "LD_PRELOAD" not in os.environ
    assert "PYTHONPATH" not in os.environ
    try:
        os.fstat(80)
    except OSError:
        pass
    else:
        raise AssertionError("Foreign descriptor inherited")
    mode = os.environ["TEST_MODE"]
    if mode == "clock-fail":
        # Keep the child alive until the runner observes the failed clock,
        # regardless of which process the scheduler runs first.
        while True:
            time.sleep(1)
    if mode in ("cancel-tree", "cancel-closed", "timeout", "orphan", "cancel-native"):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if os.fork() == 0:
            if os.fork() == 0:
                writer()
            # Leave a grandchild for the dedicated worker to adopt.
            os._exit(0)
        os.wait()
        while not Path("ready").exists():
            time.sleep(0.005)
        if mode == "orphan":
            return
        if mode == "cancel-closed":
            os.close(1)
            os.close(2)
        while True:
            time.sleep(1)
    target = os.open("/proc/self/fd/3", os.O_RDWR)
    os.pwrite(target, b"installed\n", 0)
    os.close(target)
    if mode == "failure":
        raise SystemExit(19)
    if mode == "signal":
        os.kill(os.getpid(), signal.SIGUSR1)
    if mode == "cancel-noisy":
        Path("ready").touch()
        while True:
            os.write(1, b"d" * 8192)
    print("installer completed", flush=True)


if __name__ == "__main__":
    main()
