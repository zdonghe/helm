#include "helm.h"

static PidEntry PidCache[MAX_PIDS];
static int PidCount = 0;
static ULONGLONG PidCacheExpiry = 0;

static struct {
    wchar_t exe[EXE_NAME_MAX];
    HWND hwnd;
} HwndCache[HWND_CACHE_SIZE];
static int HwndCacheCount = 0;
static int HwndCacheNext = 0;

static int CmpPid(const void *a, const void *b) {
    DWORD pa = ((const PidEntry *)a)->pid;
    DWORD pb = ((const PidEntry *)b)->pid;
    return (pa > pb) - (pa < pb);
}

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

void MaybeRebuildPidCache(void) {
    ULONGLONG now = GetTickCount64();
    if (now < PidCacheExpiry)
        return;
    PidCount = 0;
    BuildPidCache();
    PidCacheExpiry = now + PIDCACHE_TTL_MS;
}

void ForceRebuildPidCache(void) {
    PidCacheExpiry = 0;
    MaybeRebuildPidCache();
}

const wchar_t *GetExeFromPid(DWORD pid) {
    PidEntry key = {.pid = pid};
    PidEntry *found =
        (PidEntry *)bsearch(&key, PidCache, PidCount, sizeof(PidEntry), CmpPid);
    return found ? found->exe : NULL;
}

HWND LookupHwndCache(const wchar_t *exe, const wchar_t *cls) {
    for (int i = 0; i < HwndCacheCount; i++) {
        if (lstrcmpiW(HwndCache[i].exe, exe) != 0)
            continue;

        /* Validate exe ownership: Windows recycles HWND values. */
        HWND h = HwndCache[i].hwnd;
        DWORD pid = 0;
        if (!h || !IsWindow(h) || !IsWindowVisible(h) ||
            !GetWindowThreadProcessId(h, &pid)) {
            OutputDebugStringW(L"[helm] evict: invalid/hidden\n");
            goto evict;
        }
        const wchar_t *owner = GetExeFromPid(pid);
        if (!owner || lstrcmpiW(owner, exe) != 0) {
            OutputDebugStringW(L"[helm] evict: owner-mismatch\n");
            goto evict;
        }
        int cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) {
            OutputDebugStringW(L"[helm] evict: cloaked\n");
            goto evict;
        }

        for (HWND w = GetWindow(h, GW_HWNDPREV); w;
             w = GetWindow(w, GW_HWNDPREV)) {
            if (!IsWindowVisible(w))
                continue;
            LONG_PTR wex = GetWindowLongPtrW(w, GWL_EXSTYLE);
            if (wex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
                continue;
            if (cls) {
                wchar_t wcls[256];
                GetClassNameW(w, wcls, 256);
                if (lstrcmpiW(wcls, cls) != 0)
                    continue;
            }
            DWORD wpid = 0;
            GetWindowThreadProcessId(w, &wpid);
            const wchar_t *wexe = GetExeFromPid(wpid);
            if (!wexe || lstrcmpiW(wexe, exe) != 0)
                continue;
            int wCloaked = 0;
            DwmGetWindowAttribute(w, DWMWA_CLOAKED, &wCloaked,
                                  sizeof(wCloaked));
            if (wCloaked)
                continue;
            BOOL onCurrent = FALSE;
            if (Vdm && SUCCEEDED(IVDM_IsCurrent(Vdm, w, &onCurrent)) &&
                !onCurrent)
                continue;
            OutputDebugStringW(L"[helm] evict: same-exe ABOVE in z-order\n");
            goto evict;
        }
        OutputDebugStringW(L"[helm] cache-valid\n");
        return h;

    evict:
        HwndCache[i] = HwndCache[--HwndCacheCount];
        return NULL;
    }
    return NULL;
}

void StoreHwndCache(const wchar_t *exe, HWND hwnd) {
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

DWORD GetPidFromExe(const wchar_t *exe) {
    for (int i = 0; i < PidCount; i++) {
        if (lstrcmpiW(PidCache[i].exe, exe) == 0)
            return PidCache[i].pid;
    }
    return 0;
}
