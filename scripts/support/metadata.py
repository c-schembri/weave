"""Collect benchmark provenance without invoking a scripting shell."""

import ctypes
from datetime import datetime
import os
import platform
import re
import struct
import sys

from support.common import ROOT, capture, require


def windows_cpus():
    import winreg
    from ctypes import wintypes

    topology = ctypes.WinDLL("kernel32", use_last_error=True).GetLogicalProcessorInformationEx
    topology.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
    topology.restype = wintypes.BOOL
    size = wintypes.DWORD()
    topology(0xFFFF, None, ctypes.byref(size))
    if ctypes.get_last_error() != 122:  # ERROR_INSUFFICIENT_BUFFER
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size.value)
    if not topology(0xFFFF, buffer, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())

    cores, packages = [], []
    offset = 0
    affinity_format = "<QH6x" if ctypes.sizeof(ctypes.c_void_p) == 8 else "<IH6x"
    while offset < size.value:
        relationship, length = struct.unpack_from("<II", buffer, offset)
        require(length >= 8 and offset + length <= size.value, "Invalid Windows CPU topology.")
        if relationship in (0, 3):  # RelationProcessorCore / RelationProcessorPackage
            count = struct.unpack_from("<H", buffer, offset + 30)[0]
            require(32 + count * struct.calcsize(affinity_format) <= length, "Invalid CPU group count.")
            masks = {}
            for index in range(count):
                mask, group = struct.unpack_from(affinity_format, buffer, offset + 32 + index * struct.calcsize(affinity_format))
                masks[group] = mask
            (cores if relationship == 0 else packages).append(masks)
        offset += length

    result = []
    processor_index = 0
    for package in packages:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"HARDWARE\DESCRIPTION\System\CentralProcessor\{processor_index}") as key:
            name = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        logical = sum(mask.bit_count() for mask in package.values())
        physical = sum(any(mask & package.get(group, 0) for group, mask in core.items()) for core in cores)
        result.append({"Name": name, "NumberOfCores": physical, "NumberOfLogicalProcessors": logical})
        processor_index += logical
    require(bool(result), "Windows returned no CPU packages.")
    return result


def environment():
    require(os.name == "nt", "The networking benchmarks currently require Windows.")
    version = sys.getwindowsversion()
    return {
        "timestamp": datetime.now().astimezone().isoformat(),
        "revision": capture(["git", "rev-parse", "HEAD"]),
        "worktree": capture(["git", "status", "--porcelain"]).splitlines(),
        "cpu": windows_cpus(),
        "os": {"Caption": f"Microsoft Windows {platform.win32_ver()[0]}",
               "Version": f"{version.major}.{version.minor}.{version.build}", "BuildNumber": str(version.build)},
        "power_scheme": capture(["powercfg", "/GETACTIVESCHEME"]),
        "python": platform.python_version(),
    }


def compiler_versions(build):
    return [line for path in sorted(build.rglob("CMakeCXXCompiler.cmake"))
            for line in path.read_text(encoding="utf-8").splitlines() if "CMAKE_CXX_COMPILER_VERSION " in line]


def timing_build(build):
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    for flag in ("WEAVE_ENABLE_ASAN", "WEAVE_PROFILE_RUNTIME", "WEAVE_TRACE_RUNTIME"):
        require(not re.search(rf"^{flag}:BOOL=ON$", cache, re.MULTILINE), f"Timing requires {flag}=OFF.")


def source_files():
    directories = ("modules", "test_support", "cmake", "benchmarks/support", "benchmarks/integration", "scripts")
    files = {ROOT / name for name in ("CMakeLists.txt", "benchmarks/CMakeLists.txt", "docs/gate.md")}
    for directory in directories:
        files.update(path for path in (ROOT / directory).rglob("*")
                     if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc")
    return sorted(files)
