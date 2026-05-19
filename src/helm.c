#include "helm.h"

IVDM *Vdm = NULL;
IVDMI *VdmInternal = NULL;

/* ============================================================
 * Top-level command dispatcher
 * ============================================================ */

static int ProcessCommand(const wchar_t *cmd, BOOL global, BOOL admin) {
    if (wcsncmp(cmd, L"vd:", 3) == 0)
        return ProcessVdCommand(cmd + 3);
    if (wcsncmp(cmd, L"sz:", 3) == 0)
        return ProcessSzCommand(cmd + 3);
    if (wcscmp(cmd, L"max") == 0)
        return ProcessMaxCommand();
    if (wcscmp(cmd, L"min") == 0)
        return ProcessMinCommand();
    if (wcsncmp(cmd, L"swap", 4) == 0 && (cmd[4] == L'\0' || cmd[4] == L':'))
        return ProcessSwapCommand(cmd + 4);
    if (wcsncmp(cmd, L"uri:", 4) == 0)
        return ProcessUriCommand(cmd + 4);
    const wchar_t *app = (wcsncmp(cmd, L"app:", 4) == 0) ? cmd + 4 : cmd;
    return ProcessAppCommand(app, global, admin);
}

/* ============================================================
 * Pipe / daemon infrastructure
 * ============================================================ */

#define READY_EVENT_NAME L"helm-daemon-ready"
#define DAEMON_MUTEX_NAME L"helm-daemon-singleton"
#define PIPE_NAME L"\\\\.\\pipe\\helm"

static HANDLE DaemonMutex = NULL;

static BOOL AcquireDaemonMutex(void) {
    DaemonMutex = CreateMutexW(NULL, TRUE, DAEMON_MUTEX_NAME);
    if (!DaemonMutex)
        return FALSE;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(DaemonMutex);
        DaemonMutex = NULL;
        return FALSE;
    }
    return TRUE;
}

static HANDLE CreatePipeInstance(void) {
    return CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_INBOUND,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                PIPE_WAIT,
                            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
}

static void RunServer(void) {
    if (!AcquireDaemonMutex())
        return;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, READY_EVENT_NAME);

    CoCreateInstance(&CLSID_VDM, NULL, CLSCTX_ALL, &IID_IVDM, (void **)&Vdm);
    InitVdInternal();

    HANDLE pipe = CreatePipeInstance();
    if (pipe != INVALID_HANDLE_VALUE)
        SetEvent(ready);

    while (pipe != INVALID_HANDLE_VALUE) {
        if (!ConnectNamedPipe(pipe, NULL) &&
            GetLastError() != ERROR_PIPE_CONNECTED) {
            HANDLE next = CreatePipeInstance();
            CloseHandle(pipe);
            pipe = next;
            continue;
        }

        DWORD read;
        wchar_t buf[1024];
        BOOL got =
            ReadFile(pipe, buf, sizeof(buf) - 2, &read, NULL) && read > 0;
        HANDLE next = CreatePipeInstance();
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        pipe = next;
        if (!got)
            continue;
        read &= ~1u;
        buf[read / sizeof(wchar_t)] = 0;
        wchar_t cmd[1024] = {0};
        BOOL global = FALSE;
        BOOL admin = FALSE;
        wchar_t *p = buf;
        while (*p) {
            while (*p == L' ' || *p == L'\t')
                p++;
            if (!*p)
                break;
            wchar_t *start = p;
            while (*p && *p != L' ' && *p != L'\t')
                p++;
            wchar_t saved = *p;
            *p = 0;
            if (lstrcmpiW(start, L"--global") == 0 ||
                lstrcmpiW(start, L"--all") == 0)
                global = TRUE;
            else if (lstrcmpiW(start, L"--admin") == 0)
                admin = TRUE;
            else if (!cmd[0])
                StringCchCopyW(cmd, 1024, start);
            if (!saved)
                break;
            *p = saved;
            p++;
        }
        if (cmd[0])
            ProcessCommand(cmd, global, admin);
    }

    if (Vdm)
        IVDM_Release(Vdm);
    if (VdmInternal)
        VdmInternal->lpVtbl->Release(VdmInternal);
    CloseHandle(ready);
}

static BOOL TrySendToPipe(const wchar_t *cmd) {
    if (!WaitNamedPipeW(PIPE_NAME, 50))
        return FALSE;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 3 && pipe == INVALID_HANDLE_VALUE; i++) {
        pipe = CreateFileW(PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0,
                           NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY)
                return FALSE;
            WaitNamedPipeW(PIPE_NAME, 100);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written;
    BOOL ok = WriteFile(pipe, cmd, (DWORD)(wcslen(cmd) * sizeof(wchar_t)),
                        &written, NULL);
    CloseHandle(pipe);
    return ok;
}

static void SpawnDaemon(void) {
    wchar_t self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    StringCchPrintfW(cmd, MAX_PATH + 16, L"\"%s\" --server", self);
    STARTUPINFOW si = {.cb = sizeof(si),
                       .dwFlags = STARTF_USESHOWWINDOW,
                       .wShowWindow = SW_HIDE};
    PROCESS_INFORMATION pi = {0};
    CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                   CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (pi.hThread)
        CloseHandle(pi.hThread);
}

static BOOL SendWithSpawn(const wchar_t *cmd) {
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, READY_EVENT_NAME);
    if (ev) {
        WaitForSingleObject(ev, 500);
        CloseHandle(ev);
        for (int i = 0; i < 2; i++) {
            if (TrySendToPipe(cmd))
                return TRUE;
            Sleep(80);
        }
    }
    SpawnDaemon();
    ev = NULL;
    for (int i = 0; i < 10; i++) {
        ev = OpenEventW(SYNCHRONIZE, FALSE, READY_EVENT_NAME);
        if (ev)
            break;
        Sleep(50);
    }
    if (ev) {
        WaitForSingleObject(ev, 1000);
        CloseHandle(ev);
    } else {
        Sleep(300);
    }
    return TrySendToPipe(cmd);
}

/* ============================================================
 * Entry point
 * ============================================================ */

int wmain(int argc, wchar_t *argv[]) {
    BOOL server = FALSE;
    wchar_t cmdBuf[1024] = {0};
    for (int i = 1; i < argc; i++) {
        if (lstrcmpiW(argv[i], L"--server") == 0) {
            server = TRUE;
            continue;
        }
        if (cmdBuf[0])
            StringCchCatW(cmdBuf, 1024, L" ");
        StringCchCatW(cmdBuf, 1024, argv[i]);
    }
    if (server) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        RunServer();
        CoUninitialize();
        return 0;
    }
    if (!cmdBuf[0]) {
        fwprintf(
            stderr,
            L"Usage: %s <command> [--global] [--admin]\n"
            L"  app:<exe>                       focus or launch app\n"
            L"  uri:<uri>                       open URI in default handler\n"
            L"  vd:<n>                          switch to virtual desktop n\n"
            L"  vd:send:<n>                     move foreground window to "
            L"desktop n\n"
            L"  sz:left|right|down|up:+/-N      resize snapped window pair\n"
            L"  max                             maximize foreground window\n"
            L"  min                             minimize foreground window\n"
            L"  swap[:left|right|up|down]       swap snapped neighbour; snap "
            L"fg if none\n",
            argv[0]);
        return 1;
    }
    return SendWithSpawn(cmdBuf) ? 0 : 1;
}
