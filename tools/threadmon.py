import os
import sys

if sys.platform != "win32":
    class ThreadMon:
        def __init__(self, pid):
            self.pid = pid
            self.prev = {}
            self.hz = os.sysconf("SC_CLK_TCK")

        def sample(self):
            out = {}
            base = "/proc/%d/task" % self.pid
            for tid in os.listdir(base):
                try:
                    with open(os.path.join(base, tid, "stat")) as f:
                        st = f.read()
                    with open(os.path.join(base, tid, "comm")) as f:
                        name = f.read().strip()
                except OSError:
                    continue
                fields = st[st.rfind(")") + 2:].split()
                t = (int(fields[11]) + int(fields[12])) / self.hz
                if tid in self.prev:
                    out[name] = out.get(name, 0) + t - self.prev[tid]
                self.prev[tid] = t
            return out
else:
    import ctypes
    import ctypes.wintypes as wt

    TH32CS_SNAPTHREAD = 0x4
    THREAD_QUERY_LIMITED_INFORMATION = 0x0800

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)


    class THREADENTRY32(ctypes.Structure):
        _fields_ = [
            ("dwSize", wt.DWORD),
            ("cntUsage", wt.DWORD),
            ("th32ThreadID", wt.DWORD),
            ("th32OwnerProcessID", wt.DWORD),
            ("tpBasePri", wt.LONG),
            ("tpDeltaPri", wt.LONG),
            ("dwFlags", wt.DWORD),
        ]


    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    k32.OpenThread.restype = wt.HANDLE
    k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
    k32.GetThreadDescription.restype = ctypes.c_long


    def thread_ids(pid):
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
        e = THREADENTRY32()
        e.dwSize = ctypes.sizeof(e)
        ids = []
        ok = k32.Thread32First(snap, ctypes.byref(e))
        while ok:
            if e.th32OwnerProcessID == pid:
                ids.append(e.th32ThreadID)
            ok = k32.Thread32Next(snap, ctypes.byref(e))
        k32.CloseHandle(snap)
        return ids


    class ThreadMon:
        def __init__(self, pid):
            self.pid = pid
            self.handles = {}
            self.names = {}
            self.prev = {}

        def refresh(self):
            for tid in thread_ids(self.pid):
                if tid in self.handles:
                    continue
                h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                if not h:
                    continue
                self.handles[tid] = h
                self.names[tid] = None

        def name(self, tid):
            if not self.names.get(tid):
                p = ctypes.c_wchar_p()
                if k32.GetThreadDescription(self.handles[tid], ctypes.byref(p)) >= 0 and p.value:
                    self.names[tid] = p.value
                    k32.LocalFree(p)
            return self.names.get(tid) or "tid%d" % tid

        def sample(self):
            self.refresh()
            out = {}
            c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
            for tid, h in list(self.handles.items()):
                if not k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
                    continue
                t = ((k.dwHighDateTime << 32) | k.dwLowDateTime) + ((u.dwHighDateTime << 32) | u.dwLowDateTime)
                if tid in self.prev:
                    key = self.name(tid)
                    out[key] = out.get(key, 0) + (t - self.prev[tid]) / 1e7
                self.prev[tid] = t
            return out
