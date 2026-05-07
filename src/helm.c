#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>

#include <dwmapi.h>
#include <objbase.h>
#include <pathcch.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <wchar.h>

/* ============================================================
 * COM interfaces
 * ============================================================ */

/* IUnknown preamble shared by every COM vtable below. */
#define COM_IUNK_VTBL(T)                                                       \
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **);          \
    ULONG(STDMETHODCALLTYPE *AddRef)(T *);                                     \
    ULONG(STDMETHODCALLTYPE *Release)(T *)

/* --- IVirtualDesktopManager  --- */
static const CLSID CLSID_VDM = {
    0xaa509086, 0x5ca9, 0x4c25,
    {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
static const IID IID_IVDM = {0xa5cd92ff, 0x29be, 0x454c,
                             {0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b}};
typedef struct IVDM IVDM;
typedef struct {
    COM_IUNK_VTBL(IVDM);
    HRESULT(STDMETHODCALLTYPE *IsWindowOnCurrentVirtualDesktop)(IVDM *, HWND,
                                                                BOOL *);
    HRESULT(STDMETHODCALLTYPE *GetWindowDesktopId)(IVDM *, HWND, GUID *);
    HRESULT(STDMETHODCALLTYPE *MoveWindowToDesktop)(IVDM *, HWND, REFGUID);
} IVDMVtbl;
struct IVDM {
    const IVDMVtbl *lpVtbl;
};
#define IVDM_IsCurrent(p, h, out)                                              \
    (p)->lpVtbl->IsWindowOnCurrentVirtualDesktop(p, h, out)
#define IVDM_Release(p) (p)->lpVtbl->Release(p)

static IVDM *Vdm = NULL; /* initialized in RunServer */

/* --- IServiceProvider (for bootstrapping internal interfaces) --- */
static const IID IID_ISvcProv = {0x6D5140C1, 0x7436, 0x11CE,
                                 {0x80, 0x34, 0x00, 0xAA,
                                  0x00, 0x60, 0x09, 0xFA}};
typedef struct ISvcProv ISvcProv;
typedef struct {
    COM_IUNK_VTBL(ISvcProv);
    HRESULT(STDMETHODCALLTYPE *QueryService)(ISvcProv *, REFGUID sid,
                                             REFIID iid, void **);
} ISvcProvVtbl;
struct ISvcProv {
    const ISvcProvVtbl *lpVtbl;
};

/* --- IObjectArray (returned by GetDesktops) --- */
typedef struct IObjectArray IObjectArray;
typedef struct {
    COM_IUNK_VTBL(IObjectArray);
    HRESULT(STDMETHODCALLTYPE *GetCount)(IObjectArray *, UINT *);
    HRESULT(STDMETHODCALLTYPE *GetAt)(IObjectArray *, UINT, REFIID, void **);
} IObjectArrayVtbl;
struct IObjectArray {
    const IObjectArrayVtbl *lpVtbl;
};

/*
 * IVirtualDesktop - Win11 22H2+ vtable layout:
 *   [3] IsViewVisible(IApplicationView*, BOOL*)
 *   [4] GetID(GUID*)
 *
 * Win10 layout differs: GetID is at [3]. Adjust if needed.
 */
static const IID IID_IVirtualDesktop = {0x3F07F4BE, 0xB107, 0x441A,
                                        {0xAF, 0x0F, 0x39, 0xD8,
                                         0x25, 0x29, 0x07, 0x2C}};
typedef struct IVirtualDesktop IVirtualDesktop;
typedef struct {
    COM_IUNK_VTBL(IVirtualDesktop);
    HRESULT(STDMETHODCALLTYPE *IsViewVisible)(IVirtualDesktop *, void *,
                                              BOOL *);
    HRESULT(STDMETHODCALLTYPE *GetID)(IVirtualDesktop *, GUID *);
} IVirtualDesktopVtbl;
struct IVirtualDesktop {
    const IVirtualDesktopVtbl *lpVtbl;
};

/*
 * IVirtualDesktopManagerInternal - obtained via IServiceProvider::QueryService
 * on CLSID_ImmersiveShell, using CLSID_VDMI as the service ID and IID_IVDMI
 * as the interface ID.
 *
 * CLSID_VDMI (service SID) is stable across all Windows versions:
 *   {C5E0CDCA-7B6E-41B2-9FC4-D93975CC467B}
 *
 * IID_IVDMI changes with Windows builds:
 *   Win10:              {F31574D6-B682-4CDC-BD56-1827860ABEC6}
 *   Win11 21H2–22H1:   {B2F925B9-5A0F-4D2E-9F4D-2B1507593C10}
 *   Win11 23H2–25H2+:  {53F5CA0B-158F-4124-900C-057158060B27}
 *
 * Vtable layout (slots 3+) is shared across 23H2–25H2:
 *   [3]  GetCount
 *   [4]  MoveViewToDesktop
 *   [5]  CanViewMoveDesktops
 *   [6]  GetCurrentDesktop
 *   [7]  GetDesktops
 *   [8]  GetAdjacentDesktop
 *   [9]  SwitchDesktop               <- used by helm
 *   [10] SwitchDesktopAndMoveForegroundView  (24H2+ only; slot unused by helm)
 *   [11] CreateDesktopW
 *   ...
 *
 * If a future Windows update breaks this, check MScholtes/VirtualDesktop on
 * GitHub - Markus Scholtes maintains per-build interface files and typically
 * updates within days of a new Windows release.
 */
static const CLSID CLSID_ImmersiveShell = {0xC2F03A33, 0x21F5, 0x47FA,
                                           {0xB4, 0xBB, 0x15, 0x63,
                                            0x62, 0xA2, 0xF2, 0x39}};
/* Stable service identifier - used as the SID argument to QueryService */
static const GUID CLSID_VDMI = {0xC5E0CDCA, 0x7B6E, 0x41B2,
                                {0x9F, 0xC4, 0xD9, 0x39,
                                 0x75, 0xCC, 0x46, 0x7B}};
/* Interface IID - changes with Windows builds; update here when it does */
static const IID IID_IVDMI = {0x53F5CA0B, 0x158F, 0x4124,
                              {0x90, 0x0C, 0x05, 0x71,
                               0x58, 0x06, 0x0B, 0x27}};
typedef struct IVDMI IVDMI;
typedef struct {
    COM_IUNK_VTBL(IVDMI);
    HRESULT(STDMETHODCALLTYPE *GetCount)(IVDMI *, UINT *);
    HRESULT(STDMETHODCALLTYPE *MoveViewToDesktop)(IVDMI *, void *,
                                                  IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *CanViewMoveDesktops)(IVDMI *, void *, BOOL *);
    HRESULT(STDMETHODCALLTYPE *GetCurrentDesktop)(IVDMI *, IVirtualDesktop **);
    HRESULT(STDMETHODCALLTYPE *GetDesktops)(IVDMI *, IObjectArray **);
    HRESULT(STDMETHODCALLTYPE *GetAdjacentDesktop)(IVDMI *, IVirtualDesktop *,
                                                   int, IVirtualDesktop **);
    HRESULT(STDMETHODCALLTYPE *SwitchDesktop)(IVDMI *, IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *SwitchDesktopAndMoveForegroundView)(
        IVDMI *, IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *CreateDesktopW)(IVDMI *, IVirtualDesktop **);
    HRESULT(STDMETHODCALLTYPE *RemoveDesktop)(IVDMI *, IVirtualDesktop *,
                                              IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *FindDesktop)(IVDMI *, GUID *,
                                            IVirtualDesktop **);
} IVDMIVtbl;
struct IVDMI {
    const IVDMIVtbl *lpVtbl;
};

static IVDMI *VdmInternal = NULL; /* initialized in RunServer */

/* ============================================================
 * PID / HWND caches
 * ============================================================ */

#define MAX_PIDS 2048
#define EXE_NAME_MAX 64
typedef struct {
    DWORD pid;
    wchar_t exe[EXE_NAME_MAX];
} PidEntry;
static PidEntry PidCache[MAX_PIDS];
static int PidCount = 0;
static ULONGLONG PidCacheExpiry = 0;
#define PIDCACHE_TTL_MS 300

static int CmpPid(const void *a, const void *b) {
    DWORD pa = ((const PidEntry *)a)->pid;
    DWORD pb = ((const PidEntry *)b)->pid;
    return (pa > pb) - (pa < pb);
}

#define HWND_CACHE_SIZE 16
static struct {
    wchar_t exe[EXE_NAME_MAX]; /* basenames only — was MAX_PATH, ~6KB BSS saved */
    HWND hwnd;
} HwndCache[HWND_CACHE_SIZE];
static int HwndCacheCount = 0;
static int HwndCacheNext = 0;

static void BuildPidCache(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W pe = {.dwSize = sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (PidCount >= MAX_PIDS)
                break;
            PidCache[PidCount].pid = pe.th32ProcessID;
            StringCchCopyW(PidCache[PidCount].exe, EXE_NAME_MAX, pe.szExeFile);
            PidCount++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    qsort(PidCache, PidCount, sizeof(PidEntry), CmpPid);
}

static void MaybeRebuildPidCache(void) {
    ULONGLONG now = GetTickCount64();
    if (now < PidCacheExpiry)
        return;
    PidCount = 0;
    BuildPidCache();
    PidCacheExpiry = now + PIDCACHE_TTL_MS;
}

static const wchar_t *GetExeFromPid(DWORD pid) {
    PidEntry key = {.pid = pid};
    PidEntry *found =
        (PidEntry *)bsearch(&key, PidCache, PidCount, sizeof(PidEntry), CmpPid);
    return found ? found->exe : NULL;
}

static HWND LookupHwndCache(const wchar_t *exe) {
    for (int i = 0; i < HwndCacheCount; i++) {
        if (lstrcmpiW(HwndCache[i].exe, exe) != 0)
            continue;
        HWND h = HwndCache[i].hwnd;
        int cloaked = 0;
        if (IsWindow(h) && IsWindowVisible(h)) {
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!cloaked)
                return h;
        }
        HwndCache[i] = HwndCache[--HwndCacheCount]; /* swap-and-pop */
        return NULL;
    }
    return NULL;
}

static void StoreHwndCache(const wchar_t *exe, HWND hwnd) {
    for (int i = 0; i < HwndCacheCount; i++) {
        if (lstrcmpiW(HwndCache[i].exe, exe) == 0) {
            HwndCache[i].hwnd = hwnd;
            return;
        }
    }
    int slot;
    if (HwndCacheCount < HWND_CACHE_SIZE) {
        slot = HwndCacheCount++;
    } else {
        slot = HwndCacheNext;
        HwndCacheNext = (HwndCacheNext + 1) % HWND_CACHE_SIZE;
    }
    StringCchCopyW(HwndCache[slot].exe, EXE_NAME_MAX, exe);
    HwndCache[slot].hwnd = hwnd;
}

/* ============================================================
 * App focus / launch
 * ============================================================ */

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
    BOOL pollMode; /* TRUE during launch poll: PID cache may be stale, so
                      resolve the exe via OpenProcess and stop at first match */
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
    return ctx->pollMode ? FALSE : TRUE; /* poll: stop at first match */
}

static void BypassForegroundLock(void) {
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
        /* Delegate foreground to direct child. Launcher chains (wt, electron,
         * steam) spawn grandchildren, so delegation misses - poll thread
         * picks up focus in that case. */
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

static int ProcessAppCommand(const wchar_t *arg, BOOL global) {
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
    FindCtx ctx = {.exe = matchExe, .cls = AutoClass(matchExe),
                   .global = global};
    EnumWindows(EnumProc, (LPARAM)&ctx);
    if (ctx.found && !global && ctx.matchCount == 1)
        StoreHwndCache(matchExe, ctx.found);
    FocusOrLaunch(&ctx, launchExe);
    return 0;
}

/* ============================================================
 * Virtual desktop commands
 * ============================================================ */

static void InitVdInternal(void) {
    ISvcProv *sp = NULL;
    HRESULT hr =
        CoCreateInstance(&CLSID_ImmersiveShell, NULL, CLSCTX_LOCAL_SERVER,
                         &IID_ISvcProv, (void **)&sp);
    if (FAILED(hr))
        return;
    sp->lpVtbl->QueryService(sp, &CLSID_VDMI, &IID_IVDMI,
                             (void **)&VdmInternal);
    sp->lpVtbl->Release(sp);
}

/* Returns desktop at 0-based index n, caller must Release. */
static IVirtualDesktop *GetDesktopN(int n) {
    if (!VdmInternal)
        return NULL;
    IObjectArray *arr = NULL;
    if (FAILED(VdmInternal->lpVtbl->GetDesktops(VdmInternal, &arr)))
        return NULL;
    UINT count = 0;
    arr->lpVtbl->GetCount(arr, &count);
    IVirtualDesktop *desk = NULL;
    if ((UINT)n < count)
        arr->lpVtbl->GetAt(arr, (UINT)n, &IID_IVirtualDesktop, (void **)&desk);
    arr->lpVtbl->Release(arr);
    return desk;
}

/*
 * vd:N          - switch to desktop N (1-indexed)
 * vd:send:N     - move foreground window to desktop N then switch
 */
static int ProcessVdCommand(const wchar_t *arg) {
    BOOL send = FALSE;
    const wchar_t *numStr = arg;
    if (wcsncmp(arg, L"send:", 5) == 0) {
        send = TRUE;
        numStr = arg + 5;
    }
    int n = _wtoi(numStr);
    if (n < 1)
        return 1;

    IVirtualDesktop *desk = GetDesktopN(n - 1);
    if (!desk)
        return 1;

    if (send) {
        VdmInternal->lpVtbl->SwitchDesktopAndMoveForegroundView(VdmInternal,
                                                                desk);
    } else {
        VdmInternal->lpVtbl->SwitchDesktop(VdmInternal, desk);
    }
    desk->lpVtbl->Release(desk);
    return 0;
}

/* ============================================================
 * Window resize / snap commands
 * ============================================================ */

/*
 * AdjacentCtx - used by FindAdjacentProc to locate a window sharing an edge
 * with the foreground window.
 *
 * Set exactly ONE of edgeX / edgeY / rightEdgeX / bottomEdgeY to the pixel
 * coordinate of the shared edge; leave the others at -1.
 *
 * refRect must be the rect of the window being resized. It is used to filter
 * out windows that share the edge coordinate but don't actually overlap in the
 * perpendicular axis (e.g. a stacked window on the far side of the screen
 * whose left edge happens to land at the same X).
 */
typedef struct {
    int edgeX, edgeY; /* match candidate's left / top edge */
    int rightEdgeX;   /* match candidate's right edge, -1 = don't care */
    int bottomEdgeY;  /* match candidate's bottom edge, -1 = don't care */
    int tolerance;
    HWND skip;
    HWND found;
    RECT refRect; /* fg window rect - used for perpendicular overlap check */
} AdjacentCtx;

static BOOL CALLBACK FindAdjacentProc(HWND hwnd, LPARAM lp) {
    AdjacentCtx *ctx = (AdjacentCtx *)lp;
    if (hwnd == ctx->skip)
        return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        return TRUE;

    RECT r;
    GetWindowRect(hwnd, &r);

    BOOL edgeMatch = FALSE, horizontal = FALSE;
    if (ctx->edgeX >= 0 && abs(r.left - ctx->edgeX) <= ctx->tolerance) {
        edgeMatch = TRUE;
        horizontal = TRUE;
    } else if (ctx->rightEdgeX >= 0 &&
               abs(r.right - ctx->rightEdgeX) <= ctx->tolerance) {
        edgeMatch = TRUE;
        horizontal = TRUE;
    } else if (ctx->edgeY >= 0 && abs(r.top - ctx->edgeY) <= ctx->tolerance) {
        edgeMatch = TRUE;
    } else if (ctx->bottomEdgeY >= 0 &&
               abs(r.bottom - ctx->bottomEdgeY) <= ctx->tolerance) {
        edgeMatch = TRUE;
    }
    if (!edgeMatch)
        return TRUE;

    const RECT *ref = &ctx->refRect;
    if (horizontal) {
        if (r.bottom <= ref->top || r.top >= ref->bottom)
            return TRUE;
    } else {
        if (r.right <= ref->left || r.left >= ref->right)
            return TRUE;
    }

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return TRUE;

    ctx->found = hwnd;
    return FALSE;
}
/* SWP_RESIZE flags: keep Z-order/activation untouched, and post the change
 * asynchronously. SWP_ASYNCWINDOWPOS is critical here — the helm daemon
 * thread is blocked on the named-pipe ReadFile and does not pump a message
 * queue. Without ASYNC, SetWindowPos on a window owned by a different thread
 * can hang or fail silently while it waits for the target thread to ack. */
#define SWP_RESIZE                                                             \
    (SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS)
#define MIN_DIM 50 /* prevent collapsing a window to zero / negative size */

static int ProcessSzCommand(const wchar_t *arg) {
    const wchar_t *colon = wcschr(arg, L':');
    if (!colon)
        return 1;
    int pct = _wtoi(colon + 1);
    if (pct == 0)
        return 0;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;
    /* SetWindowPos on a minimized window is a no-op visually; restore first
     * so the user sees the resize land. */
    if (IsIconic(fg))
        ShowWindow(fg, SW_RESTORE);

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi))
        return 1;
    const RECT wa = mi.rcWork;
    const int monW = wa.right - wa.left;
    const int monH = wa.bottom - wa.top;

    RECT fg0;
    GetWindowRect(fg, &fg0);

    AdjacentCtx adj = {.edgeX = -1,
                       .edgeY = -1,
                       .rightEdgeX = -1,
                       .bottomEdgeY = -1,
                       .tolerance = 20,
                       .skip = fg,
                       .found = NULL,
                       .refRect = fg0};

    RECT newFg = fg0, newAdj = {0};
    int newEdge;

    if (wcsncmp(arg, L"right", 5) == 0) {
        newEdge = fg0.right + (pct * monW) / 100;
        if (newEdge > wa.right)
            newEdge = wa.right;
        if (newEdge < fg0.left + MIN_DIM)
            newEdge = fg0.left + MIN_DIM;
        adj.edgeX = fg0.right; /* neighbour whose left == fg.right */
        EnumWindows(FindAdjacentProc, (LPARAM)&adj);
        newFg.right = newEdge;
        if (adj.found) {
            GetWindowRect(adj.found, &newAdj);
            /* Preserve the original edge offset so invisible DWM borders
             * (Win11 22H2+: ~7px per side) keep both windows visually flush.
             * Without this, setting both edges to newEdge creates a ~14px gap.
             */
            int off = newAdj.left - fg0.right; /* typically -14 on Win11 */
            newAdj.left = newEdge + off;
            if (newAdj.right - newAdj.left < MIN_DIM) {
                newAdj.left = newAdj.right - MIN_DIM;
                newFg.right = newAdj.left - off; /* keep fg flush with adj */
            }
        }
    } else if (wcsncmp(arg, L"left", 4) == 0) {
        newEdge = fg0.left - (pct * monW) / 100;
        if (newEdge < wa.left)
            newEdge = wa.left;
        if (newEdge > fg0.right - MIN_DIM)
            newEdge = fg0.right - MIN_DIM;
        adj.rightEdgeX = fg0.left; /* neighbour whose right == fg.left */
        EnumWindows(FindAdjacentProc, (LPARAM)&adj);
        newFg.left = newEdge;
        if (adj.found) {
            GetWindowRect(adj.found, &newAdj);
            int off = newAdj.right - fg0.left; /* typically +14 on Win11 */
            newAdj.right = newEdge + off;
            if (newAdj.right - newAdj.left < MIN_DIM) {
                newAdj.right = newAdj.left + MIN_DIM;
                newFg.left = newAdj.right - off;
            }
        }
    } else if (wcsncmp(arg, L"down", 4) == 0) {
        newEdge = fg0.bottom + (pct * monH) / 100;
        if (newEdge > wa.bottom)
            newEdge = wa.bottom;
        if (newEdge < fg0.top + MIN_DIM)
            newEdge = fg0.top + MIN_DIM;
        adj.edgeY = fg0.bottom; /* neighbour whose top == fg.bottom */
        EnumWindows(FindAdjacentProc, (LPARAM)&adj);
        newFg.bottom = newEdge;
        if (adj.found) {
            GetWindowRect(adj.found, &newAdj);
            int off = newAdj.top - fg0.bottom; /* typically -14 on Win11 */
            newAdj.top = newEdge + off;
            if (newAdj.bottom - newAdj.top < MIN_DIM) {
                newAdj.top = newAdj.bottom - MIN_DIM;
                newFg.bottom = newAdj.top - off;
            }
        }
    } else if (wcsncmp(arg, L"up", 2) == 0) {
        newEdge = fg0.top - (pct * monH) / 100;
        if (newEdge < wa.top)
            newEdge = wa.top;
        if (newEdge > fg0.bottom - MIN_DIM)
            newEdge = fg0.bottom - MIN_DIM;
        adj.bottomEdgeY = fg0.top; /* neighbour whose bottom == fg.top */
        EnumWindows(FindAdjacentProc, (LPARAM)&adj);
        newFg.top = newEdge;
        if (adj.found) {
            GetWindowRect(adj.found, &newAdj);
            int off = newAdj.bottom - fg0.top; /* typically +14 on Win11 */
            newAdj.bottom = newEdge + off;
            if (newAdj.bottom - newAdj.top < MIN_DIM) {
                newAdj.bottom = newAdj.top + MIN_DIM;
                newFg.top = newAdj.bottom - off;
            }
        }
    } else {
        return 1;
    }

    /* Apply moves with plain SetWindowPos, not DeferWindowPos. The previous
     * Defer-based version failed silently for the whole batch when the
     * second call (on adj) returned NULL — e.g. when EnumWindows happened
     * to match a window owned by a higher-integrity process or one whose
     * thread isn't pumping messages. With sequential SetWindowPos calls,
     * the fg move always lands; an adj failure only loses the adj move. */
    SetWindowPos(fg, NULL, newFg.left, newFg.top, newFg.right - newFg.left,
                 newFg.bottom - newFg.top, SWP_RESIZE);
    if (adj.found)
        SetWindowPos(adj.found, NULL, newAdj.left, newAdj.top,
                     newAdj.right - newAdj.left, newAdj.bottom - newAdj.top,
                     SWP_RESIZE);
    return 0;
}

/*
 * helm max
 *
 * Directly maximizes the foreground window, bypassing the Windows snap state
 * machine. Use this as a Win+Up replacement in Kanata when a window is snapped
 * - SW_MAXIMIZE skips the intermediate "unsnap to floating" step that Win+Up
 * goes through, so it maximizes in a single keystroke.
 */
static int ProcessMaxCommand(void) {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;
    if (IsIconic(fg))
        ShowWindow(fg, SW_RESTORE);
    ShowWindow(fg, SW_MAXIMIZE);
    return 0;
}

/*
 * helm swap[:left|right|up|down]
 *
 * With a direction: searches for a snapped neighbour in that direction.
 *   - Found: swaps the two window rects atomically.
 *   - Not found: snaps fg to the corresponding half of the work area.
 *
 * Without a direction (plain "swap"): original behaviour — searches both
 * horizontal sides and swaps with whichever neighbour is found first.
 */
static int ProcessSwapCommand(const wchar_t *arg) {
    /* arg is "" (undirected) or ":left"/":right"/":up"/":down" */
    const wchar_t *dir = (arg[0] == L':') ? arg + 1 : NULL;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi))
        return 1;
    const RECT wa = mi.rcWork;

    RECT fgRect;
    GetWindowRect(fg, &fgRect);

    AdjacentCtx adj = {.edgeX = -1,
                       .edgeY = -1,
                       .rightEdgeX = -1,
                       .bottomEdgeY = -1,
                       .tolerance = 24,
                       .skip = fg,
                       .found = NULL,
                       .refRect = fgRect};

    if (!dir) {
        adj.edgeX = fgRect.right;
        adj.rightEdgeX = fgRect.left;
    } else if (wcscmp(dir, L"right") == 0) {
        adj.edgeX = fgRect.right;
    } else if (wcscmp(dir, L"left") == 0) {
        adj.rightEdgeX = fgRect.left;
    } else if (wcscmp(dir, L"down") == 0) {
        adj.edgeY = fgRect.bottom;
    } else if (wcscmp(dir, L"up") == 0) {
        adj.bottomEdgeY = fgRect.top;
    } else {
        return 1;
    }

    EnumWindows(FindAdjacentProc, (LPARAM)&adj);

    if (!adj.found) {
        if (!dir)
            return 0;

        RECT snap = wa;
        const int halfW = (wa.right - wa.left) / 2;
        const int halfH = (wa.bottom - wa.top) / 2;
        if (wcscmp(dir, L"left") == 0)
            snap.right = wa.left + halfW;
        else if (wcscmp(dir, L"right") == 0)
            snap.left = wa.left + halfW;
        else if (wcscmp(dir, L"up") == 0)
            snap.bottom = wa.top + halfH;
        else if (wcscmp(dir, L"down") == 0)
            snap.top = wa.top + halfH;
        SetWindowPos(fg, NULL, snap.left, snap.top, snap.right - snap.left,
                     snap.bottom - snap.top, SWP_RESIZE);
        return 0;
    }

    RECT adjRect;
    GetWindowRect(adj.found, &adjRect);

    /* Plain SetWindowPos rather than DeferWindowPos: the latter is all-or-
     * nothing across both windows, and a single failing call (e.g. cross-
     * integrity adj) would silently drop the fg move too. See the same fix
     * in ProcessSzCommand. */
    SetWindowPos(fg, NULL, adjRect.left, adjRect.top,
                 adjRect.right - adjRect.left, adjRect.bottom - adjRect.top,
                 SWP_RESIZE);
    SetWindowPos(adj.found, NULL, fgRect.left, fgRect.top,
                 fgRect.right - fgRect.left, fgRect.bottom - fgRect.top,
                 SWP_RESIZE);
    return 0;
}

/* ============================================================
 * Top-level command dispatcher
 * ============================================================ */

static int ProcessCommand(const wchar_t *cmd, BOOL global) {
    if (wcsncmp(cmd, L"vd:", 3) == 0)
        return ProcessVdCommand(cmd + 3);
    if (wcsncmp(cmd, L"sz:", 3) == 0)
        return ProcessSzCommand(cmd + 3);
    if (wcscmp(cmd, L"max") == 0)
        return ProcessMaxCommand();
    if (wcsncmp(cmd, L"swap", 4) == 0 && (cmd[4] == L'\0' || cmd[4] == L':'))
        return ProcessSwapCommand(cmd + 4);
    const wchar_t *app = (wcsncmp(cmd, L"app:", 4) == 0) ? cmd + 4 : cmd;
    return ProcessAppCommand(app, global);
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

static void RunServer(void) {
    if (!AcquireDaemonMutex())
        return;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, READY_EVENT_NAME);

    CoCreateInstance(&CLSID_VDM, NULL, CLSCTX_ALL, &IID_IVDM, (void **)&Vdm);
    InitVdInternal();

    BOOL readySignaled = FALSE;
    while (1) {
        HANDLE pipe = CreateNamedPipeW(
            PIPE_NAME, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (Vdm)
                IVDM_Release(Vdm);
            if (VdmInternal)
                VdmInternal->lpVtbl->Release(VdmInternal);
            CloseHandle(ready);
            return;
        }
        if (!readySignaled) {
            SetEvent(ready);
            readySignaled = TRUE;
        }

        if (ConnectNamedPipe(pipe, NULL) ||
            GetLastError() == ERROR_PIPE_CONNECTED) {
            DWORD read;
            wchar_t buf[1024];
            if (ReadFile(pipe, buf, sizeof(buf) - 2, &read, NULL) && read > 0) {
                read &= ~1u;
                buf[read / sizeof(wchar_t)] = 0;
                wchar_t cmd[256] = {0};
                BOOL global = FALSE;
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
                    else if (!cmd[0])
                        StringCchCopyW(cmd, 256, start);
                    if (!saved)
                        break;
                    *p = saved;
                    p++;
                }
                if (cmd[0])
                    ProcessCommand(cmd, global);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

static BOOL TrySendToPipe(const wchar_t *cmd) {
    if (!WaitNamedPipeW(PIPE_NAME, 50))
        return FALSE;
    /* Retry on ERROR_PIPE_BUSY (all server instances in use) */
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
    /* The current process is helm.exe, so its full path is what we re-launch. */
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
    for (int i = 0; i < 2; i++) {
        if (TrySendToPipe(cmd))
            return TRUE;
        Sleep(80);
    }
    SpawnDaemon();
    HANDLE ev = NULL;
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
    wchar_t cmdBuf[512] = {0};
    for (int i = 1; i < argc; i++) {
        if (lstrcmpiW(argv[i], L"--server") == 0) {
            server = TRUE;
            continue;
        }
        if (cmdBuf[0])
            StringCchCatW(cmdBuf, 512, L" ");
        StringCchCatW(cmdBuf, 512, argv[i]);
    }
    if (server) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        RunServer();
        CoUninitialize();
        return 0;
    }
    if (!cmdBuf[0]) {
        fwprintf(stderr,
                 L"Usage: %s <command> [--global]\n"
                 L"  app:<exe>          focus or launch app\n"
                 L"  vd:<n>             switch to virtual desktop n\n"
                 L"  vd:send:<n>        move foreground window to desktop n\n"
                 L"  sz:left|right|down|up:+/-N  resize snapped window pair\n"
                 L"  max                maximize foreground window (Win+Up "
                 L"replacement)\n"
                 L"  swap[:left|right|up|down]  swap adjacent snapped window; "
                 L"snap fg to that half if none\n",
                 argv[0]);
        return 1;
    }
    return SendWithSpawn(cmdBuf) ? 0 : 1;
}
