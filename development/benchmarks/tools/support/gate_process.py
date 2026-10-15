"""Own the gate worker and its descendants for the entire wall-clock budget."""

import os
from pathlib import Path
import signal
import subprocess
import time


def run_owned(executable, arguments, directory, log, timeout_ms):
    """Return the worker exit code, or -1 on timeout. Never overwrite an existing log."""
    if timeout_ms <= 0:
        return -1
    deadline = time.monotonic() + timeout_ms / 1000
    if os.name == "nt":
        return run_windows(executable, arguments, directory, log, deadline)
    with Path(log).open("xb") as output:
        process = subprocess.Popen(
            [str(executable), *map(str, arguments)], cwd=directory, stdin=subprocess.DEVNULL,
            stdout=output, stderr=output, start_new_session=True,
        )
        try:
            return process.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            return -1
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=1)


def run_windows(executable, arguments, directory, log, deadline):
    import ctypes as c
    from ctypes import wintypes as w
    import msvcrt

    class BasicLimits(c.Structure):
        _fields_ = [("ProcessTime", c.c_int64), ("JobTime", c.c_int64), ("Flags", w.DWORD),
                    ("MinWorkingSet", c.c_size_t), ("MaxWorkingSet", c.c_size_t),
                    ("ActiveProcesses", w.DWORD), ("Affinity", c.c_size_t),
                    ("Priority", w.DWORD), ("Scheduling", w.DWORD)]

    class IoCounters(c.Structure):
        _fields_ = [(name, c.c_uint64) for name in ("ReadOps", "WriteOps", "OtherOps", "ReadBytes", "WriteBytes", "OtherBytes")]

    class ExtendedLimits(c.Structure):
        _fields_ = [("Basic", BasicLimits), ("Io", IoCounters), ("ProcessMemory", c.c_size_t),
                    ("JobMemory", c.c_size_t), ("PeakProcessMemory", c.c_size_t), ("PeakJobMemory", c.c_size_t)]

    class Startup(c.Structure):
        _fields_ = [("Size", w.DWORD), ("Reserved", w.LPWSTR), ("Desktop", w.LPWSTR), ("Title", w.LPWSTR),
                    *[(name, w.DWORD) for name in ("X", "Y", "Width", "Height", "XChars", "YChars", "Fill", "Flags")],
                    ("Show", w.WORD), ("ReservedSize", w.WORD), ("ReservedData", c.c_void_p),
                    ("Input", w.HANDLE), ("Output", w.HANDLE), ("Error", w.HANDLE)]

    class ProcessInfo(c.Structure):
        _fields_ = [("Process", w.HANDLE), ("Thread", w.HANDLE), ("ProcessId", w.DWORD), ("ThreadId", w.DWORD)]

    kernel = c.WinDLL("kernel32", use_last_error=True)

    def function(name, result, arguments):
        call = getattr(kernel, name)
        call.restype, call.argtypes = result, arguments
        return call

    create_job = function("CreateJobObjectW", w.HANDLE, [c.c_void_p, w.LPCWSTR])
    set_job = function("SetInformationJobObject", w.BOOL, [w.HANDLE, c.c_int, c.c_void_p, w.DWORD])
    assign_job = function("AssignProcessToJobObject", w.BOOL, [w.HANDLE, w.HANDLE])
    create_process = function("CreateProcessW", w.BOOL, [w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p,
                              w.BOOL, w.DWORD, c.c_void_p, w.LPCWSTR, c.POINTER(Startup), c.POINTER(ProcessInfo)])
    resume = function("ResumeThread", w.DWORD, [w.HANDLE])
    wait = function("WaitForSingleObject", w.DWORD, [w.HANDLE, w.DWORD])
    exit_code = function("GetExitCodeProcess", w.BOOL, [w.HANDLE, c.POINTER(w.DWORD)])
    terminate = function("TerminateProcess", w.BOOL, [w.HANDLE, w.UINT])
    terminate_job = function("TerminateJobObject", w.BOOL, [w.HANDLE, w.UINT])
    close = function("CloseHandle", w.BOOL, [w.HANDLE])

    def check(result):
        if not result:
            raise c.WinError(c.get_last_error())
        return result

    job = None
    process = ProcessInfo()
    assigned = False
    with Path(log).open("xb") as output, open(os.devnull, "rb") as input_file:
        try:
            job = check(create_job(None, None))
            limits = ExtendedLimits()
            limits.Basic.Flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            check(set_job(job, 9, c.byref(limits), c.sizeof(limits)))
            startup = Startup()
            startup.Size = c.sizeof(startup)
            startup.Flags = 0x100  # STARTF_USESTDHANDLES
            startup.Input = msvcrt.get_osfhandle(input_file.fileno())
            startup.Output = startup.Error = msvcrt.get_osfhandle(output.fileno())
            command = c.create_unicode_buffer(subprocess.list2cmdline([str(executable), *map(str, arguments)]))
            os.set_handle_inheritable(startup.Input, True)
            os.set_handle_inheritable(startup.Output, True)
            try:
                # Assign before any child code runs, so even immediate descendants belong to the job.
                check(create_process(str(executable), command, None, None, True, 0x08000004,
                                     None, str(directory), c.byref(startup), c.byref(process)))
            finally:
                os.set_handle_inheritable(startup.Input, False)
                os.set_handle_inheritable(startup.Output, False)
            check(assign_job(job, process.Process))
            assigned = True
            check(resume(process.Thread) != 0xFFFFFFFF)
            remaining = max(0, int((deadline - time.monotonic()) * 1000))
            status = wait(process.Process, remaining)
            if status == 258:  # WAIT_TIMEOUT
                return -1
            check(status == 0)
            code = w.DWORD()
            check(exit_code(process.Process, c.byref(code)))
            return code.value
        finally:
            # Closing the non-inherited job also kills descendants if the supervisor itself is killed.
            if job:
                terminate_job(job, 4)
            if process.Process:
                if not assigned:
                    terminate(process.Process, 4)
                wait(process.Process, 1000)
                close(process.Process)
            if process.Thread:
                close(process.Thread)
            if job:
                close(job)
