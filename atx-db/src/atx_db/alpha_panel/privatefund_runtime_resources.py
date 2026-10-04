"""Verify the actual external Windows job and bounded native connection."""
from __future__ import annotations

import ctypes as ct
from ctypes import wintypes as wt
import os
from pathlib import Path
import shutil
import uuid

from . import common as C

GIB = 1024 ** 3


class _Basic(ct.Structure):
    _fields_ = [('process_time', ct.c_longlong), ('job_time', ct.c_longlong),
                ('flags', wt.DWORD), ('min_ws', ct.c_size_t), ('max_ws', ct.c_size_t),
                ('active_limit', wt.DWORD), ('affinity', ct.c_size_t),
                ('priority', wt.DWORD), ('scheduling', wt.DWORD)]


class _IO(ct.Structure):
    _fields_ = [(name, ct.c_ulonglong) for name in
               ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]


class _Limits(ct.Structure):
    _fields_ = [('basic', _Basic), ('io', _IO), ('process_memory', ct.c_size_t),
                ('job_memory', ct.c_size_t), ('peak_process', ct.c_size_t), ('peak_job', ct.c_size_t)]


def guard_identity():
    """An environment string alone does not prove membership or an enforced cap."""
    name = os.environ.get('ATX_GUARD_JOB')
    if os.name != 'nt' or not name:
        raise RuntimeError('external Windows memory guard required')
    kernel = ct.WinDLL('kernel32', use_last_error=True)
    kernel.OpenJobObjectW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    kernel.OpenJobObjectW.restype = wt.HANDLE
    kernel.GetCurrentProcess.restype = wt.HANDLE
    kernel.IsProcessInJob.argtypes = [wt.HANDLE, wt.HANDLE, ct.POINTER(wt.BOOL)]
    kernel.IsProcessInJob.restype = wt.BOOL
    kernel.QueryInformationJobObject.argtypes = [wt.HANDLE, ct.c_int, ct.c_void_p, wt.DWORD, ct.c_void_p]
    kernel.QueryInformationJobObject.restype = wt.BOOL
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    handle = kernel.OpenJobObjectW(0x0004, False, name)
    if not handle:
        raise RuntimeError('external guard job unavailable')
    try:
        member = wt.BOOL()
        limits = _Limits()
        if not kernel.IsProcessInJob(kernel.GetCurrentProcess(), handle, ct.byref(member)) or not member.value:
            raise RuntimeError('process is outside declared guard job')
        if not kernel.QueryInformationJobObject(handle, 9, ct.byref(limits), ct.sizeof(limits), None):
            raise RuntimeError('guard cap cannot be verified')
        if not limits.basic.flags & 0x2000 or not 0 < limits.job_memory <= GIB:
            raise RuntimeError('guard must enforce a job memory cap <=1GiB')
        return {'job': name, 'cap_bytes': limits.job_memory}
    finally:
        kernel.CloseHandle(handle)


def check_disk(root):
    free = shutil.disk_usage(Path(root).resolve(strict=True)).free
    if free < 40 * GIB:
        raise RuntimeError('financial runtime requires 40GiB free disk')
    return free


def connect(stage):
    """Use unchanged common.connect with explicit file-backed runtime arguments."""
    return C.connect(memory='256MB', threads=1, temp_dir=Path(stage)/'_spill',
                     db_file='fund-runtime-'+uuid.uuid4().hex+'.duckdb')
