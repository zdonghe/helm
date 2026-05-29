#include "helm.h"

BOOL IsElevated(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    BOOL elevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te;
        DWORD sz = sizeof(te);
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &sz))
            elevated = te.TokenIsElevated;
        CloseHandle(token);
    }
    cached = elevated;
    return elevated;
}

static HANDLE ShellExecAsUser(const wchar_t *file) {
    /* Caller must have called MaybeRebuildPidCache(); */
    DWORD explorerPid = GetExplorerPid();
    if (!explorerPid) {
        Log(LOG_PERF, L"ShellExecAsUser: explorer.exe not in pid cache");
        return INVALID_HANDLE_VALUE;
    }

    /* Open explorer and duplicate its (medium integrity) token */
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, explorerPid);
    if (!hProc) {
        Log(LOG_PERF, L"ShellExecAsUser: OpenProcess failed err=%lu",
            GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    HANDLE hToken = NULL;
    BOOL ok = OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hToken);
    CloseHandle(hProc);
    if (!ok) {
        Log(LOG_PERF, L"ShellExecAsUser: OpenProcessToken failed err=%lu",
            GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    HANDLE hDup = NULL;
    ok = DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                          TokenPrimary, &hDup);
    CloseHandle(hToken);
    if (!ok) {
        Log(LOG_PERF, L"ShellExecAsUser: DuplicateTokenEx failed err=%lu",
            GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    /* Launch file with explorer's token — inherits medium integrity */
    wchar_t cmd[MAX_PATH + 4];
    StringCchPrintfW(cmd, MAX_PATH + 4, L"\"%s\"", file);
    STARTUPINFOW si = {.cb = sizeof(si),
                       .dwFlags = STARTF_USESHOWWINDOW,
                       .wShowWindow = SW_SHOWNORMAL};
    PROCESS_INFORMATION pi = {0};
    ok = CreateProcessWithTokenW(hDup, 0, NULL, cmd, 0, NULL, NULL, &si, &pi);
    CloseHandle(hDup);
    if (!ok) {
        Log(LOG_PERF,
            L"ShellExecAsUser: CreateProcessWithTokenW failed err=%lu",
            GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void ResolveTarget(const wchar_t *in, wchar_t *matchExe,
                          wchar_t *launchExe, size_t sz) {
    const wchar_t *lastBs = wcsrchr(in, L'\\');
    const wchar_t *lastFs = wcsrchr(in, L'/');
    const wchar_t *sep;
    if (lastBs && lastFs)
        sep = lastBs > lastFs ? lastBs : lastFs;
    else
        sep = lastBs ? lastBs : lastFs;
    const wchar_t *base = sep ? sep + 1 : in;

    wchar_t stem[MAX_PATH];
    StringCchCopyW(stem, MAX_PATH, base);
    size_t slen = wcslen(stem);
    if (slen >= 4 && _wcsicmp(stem + slen - 4, L".exe") == 0)
        stem[slen - 4] = 0;

    if (lstrcmpiW(stem, L"wt") == 0 ||
        lstrcmpiW(stem, L"windowsterminal") == 0) {
        StringCchCopyW(matchExe, sz, L"WindowsTerminal.exe");
        StringCchCopyW(launchExe, sz, L"wt.exe");
        return;
    }

    StringCchCopyW(matchExe, sz, stem);
    StringCchCatW(matchExe, sz, L".exe");
    StringCchCopyW(launchExe, sz, matchExe);
}

static const wchar_t *AutoClass(const wchar_t *exe) {
    if (lstrcmpiW(exe, L"msedge.exe") == 0 ||
        lstrcmpiW(exe, L"chrome.exe") == 0)
        return L"Chrome_WidgetWin_1";
    if (lstrcmpiW(exe, L"explorer.exe") == 0)
        return L"CabinetWClass";
    return NULL;
}

typedef struct {
    const wchar_t *exe;
    const wchar_t *cls;
    HWND found;
    BOOL global;
    BOOL polling;
} FindCtx;

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    FindCtx *ctx = (FindCtx *)lp;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        return TRUE;
    if (ctx->cls) {
        wchar_t cls[256];
        GetClassNameW(hwnd, cls, 256);
        if (lstrcmpiW(cls, ctx->cls) != 0)
            return TRUE;
    }

    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    const wchar_t *exe = GetExeFromPid(pid);
    if (!exe || lstrcmpiW(exe, ctx->exe) != 0)
        return TRUE;

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return TRUE;

    if (!ctx->polling && !ctx->global && Vdm) {
        BOOL onCurrent = FALSE;
        if (SUCCEEDED(IVDM_IsCurrent(Vdm, hwnd, &onCurrent)) && !onCurrent)
            return TRUE;
    }

    ctx->found = hwnd;
    return FALSE;
}

void BypassForegroundLock(void) {
    INPUT dummy = {.type = INPUT_MOUSE};
    SendInput(1, &dummy, sizeof(INPUT));
}

typedef struct {
    wchar_t exe[EXE_NAME_MAX];
    const wchar_t *cls;
    HANDLE hProcess;
} LaunchPollCtx;

static void FocusHwnd(HWND h) {
    if (IsIconic(h))
        ShowWindow(h, SW_RESTORE);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                     SWP_ASYNCWINDOWPOS);
    BypassForegroundLock();
    SetForegroundWindow(h);
}
#define LAUNCH_POLL_TIMEOUT_MS 5000
#define LAUNCH_BACKSTOP_MS 250

typedef struct {
    const wchar_t *exe;
    const wchar_t *cls;
    HWND found;
    int events;
} PollHookCtx;

static __thread PollHookCtx *PollCtx;

static BOOL ExeBasenameOfPid(DWORD pid, wchar_t *out, DWORD cch) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return FALSE;
    wchar_t full[MAX_PATH];
    DWORD n = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, full, &n);
    CloseHandle(h);
    if (!ok)
        return FALSE;
    const wchar_t *bs = wcsrchr(full, L'\\');
    StringCchCopyW(out, cch, bs ? bs + 1 : full);
    return TRUE;
}

static void CALLBACK PollWinEventProc(HWINEVENTHOOK hook, DWORD event,
                                      HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD tid, DWORD t) {
    (void)hook;
    (void)event;
    (void)tid;
    (void)t;
    PollHookCtx *ctx = PollCtx;
    if (!ctx)
        return;
    ctx->events++;
    if (ctx->found || !hwnd)
        return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) /* top-level only */
        return;
    if (!IsWindowVisible(hwnd))
        return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        return;
    if (ctx->cls) {
        wchar_t cls[256];
        GetClassNameW(hwnd, cls, 256);
        if (lstrcmpiW(cls, ctx->cls) != 0)
            return;
    }
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t exe[EXE_NAME_MAX];
    if (!ExeBasenameOfPid(pid, exe, EXE_NAME_MAX) ||
        lstrcmpiW(exe, ctx->exe) != 0)
        return;
    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return;
    ctx->found = hwnd;
}

static DWORD WINAPI LaunchPollThread(LPVOID param) {
    LaunchPollCtx *lp = (LaunchPollCtx *)param;
    long long t0 = StartMeasuring();
    if (lp->hProcess) {
        CloseHandle(lp->hProcess);
        lp->hProcess = NULL;
    }

    PollHookCtx hookCtx = {.exe = lp->exe, .cls = lp->cls, .found = NULL};
    PollCtx = &hookCtx;
    DWORD hf = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                         NULL, PollWinEventProc, 0, 0, hf);
    HWINEVENTHOOK hookUC =
        SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, NULL,
                        PollWinEventProc, 0, 0, hf);

    int scans = 0;
    ForceRebuildPidCache();
    FindCtx fc = {.exe = lp->exe, .cls = lp->cls, .polling = TRUE};
    scans++;
    EnumWindows(EnumProc, (LPARAM)&fc);
    HWND found = fc.found;

    DWORD start = GetTickCount();
    DWORD lastScan = start;
    while (!found && !hookCtx.found) {
        if (GetTickCount() - start >= LAUNCH_POLL_TIMEOUT_MS)
            break;
        DWORD r = MsgWaitForMultipleObjects(0, NULL, FALSE, LAUNCH_BACKSTOP_MS,
                                            QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        DWORD now = GetTickCount();
        if (!hookCtx.found && now - lastScan >= LAUNCH_BACKSTOP_MS) {
            ForceRebuildPidCache();
            FindCtx bc = {.exe = lp->exe, .cls = lp->cls, .polling = TRUE};
            scans++;
            EnumWindows(EnumProc, (LPARAM)&bc);
            found = bc.found;
            lastScan = now;
        }
    }

    if (hook)
        UnhookWinEvent(hook);
    if (hookUC)
        UnhookWinEvent(hookUC);
    PollCtx = NULL;

    HWND target = hookCtx.found ? hookCtx.found : found;
    if (target) {
        Log(LOG_PERF,
            L"launch %ls: detected %.0f ms (hook events %d, scans %d, via %ls)",
            lp->exe, FinishMeasuring(t0), hookCtx.events, scans,
            hookCtx.found ? L"hook" : L"scan");
        FocusHwnd(target);
    } else {
        Log(LOG_PERF,
            L"launch %ls: not found %.0f ms (hook events %d, scans %d)",
            lp->exe, FinishMeasuring(t0), hookCtx.events, scans);
    }
    free(lp);
    return 0;
}

static HANDLE ShellLaunch(const wchar_t *exe, const wchar_t *verb) {
    SHELLEXECUTEINFOW sei = {.cbSize = sizeof(sei),
                             .fMask = SEE_MASK_NOCLOSEPROCESS,
                             .lpVerb = verb,
                             .lpFile = exe,
                             .nShow = SW_SHOWNORMAL};
    if (!ShellExecuteExW(&sei))
        return INVALID_HANDLE_VALUE;
    if (sei.hProcess)
        AllowSetForegroundWindow(GetProcessId(sei.hProcess));
    return sei.hProcess;
}

static BOOL LookupAppPath(const wchar_t *exe, wchar_t *fullPath, DWORD cb) {
    wchar_t regKey[MAX_PATH + 64];
    StringCchPrintfW(regKey, countof(regKey),
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                     L"\\App Paths\\%s",
                     exe);
    DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD sz = cb;
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, regKey, NULL, flags, NULL,
                              fullPath, &sz);
    if (st != ERROR_SUCCESS) {
        sz = cb;
        st = RegGetValueW(HKEY_LOCAL_MACHINE, regKey, NULL, flags, NULL,
                          fullPath, &sz);
    }
    return st == ERROR_SUCCESS && fullPath[0];
}

static HANDLE DirectLaunch(const wchar_t *exe) {
    STARTUPINFOW si = {.cb = sizeof(si),
                       .dwFlags = STARTF_USESHOWWINDOW,
                       .wShowWindow = SW_SHOWNORMAL};
    PROCESS_INFORMATION pi = {0};
    wchar_t cmdLine[MAX_PATH + 2];

    StringCchPrintfW(cmdLine, countof(cmdLine), L"\"%s\"", exe);
    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si,
                        &pi)) {
        if (GetLastError() == ERROR_ELEVATION_REQUIRED)
            return ShellLaunch(exe, L"open");

        wchar_t fullPath[MAX_PATH] = {0};
        if (!LookupAppPath(exe, fullPath, sizeof(fullPath)))
            return INVALID_HANDLE_VALUE;

        wchar_t workDir[MAX_PATH];
        StringCchCopyW(workDir, MAX_PATH, fullPath);
        wchar_t *slash = wcsrchr(workDir, L'\\');
        if (slash)
            *slash = L'\0';

        StringCchPrintfW(cmdLine, countof(cmdLine), L"\"%s\"", fullPath);
        if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, workDir,
                            &si, &pi)) {
            if (GetLastError() == ERROR_ELEVATION_REQUIRED)
                return ShellLaunch(fullPath, L"open");
            return INVALID_HANDLE_VALUE;
        }
    }

    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void FocusOrLaunch(FindCtx *ctx, const wchar_t *launchExe, BOOL admin) {
    if (ctx->found) {
        FocusHwnd(ctx->found);
        return;
    }

    long long tL = StartMeasuring();
    HANDLE hProcess;

    if (admin) {
        hProcess = ShellLaunch(launchExe, L"runas");
    } else if (IsElevated()) {
        wchar_t fullPath[MAX_PATH] = {0};
        const wchar_t *target =
            LookupAppPath(launchExe, fullPath, sizeof(fullPath)) ? fullPath
                                                                 : launchExe;
        hProcess = ShellExecAsUser(target);
        if (hProcess != INVALID_HANDLE_VALUE)
            AllowSetForegroundWindow(GetProcessId(hProcess));
        else
            hProcess = ShellLaunch(launchExe, L"open");
    } else {
        hProcess = DirectLaunch(launchExe);
    }

    Log(LOG_PERF, L"launch %ls: %.2f ms", launchExe, FinishMeasuring(tL));
    if (hProcess == INVALID_HANDLE_VALUE)
        return;

    LaunchPollCtx *lp = malloc(sizeof(LaunchPollCtx));
    if (!lp) {
        if (hProcess)
            CloseHandle(hProcess);
        return;
    }
    StringCchCopyW(lp->exe, EXE_NAME_MAX, ctx->exe);
    lp->cls = ctx->cls;
    lp->hProcess = hProcess;
    HANDLE t = CreateThread(NULL, 0, LaunchPollThread, lp, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        if (lp->hProcess)
            CloseHandle(lp->hProcess);
        free(lp);
    }
}

int ProcessAppCommand(const wchar_t *arg, BOOL global, BOOL admin) {
    long long t0 = StartMeasuring();
    wchar_t matchExe[MAX_PATH], launchExe[MAX_PATH];
    ResolveTarget(arg, matchExe, launchExe, MAX_PATH);
    const wchar_t *cls = AutoClass(matchExe);
    if (!global) {
        HWND cached = LookupHwndCache(matchExe, cls);
        if (cached) {
            Log(LOG_PERF, L"app %ls: hwnd-cache hit %.2f ms", matchExe,
                FinishMeasuring(t0));
            FocusHwnd(cached);
            return 0;
        }
    }
    MaybeRebuildPidCache();
    long long t1 = StartMeasuring();
    FindCtx ctx = {.exe = matchExe, .cls = cls, .global = global};
    EnumWindows(EnumProc, (LPARAM)&ctx);
    Log(LOG_PERF, L"app %ls: EnumWindows %.2f ms found=%d", matchExe,
        FinishMeasuring(t1), ctx.found != NULL);
    if (ctx.found && !global)
        StoreHwndCache(matchExe, ctx.found);
    FocusOrLaunch(&ctx, launchExe, admin);
    Log(LOG_PERF, L"app %ls: total %.2f ms", matchExe, FinishMeasuring(t0));
    return 0;
}
