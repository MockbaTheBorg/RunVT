// abstraction_windows.h - RunVT OS abstraction layer, Windows side.
//
// Same API as abstraction_posix.h, built on ConPTY instead of a real
// pty (needs Windows 10 1809+, that's when CreatePseudoConsole showed
// up). I haven't actually built or run this yet - Linux is the target
// for now - but it's written to the same shape as the POSIX version so
// a Windows build later should only mean touching this one file.

#ifndef RUNVT_ABSTRACTION_WINDOWS_H
#define RUNVT_ABSTRACTION_WINDOWS_H

// ConPTY's declarations in the Windows SDK (and mingw-w64's headers)
// are gated behind this version check - without it, windows.h just
// silently omits CreatePseudoConsole and friends instead of erroring,
// which is a confusing way to find out.
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006 // NTDDI_WIN10_RS5 (1809), where ConPTY landed
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif

#include <windows.h>
#include <string.h>
#include <stdio.h>

// Trying to work around older headers rather than
// making everyone upgrade their mingw-w64 package.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
typedef HANDLE HPCON;
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
WINBASEAPI HRESULT WINAPI CreatePseudoConsole(COORD size, HANDLE hInput,
    HANDLE hOutput, DWORD dwFlags, HPCON *phPC);
WINBASEAPI void WINAPI ClosePseudoConsole(HPCON hPC);
#endif

typedef struct RT_Process {
    HPCON hpc;
    HANDLE in_write;   // we write here, ConPTY reads it as child stdin
    HANDLE out_read;   // we read here, ConPTY writes child stdout/stderr
    PROCESS_INFORMATION pi;
    int exited;
    int exit_code;
} RT_Process;

static void rt_attach_console(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

static int rt_spawn(RT_Process *proc, char *const argv[], int cols, int rows) {
    HANDLE in_read = NULL, in_write = NULL;
    HANDLE out_read = NULL, out_write = NULL;
    COORD size;
    STARTUPINFOEXA si;
    SIZE_T attr_size = 0;
    char cmdline[4096];
    int i;

    memset(proc, 0, sizeof(*proc));

    if (!CreatePipe(&in_read, &in_write, NULL, 0)) return -1;
    if (!CreatePipe(&out_read, &out_write, NULL, 0)) return -1;

    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    if (CreatePseudoConsole(size, in_read, out_write, 0, &proc->hpc) != S_OK) {
        return -1;
    }

    CloseHandle(in_read);
    CloseHandle(out_write);
    proc->in_write = in_write;
    proc->out_read = out_read;

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
        GetProcessHeap(), 0, attr_size);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size);
    UpdateProcThreadAttribute(si.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, proc->hpc, sizeof(HPCON),
        NULL, NULL);

    // CreateProcess wants one command line string, not an argv array -
    // this is the naive join with no quoting, which is fine for the
    // simple "app plus a few args" case RunVT targets but will bite
    // anyone passing an argument with a space in it. Good enough for
    // v1, worth revisiting if that turns out to matter.
    cmdline[0] = '\0';
    for (i = 0; argv[i] != NULL; i++) {
        if (i > 0) strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
        strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 1);
    }

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
            &si.StartupInfo, &proc->pi)) {
        return -1;
    }

    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);

    return 0;
}

// Windows pipes don't have anything like select()/poll(), so this is
// just a Sleep-and-recheck loop. Coarser than the POSIX version but
// the 10ms step keeps it well under one frame.
static int rt_poll_readable(RT_Process *proc, int timeout_ms) {
    DWORD avail = 0;
    DWORD waited = 0;
    const DWORD step = 10;

    for (;;) {
        if (!PeekNamedPipe(proc->out_read, NULL, 0, NULL, &avail, NULL)) {
            return -1;
        }
        if (avail > 0) return 1;
        if (timeout_ms >= 0 && (int)waited >= timeout_ms) return 0;
        Sleep(step);
        waited += step;
        if (timeout_ms < 0) continue;
    }
}

static int rt_read(RT_Process *proc, unsigned char *buf, int maxlen) {
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(proc->out_read, NULL, 0, NULL, &avail, NULL)) {
        return -1;
    }
    if (avail == 0) return 0;
    if (!ReadFile(proc->out_read, buf, (DWORD)maxlen, &got, NULL)) {
        return -1;
    }
    return (int)got;
}

static int rt_write(RT_Process *proc, const unsigned char *buf, int len) {
    DWORD written = 0;
    if (!WriteFile(proc->in_write, buf, (DWORD)len, &written, NULL)) {
        return -1;
    }
    return (int)written;
}

static int rt_child_alive(RT_Process *proc) {
    DWORD code;
    if (proc->exited) return 0;
    if (!GetExitCodeProcess(proc->pi.hProcess, &code)) return 0;
    if (code != STILL_ACTIVE) {
        proc->exited = 1;
        proc->exit_code = (int)code;
        return 0;
    }
    return 1;
}

static void rt_cleanup(RT_Process *proc) {
    if (proc->hpc) ClosePseudoConsole(proc->hpc);
    if (proc->in_write) CloseHandle(proc->in_write);
    if (proc->out_read) CloseHandle(proc->out_read);
    if (proc->pi.hProcess) CloseHandle(proc->pi.hProcess);
    if (proc->pi.hThread) CloseHandle(proc->pi.hThread);
}

#endif // RUNVT_ABSTRACTION_WINDOWS_H
