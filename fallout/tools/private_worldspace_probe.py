"""Run the exact release DLL on a private desktop; never switch input desktops.

Run with pythonw.exe. The private game and its loader are owned by a kill-on-close
job. Temporary probe/cursor DLLs and the background-running INI edit are restored
after that job empties. The fixed LoadingScreens.dll stays installed.
"""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
GAME = Path(r'C:\Games\Fallout.4 1.10.163')
GUARD = Path(r'C:\Development\Realistic Reflections - Mirrors Fallout 4\build\private-desktop-cursor-guard\Release\000_FHCRCursorGuard.dll')
GUARD_HASH = 'fe53a85911209e930b9bf27867ddde377f16b1c861a754f5983941f9bb488871'
user = c.WinDLL('user32', use_last_error=True)
kernel = c.WinDLL('kernel32', use_last_error=True)

class Startup(c.Structure):
    _fields_ = [('cb', w.DWORD), ('reserved', w.LPWSTR), ('desktop', w.LPWSTR), ('title', w.LPWSTR),
                ('x', w.DWORD), ('y', w.DWORD), ('cx', w.DWORD), ('cy', w.DWORD),
                ('charsx', w.DWORD), ('charsy', w.DWORD), ('fill', w.DWORD), ('flags', w.DWORD),
                ('show', w.WORD), ('reserved2size', w.WORD), ('reserved2', c.c_void_p),
                ('stdin', w.HANDLE), ('stdout', w.HANDLE), ('stderr', w.HANDLE)]
class Process(c.Structure):
    _fields_ = [('process', w.HANDLE), ('thread', w.HANDLE), ('pid', w.DWORD), ('tid', w.DWORD)]
class BasicLimit(c.Structure):
    _fields_ = [('processTime', c.c_int64), ('jobTime', c.c_int64), ('flags', w.DWORD),
                ('minWS', c.c_size_t), ('maxWS', c.c_size_t), ('active', w.DWORD),
                ('affinity', c.c_size_t), ('priority', w.DWORD), ('scheduling', w.DWORD)]
class ExtendedLimit(c.Structure):
    _fields_ = [('basic', BasicLimit), ('io', c.c_uint64 * 6), ('processMemory', c.c_size_t),
                ('jobMemory', c.c_size_t), ('peakProcess', c.c_size_t), ('peakJob', c.c_size_t)]

user.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p]
user.CreateDesktopW.restype = w.HANDLE
user.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]
user.OpenInputDesktop.restype = w.HANDLE
user.GetUserObjectInformationW.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
user.CloseDesktop.argtypes = [w.HANDLE]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetThreadDesktop.argtypes = [w.DWORD]
user.GetThreadDesktop.restype = w.HANDLE
ENUM = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumDesktopWindows.argtypes = [w.HANDLE, ENUM, w.LPARAM]
kernel.CreateProcessW.argtypes = [w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p, w.BOOL,
                                  w.DWORD, c.c_void_p, w.LPCWSTR, c.POINTER(Startup), c.POINTER(Process)]
kernel.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
kernel.CreateJobObjectW.restype = w.HANDLE
kernel.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
kernel.QueryInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
kernel.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.ResumeThread.argtypes = [w.HANDLE]
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
kernel.K32EnumProcesses.argtypes = [c.POINTER(w.DWORD), w.DWORD, c.POINTER(w.DWORD)]

def check(result):
    if not result: raise c.WinError(c.get_last_error())
    return result

def desktop_name(handle):
    name = c.create_unicode_buffer(256)
    needed = w.DWORD()
    check(user.GetUserObjectInformationW(handle, 2, name, c.sizeof(name), c.byref(needed)))
    return name.value

def job_pids(job):
    buffer = c.create_string_buffer(65536)
    check(kernel.QueryInformationJobObject(job, 3, buffer, len(buffer), None))
    count = struct.unpack_from('<I', buffer, 4)[0]
    return list(struct.unpack_from('<' + 'Q' * count, buffer, 8))

def process_path(pid):
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle: return ''
    try:
        text = c.create_unicode_buffer(32768)
        size = w.DWORD(len(text))
        return text.value if kernel.QueryFullProcessImageNameW(handle, 0, text, c.byref(size)) else ''
    finally: kernel.CloseHandle(handle)

def windows(desktop, owned):
    found = []
    errors = []
    @ENUM
    def visit(hwnd, _):
        pid = w.DWORD()
        tid = user.GetWindowThreadProcessId(hwnd, c.byref(pid))
        if pid.value in owned:
            try:
                text = c.create_unicode_buffer(512)
                user.GetWindowTextW(hwnd, text, len(text))
                # The enumeration itself proves which desktop owns this HWND.
                # GetThreadDesktop from a different process can return a handle
                # without query rights; the in-process probe checks that too.
                found.append(dict(hwnd=int(hwnd), pid=pid.value, tid=tid, title=text.value,
                                  desktop=desktop_name(desktop), verifiedBy='EnumDesktopWindows'))
            except Exception as error:
                errors.append(str(error))
                return False
        return True
    c.set_last_error(0)
    result = user.EnumDesktopWindows(desktop, visit, 0)
    if errors: raise RuntimeError('Window desktop observation failed: ' + repr(errors))
    # A newly created, empty desktop can return FALSE with ERROR_SUCCESS.
    if not result and c.get_last_error(): raise c.WinError(c.get_last_error())
    return found

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def restore_with_retry(action):
    # A just-exited DLL can remain briefly locked while Windows closes its
    # image section. Do not let one locked helper skip the remaining restores.
    for attempt in range(40):
        try:
            action()
            return
        except PermissionError:
            if attempt == 39: raise
            time.sleep(.25)

def main():
    import pefile
    run = Path(sys.argv[1]).resolve()
    run.mkdir(parents=True, exist_ok=True)
    backup = run / 'backup'
    backup.mkdir(exist_ok=True)
    record = dict(hostPid=os.getpid(), game=str(GAME), started=time.time())
    (run / 'host.json').write_text(json.dumps(record, indent=2))
    owned_paths = []
    renamed_paths = []
    job = desktop = input_desktop = None
    process = Process()
    try:
        pids = (w.DWORD * 16384)()
        used = w.DWORD()
        check(kernel.K32EnumProcesses(pids, c.sizeof(pids), c.byref(used)))
        targets = {str(GAME / leaf).lower() for leaf in ('Fallout4.exe', 'f4se_loader.exe')}
        if any(process_path(pid).lower() in targets for pid in pids[:used.value // 4]):
            raise RuntimeError('The target game is already running; no files changed')
        check(digest(GUARD) == GUARD_HASH)
        plugins = GAME / 'Data/F4SE/Plugins'
        if '--omit-menu-framework' in sys.argv[2:]:
            target = plugins / 'F4SEMenuFramework.dll'
            parked = plugins / 'F4SEMenuFramework.dll.fls-issue2-private'
            if parked.exists(): raise RuntimeError('A previous isolated framework test needs restoration')
            if target.exists():
                expected = digest(target)
                shutil.copy2(target, backup / target.name)
                target.rename(parked)
                renamed_paths.append((target, parked, expected))
                record['temporarilyOmitted'] = {target.name: expected}
        if '--minimal-plugins' in sys.argv[2:]:
            keep = {'loadingscreens.dll', 'flsworldspaceprobe.dll', '000_fhcrcursorguard.dll', 'buffout4.dll'}
            for target in plugins.glob('*.dll'):
                if target.name.lower() in keep: continue
                parked = target.with_name(target.name + '.fls-issue2-private')
                if parked.exists(): raise RuntimeError('A prior private plugin test needs restoration')
                expected = digest(target)
                shutil.copy2(target, backup / target.name)
                target.rename(parked)
                renamed_paths.append((target, parked, expected))
                record.setdefault('temporarilyOmitted', {})[target.name] = expected
        release = ROOT / 'build/Release/LoadingScreens.dll'
        probe = ROOT / 'build/Release/FLSWorldspaceProbe.dll'
        if '--without-loadscreens' in sys.argv[2:]:
            target = plugins / release.name
            parked = target.with_name(target.name + '.fls-issue2-private')
            if parked.exists(): raise RuntimeError('A previous baseline test needs restoration')
            if target.exists():
                expected = digest(target)
                shutil.copy2(target, backup / target.name)
                target.rename(parked)
                renamed_paths.append((target, parked, expected))
                record.setdefault('temporarilyOmitted', {})[target.name] = expected
        # Read the actual release linker map, including the preferred image base.
        pe = pefile.PE(str(release), fast_load=True)
        symbol_map = (release.parent / 'LoadingScreens.map').read_text()
        match = re.search(r'\?Submit@WorldspacePreload@VRLoadingScreens@@\S+\s+([0-9a-fA-F]{16})\s+f', symbol_map)
        if not match: raise RuntimeError('Preparation entry missing from release map')
        prepare_rva = int(match[1], 16) - pe.OPTIONAL_HEADER.ImageBase
        (run / 'prepare-rva.txt').write_text(hex(prepare_rva))
        (run / 'prepare-entry.bin').write_bytes(pe.get_data(prepare_rva, 24))
        engine = pefile.PE(str(GAME / 'Fallout4.exe'), fast_load=True)
        (run / 'console-entry.bin').write_bytes(engine.get_data(0x125B4A0, 24))

        for source in ([probe, GUARD] if '--without-loadscreens' in sys.argv[2:] else [release, probe, GUARD]):
            target = plugins / source.name
            original = target.read_bytes() if target.exists() else None
            if original is not None: (backup / source.name).write_bytes(original)
            installed = source.read_bytes()
            target.write_bytes(installed)
            # The user-requested fix stays installed; test-only helpers are temporary.
            owned_paths.append((target, original, installed, source == release))
        custom = Path(os.environ['USERPROFILE']) / 'Documents/My Games/Fallout4/Fallout4Custom.ini'
        original = custom.read_bytes()
        (backup / custom.name).write_bytes(original)
        text = original.decode('utf-8-sig')
        text = re.sub(r'(?im)^bAlwaysActive\s*=.*(?:\r?\n)?', '', text)
        text = re.sub(r'(?im)^\[General\]\s*\r?$', '[General]\nbAlwaysActive=1', text, count=1)
        installed = text.encode('utf-8')
        custom.write_bytes(installed)
        owned_paths.append((custom, original, installed, False))

        name = 'FLSIssue2_' + str(os.getpid())
        os.environ['FLS_PROBE_DESKTOP'] = name
        os.environ['FLS_PROBE_DIRECTORY'] = str(run)
        os.environ['FHCR_CURSOR_GUARD_LOG'] = str(run / 'cursor-guard.log')
        desktop = check(user.CreateDesktopW(name, None, None, 0, 0x1FF, None))
        input_desktop = check(user.OpenInputDesktop(0, False, 0x41))
        input_name = desktop_name(input_desktop)
        if name == input_name: raise RuntimeError('Private desktop is the input desktop')
        job = check(kernel.CreateJobObjectW(None, None))
        limits = ExtendedLimit()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        check(kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)))
        startup = Startup()
        startup.cb = c.sizeof(startup)
        startup.desktop = 'WinSta0\\' + name
        startup.flags = 1
        startup.show = 0
        loader = GAME / 'f4se_loader.exe'
        command = c.create_unicode_buffer(subprocess.list2cmdline([str(loader)]))
        check(kernel.CreateProcessW(str(loader), command, None, None, False,
                                    0x08000204, None, str(GAME), c.byref(startup), c.byref(process)))
        if not kernel.AssignProcessToJobObject(job, process.process):
            kernel.TerminateProcess(process.process, 1)
            raise c.WinError(c.get_last_error())
        kernel.ResumeThread(process.thread)
        kernel.CloseHandle(process.thread)
        process.thread = None
        record.update(desktop=name, inputDesktop=input_name, loaderPid=process.pid,
                      dllSha256=digest(release), probeSha256=digest(probe), prepareRva=prepare_rva)
        (run / 'launch.json').write_text(json.dumps(record, indent=2))
        end = time.monotonic() + 1800
        observed_game = False
        while time.monotonic() < end and not (run / 'stop').exists():
            pids = job_pids(job)
            if not pids: break
            leaked = windows(input_desktop, pids)
            if leaked: raise RuntimeError('Owned window appeared on input desktop: ' + repr(leaked))
            private_windows = windows(desktop, pids)
            for window in private_windows:
                if window['desktop'] != name: raise RuntimeError('Game window desktop mismatch')
            children = [{'pid': pid, 'path': process_path(pid)} for pid in pids]
            for child in children:
                if child['path'].lower() == str(GAME / 'Fallout4.exe').lower():
                    observed_game = True
                    record['gamePid'] = child['pid']
            (run / 'status.json').write_text(json.dumps(dict(record, children=children,
                windows=private_windows, mainDesktopWindows=[], observedAt=time.time()), indent=2))
            time.sleep(1)
        if job_pids(job): kernel.TerminateJobObject(job, 0)
        deadline = time.monotonic() + 30
        while job_pids(job) and time.monotonic() < deadline: time.sleep(.2)
        if job_pids(job): raise RuntimeError('Owned game job failed to drain')
        record['observedGame'] = observed_game
    except Exception:
        record['error'] = traceback.format_exc()
        (run / 'error.txt').write_text(record['error'])
    finally:
        if job:
            kernel.TerminateJobObject(job, 0)
            deadline = time.monotonic() + 30
            while job_pids(job) and time.monotonic() < deadline: time.sleep(.2)
        for target, original, installed, keep in reversed(owned_paths):
            if keep: continue
            def restore_owned():
                if not target.exists() or target.read_bytes() != installed:
                    raise RuntimeError('Owned bytes changed: ' + str(target))
                if original is None: target.unlink()
                else: target.write_bytes(original)
            try:
                restore_with_retry(restore_owned)
            except Exception as error:
                record.setdefault('restoreConflicts', []).append(str(error))
        for target, parked, expected in reversed(renamed_paths):
            def restore_parked():
                if target.exists() or not parked.exists() or digest(parked) != expected:
                    raise RuntimeError('Parked bytes changed: ' + str(target))
                parked.rename(target)
            try:
                restore_with_retry(restore_parked)
            except Exception as error:
                record.setdefault('restoreConflicts', []).append(str(error))
        if process.process: kernel.CloseHandle(process.process)
        if job: kernel.CloseHandle(job)
        if input_desktop: user.CloseDesktop(input_desktop)
        if desktop: user.CloseDesktop(desktop)
        record['finished'] = time.time()
        (run / 'finished.json').write_text(json.dumps(record, indent=2))

if __name__ == '__main__': main()
