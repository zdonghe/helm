#include "helm.h"

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
    BOOL pollMode;
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
    BOOL match = FALSE;
    if (ctx->pollMode) {
        HANDLE proc =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc)
            return TRUE;
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
            const wchar_t *base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            match = (lstrcmpiW(base, ctx->exe) == 0);
        }
        CloseHandle(proc);
    } else {
        const wchar_t *exe = GetExeFromPid(pid);
        match = (exe && lstrcmpiW(exe, ctx->exe) == 0);
    }
    if (!match)
        return TRUE;

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return TRUE;

    /* Filter to current virtual desktop only in non-poll, non-global mode.
     * During launch poll the new window's desktop assignment is unstable. */
    if (!ctx->pollMode && !ctx->global && Vdm) {
        BOOL onCurrent = FALSE;
        if (SUCCEEDED(IVDM_IsCurrent(Vdm, hwnd, &onCurrent)) && !onCurrent)
            return TRUE;
    }

    if (!ctx->found)
        ctx->found = hwnd;
    ctx->matchCount++;
    return ctx->pollMode ? FALSE : TRUE;
}

void BypassForegroundLock(void) {
    INPUT dummy = {.type = INPUT_MOUSE};
    SendInput(1, &dummy, sizeof(INPUT));
}

typedef struct {
    wchar_t exe[EXE_NAME_MAX];
    wchar_t cls[256];
} LaunchPollCtx;

static DWORD WINAPI LaunchPollThread(LPVOID param) {
    LaunchPollCtx *lp = (LaunchPollCtx *)param;
    for (int i = 0; i < 60; i++) {
        Sleep(50);
        FindCtx ctx = {.exe = lp->exe,
                       .cls = lp->cls[0] ? lp->cls : NULL,
                       .pollMode = TRUE};
        EnumWindows(EnumProc, (LPARAM)&ctx);
        if (ctx.found) {
            if (IsIconic(ctx.found))
                ShowWindow(ctx.found, SW_RESTORE);
            BypassForegroundLock();
            SetForegroundWindow(ctx.found);
            break;
        }
    }
    free(lp);
    return 0;
}

static void FocusHwnd(HWND h) {
    if (IsIconic(h))
        ShowWindow(h, SW_RESTORE);
    BypassForegroundLock();
    SetForegroundWindow(h);
}

static void FocusOrLaunch(FindCtx *ctx, const wchar_t *launchExe) {
    if (ctx->found) {
        FocusHwnd(ctx->found);
        return;
    }
    SHELLEXECUTEINFOW sei = {.cbSize = sizeof(sei),
                             .fMask = SEE_MASK_NOCLOSEPROCESS,
                             .lpVerb = L"open",
                             .lpFile = launchExe,
                             .nShow = SW_SHOWNORMAL};
    if (!ShellExecuteExW(&sei))
        return;
    if (sei.hProcess) {
        AllowSetForegroundWindow(GetProcessId(sei.hProcess));
        CloseHandle(sei.hProcess);
    }
    LaunchPollCtx *lp = malloc(sizeof(LaunchPollCtx));
    if (!lp)
        return;
    StringCchCopyW(lp->exe, EXE_NAME_MAX, ctx->exe);
    StringCchCopyW(lp->cls, 256, ctx->cls ? ctx->cls : L"");
    HANDLE t = CreateThread(NULL, 0, LaunchPollThread, lp, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        free(lp);
}

int ProcessAppCommand(const wchar_t *arg, BOOL global) {
    wchar_t matchExe[MAX_PATH], launchExe[MAX_PATH];
    ResolveTarget(arg, matchExe, launchExe, MAX_PATH);
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
    MaybeRebuildPidCache();
    FindCtx ctx = {
        .exe = matchExe, .cls = AutoClass(matchExe), .global = global};
    EnumWindows(EnumProc, (LPARAM)&ctx);
    if (ctx.found && !global && ctx.matchCount == 1)
        StoreHwndCache(matchExe, ctx.found);
    FocusOrLaunch(&ctx, launchExe);
    return 0;
}
