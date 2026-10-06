"""Read native Windows process counters without mixing client and server CPU."""

import ctypes as c
from ctypes import wintypes as w
import os


def counters(process):
    if os.name != "nt":
        raise ValueError("This benchmark currently requires Windows.")
    kernel = c.WinDLL("kernel32", use_last_error=True)
    times = kernel.GetProcessTimes
    times.argtypes = [w.HANDLE, *([c.POINTER(w.FILETIME)] * 4)]
    times.restype = w.BOOL
    cycle_time = kernel.QueryProcessCycleTime
    cycle_time.argtypes = [w.HANDLE, c.POINTER(c.c_uint64)]
    cycle_time.restype = w.BOOL
    created, exited, system, user = (w.FILETIME() for _ in range(4))
    cycles = c.c_uint64()
    handle = int(process._handle)
    if not times(handle, c.byref(created), c.byref(exited), c.byref(system), c.byref(user)):
        raise c.WinError(c.get_last_error())
    if not cycle_time(handle, c.byref(cycles)):
        raise c.WinError(c.get_last_error())

    class Memory(c.Structure):
        _fields_ = [("size", w.DWORD), ("faults", w.DWORD),
                    *[(name, c.c_size_t) for name in ("peak_working_set", "working_set", "peak_paged",
                       "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile", "private")]]

    memory = Memory()
    memory.size = c.sizeof(memory)
    query_memory = c.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    query_memory.argtypes = [w.HANDLE, c.POINTER(Memory), w.DWORD]
    query_memory.restype = w.BOOL
    if not query_memory(handle, c.byref(memory), memory.size):
        raise c.WinError(c.get_last_error())
    ticks = lambda value: (value.dwHighDateTime << 32) | value.dwLowDateTime
    return {"cpu_seconds": (ticks(system) + ticks(user)) * 1e-7, "cycles": cycles.value,
            "private_bytes": memory.private, "working_set_bytes": memory.working_set}
