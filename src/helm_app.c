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
    MaybeRebuildPidCache();
    DWORD explorerPid = GetPidFromExe(L"explorer.exe");
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
    ok = CreateProcessWithTokenW(hDup, LOGON_WITH_PROFILE, NULL, cmd, 0, NULL,
                                 NULL, &si, &pi);
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
static DWORD WINAPI LaunchPollThread(LPVOID param) {
    LaunchPollCtx *lp = (LaunchPollCtx *)param;
    long long t0 = StartMeasuring();

    /* Fast path: wait until the app's message pump is running, then scan
     * once. Covers most GUI apps (browsers, terminals, editors). */
    if (lp->hProcess) {
        WaitForInputIdle(lp->hProcess, 5000);
        CloseHandle(lp->hProcess);
        lp->hProcess = NULL;
        MaybeRebuildPidCache();
        FindCtx ctx = {.exe = lp->exe, .cls = lp->cls, .polling = TRUE};
        EnumWindows(EnumProc, (LPARAM)&ctx);
        if (ctx.found) {
            Log(LOG_PERF, L"launch %ls: fast-path %.0f ms", lp->exe,
                FinishMeasuring(t0));
            FocusHwnd(ctx.found);
            free(lp);
            return 0;
        }
        Log(LOG_PERF, L"launch %ls: fast-path miss %.0f ms, falling back",
            lp->exe, FinishMeasuring(t0));
    }

    /* Fallback: console wrappers, Electron, or no process handle.
     * Force cache rebuild every iteration so TTL does not add latency. */
    int delay = 30;
    for (int i = 0; i < 16; i++) {
        Sleep(delay);
        delay = delay < 200 ? delay * 2 : 200;
        ForceRebuildPidCache();
        FindCtx ctx = {.exe = lp->exe, .cls = lp->cls, .polling = TRUE};
        EnumWindows(EnumProc, (LPARAM)&ctx);
        if (ctx.found) {
            Log(LOG_PERF, L"launch %ls: fallback iter %d %.0f ms", lp->exe,
                i + 1, FinishMeasuring(t0));
            FocusHwnd(ctx.found);
            break;
        }
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

static void FocusOrLaunch(FindCtx *ctx, const wchar_t *launchExe, BOOL admin) {
    if (ctx->found) {
        FocusHwnd(ctx->found);
        return;
    }

    HANDLE hProcess = INVALID_HANDLE_VALUE;

    if (!admin && IsElevated()) {
        wchar_t fullPath[MAX_PATH] = {0};
        BOOL hasSep =
            wcschr(launchExe, L'\\') != NULL || wcschr(launchExe, L'/') != NULL;
        const wchar_t *target = launchExe;
        if (!hasSep && LookupAppPath(launchExe, fullPath, sizeof(fullPath)))
            target = fullPath;
        long long tE = StartMeasuring();
        hProcess = ShellExecAsUser(target);
        Log(LOG_PERF, L"ShellExecAsUser %ls: %.2f ms", launchExe,
            FinishMeasuring(tE));
        if (hProcess != INVALID_HANDLE_VALUE)
            AllowSetForegroundWindow(GetProcessId(hProcess));
    }
    if (hProcess == INVALID_HANDLE_VALUE) {
        long long tL = StartMeasuring();
        hProcess = ShellLaunch(launchExe, admin ? L"runas" : L"open");
        Log(LOG_PERF, L"ShellLaunch %ls: %.2f ms", launchExe,
            FinishMeasuring(tL));
        if (hProcess == INVALID_HANDLE_VALUE)
            return;
    }

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
