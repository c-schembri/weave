import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from support.common import ROOT, read_json
from support.gate_process import run_owned


def alive(pid):
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return False
    try:
        return kernel.WaitForSingleObject(handle, 0) == 258
    finally:
        kernel.CloseHandle(handle)


@unittest.skipUnless(os.name == "nt", "Windows Job Object ownership tests")
class ProcessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="weave-process-", suffix=" with spaces")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = ROOT / "tests/support/gate_process_fixture.py"

    def run_fixture(self, mode, *, timeout=5000, arguments=()):
        return run_owned(sys.executable, [str(self.fixture), "--mode", mode, "--directory", str(self.root), *arguments],
                         self.root, self.root / "run.log", timeout)

    def assert_tree_stopped(self):
        for name in ("parent", "child"):
            pid = int((self.root / f"{name}.pid").read_text())
            deadline = time.monotonic() + 1
            while alive(pid) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(alive(pid), f"Owned {name} survived cleanup")

    def test_exit_and_log(self):
        self.assertEqual(self.run_fixture("exit"), 7)
        self.assertIn("expected exit", (self.root / "run.log").read_text())

    def test_argument_quoting(self):
        arguments = ["", "two words", 'quote"inside', "trailing\\", "space and trailing\\", "a&b|c", "caf\u00e9"]
        self.assertEqual(self.run_fixture("args", arguments=arguments), 0)
        self.assertEqual(json.loads((self.root / "run.log").read_text()), arguments)

    def test_deadline_kills_descendants(self):
        start = time.monotonic()
        self.assertEqual(self.run_fixture("hang", timeout=2000), -1)
        self.assertLess(time.monotonic() - start, 4)
        self.assert_tree_stopped()

    def test_descendants_reaped_after_normal_parent_exit(self):
        self.assertEqual(self.run_fixture("orphan"), 0)
        self.assert_tree_stopped()

    def test_supervisor_death_kills_descendants(self):
        supervisor = subprocess.Popen([sys.executable, str(self.fixture), "--mode", "supervisor", "--directory", str(self.root)],
                                      creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / "child.pid").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((self.root / "child.pid").exists())
        finally:
            supervisor.kill()
            supervisor.wait(timeout=3)
        self.assert_tree_stopped()

    def test_failure_and_log_preservation(self):
        with self.assertRaises(OSError):
            run_owned(str(self.root / "missing.exe"), [], self.root, self.root / "run.log", 1000)
        with self.assertRaises(FileExistsError):
            self.run_fixture("exit")
        self.assertEqual(run_owned(sys.executable, [], self.root, self.root / "untouched.log", 0), -1)
        self.assertFalse((self.root / "untouched.log").exists())

    def test_runner_setup_failure_publishes_evidence(self):
        output = self.root / "evidence"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/run_concurrent_gate.py"), "--build-directory",
                                 str(self.root / "missing-build"), "--output-directory", str(output)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(read_json(output / "gate.json")["decision"], "invalid")
        self.assertEqual(read_json(output / "run.json")["exit_code"], 3)


if __name__ == "__main__":
    unittest.main()
