#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise VM supervisor shutdown with small processes, without a VM or disk."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / 'vm'))
from windows_setup import run_vm

READY_TIMEOUT_SECONDS = 5
CASE_TIMEOUT_SECONDS = 15
CHILD_LIFETIME_SECONDS = 30
POLL_SECONDS = 0.02
FAILURE_EXIT_CODE = 7


def child(output, kind, mode):
    if mode == 'ignore-term':
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    (output / (kind + '.pid')).write_text(str(os.getpid()))
    if mode == 'failure':
        sys.exit(FAILURE_EXIT_CODE)
    if kind == 'vm':
        if mode == 'success':
            return
        if mode == 'signal':
            os.kill(os.getpid(), signal.SIGUSR1)
        time.sleep(CHILD_LIFETIME_SECONDS)
    else:
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(output / 'tpm.sock'))
            server.listen()
            time.sleep(CHILD_LIFETIME_SECONDS)


def runner(output, vm_mode, tpm_mode):
    command = [sys.executable, __file__, '--child', str(output)]
    timeout = 0.2 if vm_mode == 'timeout' else CASE_TIMEOUT_SECONDS
    vm_command = command + ['vm', vm_mode]
    if vm_mode == 'missing-program':
        vm_command = [str(output / 'missing-program')]
    run_vm(output, command + ['tpm', tpm_mode], vm_command, timeout=timeout)


def records(output):
    path = output / 'process-status.jsonl'
    if not path.exists():
        return []
    # The supervisor may still be appending its next record.
    return [json.loads(line) for line in path.read_text().splitlines(keepends=True) if line.endswith('\n')]


def wait_ready(output, process, phase=None):
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        ready = (output / 'vm.pid').exists()
        if phase is not None:
            ready = any(record['phase'] == phase for record in records(output))
        if ready:
            return
        if process.poll() is not None:
            raise AssertionError('Runner exited before ' + str(phase or 'VM readiness'))
        time.sleep(POLL_SECONDS)
    raise AssertionError('Timed out waiting for ' + str(phase or 'VM readiness'))


class VmSupervisionTest(unittest.TestCase):
    def run_case(self, vm_mode='success', tpm_mode='wait', interrupt_signal=None,
                 repeat=False, interrupt_phase=None):
        with tempfile.TemporaryDirectory(prefix='rufus-vm-supervision-') as temporary:
            output = Path(temporary)
            children_reaped = False
            with (output / 'runner.log').open('wb') as log:
                process = subprocess.Popen([sys.executable, __file__, '--runner', str(output), vm_mode, tpm_mode],
                                           stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    if interrupt_signal is not None:
                        wait_ready(output, process, interrupt_phase)
                        process.send_signal(interrupt_signal)
                        if repeat:
                            wait_ready(output, process, 'cleanup_started')
                            process.send_signal(interrupt_signal)
                    result = process.wait(timeout=CASE_TIMEOUT_SECONDS)
                    history = records(output)
                    self.assertTrue(history, (output / 'runner.log').read_text())
                    self.assertEqual(history[-1]['phase'], 'stopped')
                    for name in ('vm', 'tpm'):
                        pid = history[-1][name + '_pid']
                        if pid is not None:
                            self.assertIsNotNone(history[-1][name + '_exit_code'])
                            with self.assertRaises(ProcessLookupError, msg=name + ' survived runner exit'):
                                os.kill(pid, 0)
                    children_reaped = True
                    return result, history
                finally:
                    # Limit a failing regression to these disposable children.
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    if not children_reaped:
                        for name in ('vm.pid', 'tpm.pid'):
                            path = output / name
                            if path.exists():
                                try:
                                    os.kill(int(path.read_text()), signal.SIGKILL)
                                except ProcessLookupError:
                                    pass

    def test_normal_exit(self):
        result, history = self.run_case()
        self.assertEqual(result, 0)
        self.assertEqual([entry['phase'] for entry in history],
                         ['running', 'vm_exited', 'cleanup_started', 'stopped'])
        self.assertEqual(history[1]['vm_exit_code'], 0)
        self.assertIsNone(history[-1]['error'])

    def test_failed_vm(self):
        for mode, code in (('failure', FAILURE_EXIT_CODE), ('signal', -signal.SIGUSR1)):
            with self.subTest(mode=mode):
                result, history = self.run_case(vm_mode=mode)
                self.assertNotEqual(result, 0)
                self.assertEqual(history[1]['phase'], 'vm_exited')
                self.assertEqual(history[1]['vm_exit_code'], code)
                self.assertTrue(history[-1]['error'].startswith('RuntimeError:'))

    def test_timeout(self):
        result, history = self.run_case(vm_mode='timeout')
        self.assertNotEqual(result, 0)
        self.assertTrue(history[-1]['error'].startswith('TimeoutExpired:'))
        self.assertEqual(history[-1]['vm_exit_code'], -signal.SIGTERM)

    def test_start_failure(self):
        result, history = self.run_case(tpm_mode='failure')
        self.assertNotEqual(result, 0)
        self.assertIsNone(history[-1]['vm_pid'])
        self.assertEqual(history[-1]['tpm_exit_code'], FAILURE_EXIT_CODE)
        result, history = self.run_case(vm_mode='missing-program')
        self.assertNotEqual(result, 0)
        self.assertTrue(history[-1]['error'].startswith('FileNotFoundError:'))
        self.assertEqual(history[-1]['tpm_exit_code'], -signal.SIGTERM)

    def test_interrupt(self):
        for signum in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signum=signum):
                result, history = self.run_case(vm_mode='wait', interrupt_signal=signum)
                self.assertIn(result, (128 + signum, -signum))
                self.assertIsNotNone(history[-1]['error'])
                self.assertEqual(history[-1]['vm_exit_code'], -signal.SIGTERM)

    def test_interrupt_during_cleanup(self):
        result, history = self.run_case(vm_mode='ignore-term', interrupt_signal=signal.SIGTERM, repeat=True)
        self.assertEqual(result, 128 + signal.SIGTERM)
        self.assertEqual(history[-1]['vm_exit_code'], -signal.SIGKILL)
        self.assertEqual(history[-1]['tpm_exit_code'], -signal.SIGTERM)

    def test_first_interrupt_during_normal_cleanup(self):
        result, history = self.run_case(tpm_mode='ignore-term', interrupt_signal=signal.SIGTERM,
                                       interrupt_phase='cleanup_started')
        # The VM succeeded, but a pending interrupt must not become a success
        # when it is delivered after stopping the unresponsive TPM.
        self.assertEqual(result, 128 + signal.SIGTERM)
        self.assertEqual(history[-1]['vm_exit_code'], 0)
        self.assertEqual(history[-1]['tpm_exit_code'], -signal.SIGKILL)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--child':
        child(Path(sys.argv[2]), sys.argv[3], sys.argv[4])
    elif len(sys.argv) > 1 and sys.argv[1] == '--runner':
        runner(Path(sys.argv[2]), sys.argv[3], sys.argv[4])
    else:
        unittest.main()
