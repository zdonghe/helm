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

BOOL ShellExecAsUser(const wchar_t *file, const wchar_t *verb) {
    MaybeRebuildPidCache();
    DWORD explorerPid = GetPidFromExe(L"explorer.exe");
    if (!explorerPid)
        return FALSE;

    /* Open explorer and duplicate its (medium integrity) token */
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, explorerPid);
    if (!hProc)
        return FALSE;

    HANDLE hToken = NULL;
    BOOL ok = OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hToken);
    CloseHandle(hProc);
    if (!ok)
        return FALSE;

    HANDLE hDup = NULL;
    ok = DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                          TokenPrimary, &hDup);
    CloseHandle(hToken);
    if (!ok)
        return FALSE;

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
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok;
}
static void ResolveTarget(const wchar_t *in, wchar_t *matchExe,
                          wchar_t *launchExe, size_t sz) {
    const wchar_t *base = wcsrchr(in, L'\\');
    base = base ? base + 1 : in;
    if (lstrcmpiW(base, L"wt") == 0 ||
        lstrcmpiW(base, L"windowsterminal") == 0) {
        StringCchCopyW(matchExe, sz, L"WindowsTerminal.exe");
        StringCchCopyW(launchExe, sz, L"wt.exe");
        return;
    }
    StringCchCopyW(matchExe, sz, base);
    size_t len = wcslen(matchExe);
    if (len < 4 || _wcsicmp(matchExe + len - 4, L".exe") != 0)
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
    int matchCount;
    BOOL global;
    BOOL stopOnFirst;
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
    BOOL match = (exe && lstrcmpiW(exe, ctx->exe) == 0);
    if (!match)
        return TRUE;

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return TRUE;

    /* Filter to current virtual desktop only in non-poll, non-global mode.
     * During launch poll the new window's desktop assignment is unstable.
     */
    if (!ctx->stopOnFirst && !ctx->global && Vdm) {
        BOOL onCurrent = FALSE;
        if (SUCCEEDED(IVDM_IsCurrent(Vdm, hwnd, &onCurrent)) && !onCurrent)
            return TRUE;
    }

    if (!ctx->found)
        ctx->found = hwnd;
    ctx->matchCount++;
    return ctx->stopOnFirst ? FALSE : TRUE;
}

void BypassForegroundLock(void) {
    INPUT dummy = {.type = INPUT_MOUSE};
    SendInput(1, &dummy, sizeof(INPUT));
}

typedef struct {
    wchar_t exe[EXE_NAME_MAX];
    wchar_t cls[256];
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

    /* Fast path: wait until the app's message pump is running, then scan
     * once. Covers most GUI apps (browsers, terminals, editors). */
    if (lp->hProcess) {
        WaitForInputIdle(lp->hProcess, 5000);
        CloseHandle(lp->hProcess);
        lp->hProcess = NULL;
        MaybeRebuildPidCache();
        FindCtx ctx = {.exe = lp->exe,
                       .cls = lp->cls[0] ? lp->cls : NULL,
                       .stopOnFirst = TRUE};
        EnumWindows(EnumProc, (LPARAM)&ctx);
        if (ctx.found) {
            FocusHwnd(ctx.found);
            free(lp);
            return 0;
        }
    }

    /* Fallback: console wrappers, Electron, or no process handle.
     * Force cache rebuild every iteration so TTL does not add latency. */
    int delay = 30;
    for (int i = 0; i < 16; i++) {
        Sleep(delay);
        delay = delay < 200 ? delay * 2 : 200;
        ForceRebuildPidCache();
        FindCtx ctx = {.exe = lp->exe,
                       .cls = lp->cls[0] ? lp->cls : NULL,
                       .stopOnFirst = TRUE};
        EnumWindows(EnumProc, (LPARAM)&ctx);
        if (ctx.found) {
            FocusHwnd(ctx.found);
            break;
        }
    }
    free(lp);
    return 0;
}

static void FocusOrLaunch(FindCtx *ctx, const wchar_t *launchExe, BOOL admin) {
    if (ctx->found) {
        FocusHwnd(ctx->found);
        return;
    }

    HANDLE hProcess = NULL;

    if (admin || !IsElevated()) {
        SHELLEXECUTEINFOW sei = {.cbSize = sizeof(sei),
                                 .fMask = SEE_MASK_NOCLOSEPROCESS,
                                 .lpVerb = admin ? L"runas" : L"open",
                                 .lpFile = launchExe,
                                 .nShow = SW_SHOWNORMAL};
        if (!ShellExecuteExW(&sei))
            return;
        if (sei.hProcess) {
            AllowSetForegroundWindow(GetProcessId(sei.hProcess));
            hProcess = sei.hProcess; /* thread takes ownership */
        }
    } else if (!ShellExecAsUser(launchExe, NULL)) {
        OutputDebugStringW(
            L"[helm] ShellExecAsUser failed, launching elevated\n");
        SHELLEXECUTEINFOW sei = {.cbSize = sizeof(sei),
                                 .fMask = SEE_MASK_NOCLOSEPROCESS,
                                 .lpVerb = L"open",
                                 .lpFile = launchExe,
                                 .nShow = SW_SHOWNORMAL};
        if (!ShellExecuteExW(&sei))
            return;
        if (sei.hProcess) {
            AllowSetForegroundWindow(GetProcessId(sei.hProcess));
            hProcess = sei.hProcess;
        }
    } else {
        AllowSetForegroundWindow(ASFW_ANY);
        /* ShellExecAsUser path: no handle, hProcess stays NULL */
    }

    LaunchPollCtx *lp = malloc(sizeof(LaunchPollCtx));
    if (!lp) {
        if (hProcess)
            CloseHandle(hProcess);
        return;
    }
    StringCchCopyW(lp->exe, EXE_NAME_MAX, ctx->exe);
    StringCchCopyW(lp->cls, 256, ctx->cls ? ctx->cls : L"");
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
    wchar_t matchExe[MAX_PATH], launchExe[MAX_PATH];
    ResolveTarget(arg, matchExe, launchExe, MAX_PATH);
    MaybeRebuildPidCache();
    if (!global) {
        HWND cached = LookupHwndCache(matchExe);
        if (cached) {
            BOOL onCurrent = TRUE;
            if (Vdm)
                IVDM_IsCurrent(Vdm, cached, &onCurrent);
            if (onCurrent) {
                FocusHwnd(cached);
                return 0;
            }
        }
    }
    FindCtx ctx = {
        .exe = matchExe, .cls = AutoClass(matchExe), .global = global};
    EnumWindows(EnumProc, (LPARAM)&ctx);
    if (ctx.found && !global) {
        if (ctx.matchCount == 1)
            StoreHwndCache(matchExe, ctx.found);
        else
            EvictHwndCache(matchExe);
    }
    FocusOrLaunch(&ctx, launchExe, admin);
    return 0;
}
