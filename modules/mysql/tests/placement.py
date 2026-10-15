"""Optional logical CPU placement for this fixture and its inherited children."""

from contextlib import contextmanager
import ctypes
import os


def parse_cpus(text):
    parts = text.split(",")
    if not parts or any(not part.isascii() or not part.isdecimal() for part in parts):
        raise ValueError("CPUs must be a comma-separated list of nonnegative integers")
    cpus = frozenset(map(int, parts))
    if len(cpus) != len(parts):
        raise ValueError("CPU IDs must not repeat")
    return cpus


def windows_functions():
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.argtypes = []
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GetActiveProcessorGroupCount.argtypes = []
    kernel.GetActiveProcessorGroupCount.restype = wintypes.WORD
    kernel.GetProcessAffinityMask.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel.GetProcessAffinityMask.restype = wintypes.BOOL
    kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    kernel.SetProcessAffinityMask.restype = wintypes.BOOL
    if kernel.GetActiveProcessorGroupCount() != 1:
        raise RuntimeError("explicit placement requires a single Windows processor group")
    return kernel


def current_cpus():
    if os.name != "nt":
        return frozenset(os.sched_getaffinity(0))
    kernel = windows_functions()
    process, system = ctypes.c_size_t(), ctypes.c_size_t()
    if not kernel.GetProcessAffinityMask(kernel.GetCurrentProcess(), ctypes.byref(process), ctypes.byref(system)):
        raise ctypes.WinError(ctypes.get_last_error())
    return frozenset(cpu for cpu in range(process.value.bit_length()) if process.value & (1 << cpu))


def set_cpus(cpus):
    if os.name != "nt":
        os.sched_setaffinity(0, cpus)
    else:
        kernel = windows_functions()
        mask = sum(1 << cpu for cpu in cpus)
        if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), mask):
            raise ctypes.WinError(ctypes.get_last_error())
    if current_cpus() != cpus:
        raise RuntimeError("the OS did not apply the exact requested CPU placement")


def validate(client, server, available):
    if client is None and server is None:
        return
    if not client or not server:
        raise ValueError("client and server CPU sets must both be provided and nonempty")
    if client & server:
        raise ValueError("client and server logical CPU sets must not overlap")
    if not (client | server) <= available:
        raise ValueError("requested CPU IDs are outside this process's allowed set")


@contextmanager
def placed(cpus):
    if cpus is None:
        yield
        return
    original = current_cpus()
    if not cpus or not cpus <= original:
        raise ValueError("placement must be a nonempty subset of the current allowed CPUs")
    try:
        set_cpus(cpus)
        yield
    finally:
        set_cpus(original)
