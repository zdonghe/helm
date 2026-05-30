#include "helm.h"

#include <winternl.h>

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

static PidEntry PidCache[MAX_PIDS] __attribute__((section(".bss")));
static int PidCount = 0;
static ULONGLONG PidCacheExpiry = 0;
static DWORD ExplorerPid = 0;

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

static SRWLOCK PidLock = SRWLOCK_INIT;
static void *PidScratch = NULL;
static ULONG PidScratchCap = 0;

static void BuildPidCache(void) {
    long long t0 = StartMeasuring();
    PidCount = 0;
    ExplorerPid = 0;

    if (!PidScratch) {
        PidScratchCap = 384 * 1024;
        PidScratch = malloc(PidScratchCap);
        if (!PidScratch)
            return;
    }
    ULONG need = 0;
    NTSTATUS st;
    for (;;) {
        st = NtQuerySystemInformation(SystemProcessInformation, PidScratch,
                                      PidScratchCap, &need);
        if (st != STATUS_INFO_LENGTH_MISMATCH)
            break;
        ULONG newcap = need + 64 * 1024;
        void *nb = realloc(PidScratch, newcap);
        if (!nb)
            return;
        PidScratch = nb;
        PidScratchCap = newcap;
    }
    if (st < 0)
        return;

    BYTE *p = (BYTE *)PidScratch;
    for (;;) {
        SYSTEM_PROCESS_INFORMATION *spi = (SYSTEM_PROCESS_INFORMATION *)p;
        if (PidCount < MAX_PIDS) {
            DWORD pid = (DWORD)(ULONG_PTR)spi->UniqueProcessId;
            wchar_t *name = spi->ImageName.Buffer;
            USHORT n = spi->ImageName.Length / sizeof(wchar_t);
            PidCache[PidCount].pid = pid;
            if (name && n) {
                if (n >= EXE_NAME_MAX)
                    n = EXE_NAME_MAX - 1;
                memcpy(PidCache[PidCount].exe, name, n * sizeof(wchar_t));
                PidCache[PidCount].exe[n] = L'\0';
                if (!ExplorerPid &&
                    lstrcmpiW(PidCache[PidCount].exe, L"explorer.exe") == 0)
                    ExplorerPid = pid;
            } else {
                /* System Idle Process has a NULL ImageName.Buffer. */
                PidCache[PidCount].exe[0] = L'\0';
            }
            PidCount++;
        }
        if (!spi->NextEntryOffset)
            break;
        p += spi->NextEntryOffset;
    }
    qsort(PidCache, PidCount, sizeof(PidEntry), CmpPid);
    Log(LOG_PERF, L"pid-cache: %d procs %.2f ms", PidCount,
        FinishMeasuring(t0));
}

DWORD GetExplorerPid(void) { return ExplorerPid; }

void MaybeRebuildPidCache(void) {
    if (GetTickCount64() < PidCacheExpiry)
        return;
    AcquireSRWLockExclusive(&PidLock);
    if (GetTickCount64() >= PidCacheExpiry) {
        BuildPidCache();
        PidCacheExpiry = GetTickCount64() + PIDCACHE_TTL_MS;
    }
    ReleaseSRWLockExclusive(&PidLock);
}

void ForceRebuildPidCache(void) {
    AcquireSRWLockExclusive(&PidLock);
    BuildPidCache();
    PidCacheExpiry = GetTickCount64() + PIDCACHE_TTL_MS;
    ReleaseSRWLockExclusive(&PidLock);
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
            Log(LOG_TRACE, L"evict: invalid/hidden");
            goto evict;
        }
        const wchar_t *owner = GetExeFromPid(pid);
        if (!owner || lstrcmpiW(owner, exe) != 0) {
            Log(LOG_TRACE, L"evict: owner-mismatch");
            goto evict;
        }
        int cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) {
            Log(LOG_TRACE, L"evict: cloaked");
            goto evict;
        }

        long long tZ = StartMeasuring();
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
            Log(LOG_PERF, L"z-order walk: %.2f ms (evict)",
                FinishMeasuring(tZ));
            Log(LOG_TRACE, L"evict: same-exe ABOVE in z-order");
            goto evict;
        }
        Log(LOG_PERF, L"z-order walk: %.2f ms", FinishMeasuring(tZ));
        Log(LOG_TRACE, L"cache-valid");
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
