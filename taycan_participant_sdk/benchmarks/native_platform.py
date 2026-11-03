"""Host metrics for the explicitly non-acceptance macOS development mode."""
import ctypes
import errno
import os
from pathlib import Path
import platform
import subprocess


class ProcTaskInfo(ctypes.Structure):
    # macOS SDK sys/proc_info.h: six uint64_t fields, then twelve int32_t fields.
    _fields_ = [(name, ctypes.c_uint64) for name in (
        'virtual_size', 'resident_size', 'total_user', 'total_system',
        'threads_user', 'threads_system')] + [(name, ctypes.c_int32) for name in (
        'policy', 'faults', 'pageins', 'cow_faults', 'messages_sent', 'messages_received',
        'syscalls_mach', 'syscalls_unix', 'context_switches', 'thread_count', 'running', 'priority')]


_libproc = None


def read_macos_rss(pid):
    global _libproc
    if platform.system() != 'Darwin':
        raise OSError('macOS RSS reader requires Darwin')
    if _libproc is None:
        _libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
        _libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                        ctypes.c_void_p, ctypes.c_int]
        _libproc.proc_pidinfo.restype = ctypes.c_int
    info = ProcTaskInfo()
    ctypes.set_errno(0)
    size = _libproc.proc_pidinfo(pid, 4, 0, ctypes.byref(info), ctypes.sizeof(info))
    if size == ctypes.sizeof(info):
        return info.resident_size or None
    error = ctypes.get_errno()
    if error in (errno.ESRCH, errno.ENOENT):
        return None
    if size == 0 and not error:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return None
    raise OSError(error or errno.EIO, 'proc_pidinfo(PROC_PIDTASKINFO) returned {} bytes'.format(size))


def native_processes(targets, exclude=()):
    result = subprocess.run(['/bin/ps', '-axo', 'pid=,comm=,args='],
                            capture_output=True, text=True, check=True, timeout=10)
    found = []
    excluded = {os.getpid()} | set(exclude)
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) < 3 or not fields[0].isdigit() or int(fields[0]) in excluded:
            continue
        executable, command = fields[1:]
        if Path(executable).name in ('zsh', 'bash', 'sh', 'ps'):
            continue
        tokens = command.split()
        if Path(executable).name in targets or any(Path(token).name in targets for token in tokens):
            found.append(dict(pid=int(fields[0]), command=command))
    return found
