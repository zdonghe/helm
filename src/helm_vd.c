#include "helm.h"

/* ============================================================
 * Desktop array cache
 * ============================================================ */

static IObjectArray *sVdArr = NULL;
static UINT sVdCount = 0;

void VdCacheInvalidate(void) {
    if (sVdArr) {
        sVdArr->lpVtbl->Release(sVdArr);
        sVdArr = NULL;
    }
    sVdCount = 0;
}

/* ============================================================
 * IVirtualDesktopNotification
 *
 * Tells helm when some event involving virutal desktops occur
 * ============================================================ */

static volatile LONG g_vdDirty = 0;

static HRESULT STDMETHODCALLTYPE OnCreated(IVdNotification *This,
                                           IVirtualDesktop *p) {
    (void)This;
    (void)p;
    InterlockedExchange(&g_vdDirty, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnDestroyBegin(IVdNotification *This,
                                                IVirtualDesktop *a,
                                                IVirtualDesktop *b) {
    (void)This;
    (void)a;
    (void)b;
    InterlockedExchange(&g_vdDirty, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnDestroyFailed(IVdNotification *This,
                                                 IVirtualDesktop *a,
                                                 IVirtualDesktop *b) {
    (void)This;
    (void)a;
    (void)b;
    InterlockedExchange(&g_vdDirty, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnDestroyed(IVdNotification *This,
                                             IVirtualDesktop *a,
                                             IVirtualDesktop *b) {
    (void)This;
    (void)a;
    (void)b;
    InterlockedExchange(&g_vdDirty, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnMoved(IVdNotification *This,
                                         IVirtualDesktop *p, UINT from,
                                         UINT to) {
    (void)This;
    (void)p;
    (void)from;
    (void)to;
    InterlockedExchange(&g_vdDirty, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnNameChanged(IVdNotification *This,
                                               IVirtualDesktop *p, void *name) {
    (void)This;
    (void)p;
    (void)name;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnViewChanged(IVdNotification *This, void *p) {
    (void)This;
    (void)p;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnCurrentChanged(IVdNotification *This,
                                                  IVirtualDesktop *a,
                                                  IVirtualDesktop *b) {
    (void)This;
    (void)a;
    (void)b;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnWallpaperChanged(IVdNotification *This,
                                                    IVirtualDesktop *p,
                                                    void *path) {
    (void)This;
    (void)p;
    (void)path;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnSwitched(IVdNotification *This,
                                            IVirtualDesktop *p, int t) {
    (void)This;
    (void)p;
    (void)t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OnRemoteConnected(IVdNotification *This,
                                                   IVirtualDesktop *p) {
    (void)This;
    (void)p;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE VdNotif_QI(IVdNotification *This, REFIID riid,
                                            void **ppv) {
    (void)This;
    static const IID iid_iunk = {
        0x00000000,
        0x0000,
        0x0000,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    if (IsEqualIID(riid, &iid_iunk) || IsEqualIID(riid, &IID_IVdNotification)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE VdNotif_AddRef(IVdNotification *This) {
    (void)This;
    return 1;
}

static ULONG STDMETHODCALLTYPE VdNotif_Release(IVdNotification *This) {
    (void)This;
    return 1;
}

static const IVdNotificationVtbl sVdNotifVtbl = {
    VdNotif_QI,     VdNotif_AddRef,    VdNotif_Release,  OnCreated,
    OnDestroyBegin, OnDestroyFailed,   OnDestroyed,      OnMoved,
    OnNameChanged,  OnViewChanged,     OnCurrentChanged, OnWallpaperChanged,
    OnSwitched,     OnRemoteConnected,
};

static IVdNotification sVdNotif = {&sVdNotifVtbl};
static IVdNotifSvc *sVdNotifSvc = NULL;
static DWORD sVdNotifCookie = 0;

/* ============================================================
 * VdNotifyInit / VdNotifyShutdown
 * ============================================================ */

void VdNotifyInit(void) {
    ISvcProv *sp = NULL;
    HRESULT hr =
        CoCreateInstance(&CLSID_ImmersiveShell, NULL, CLSCTX_LOCAL_SERVER,
                         &IID_ISvcProv, (void **)&sp);
    if (FAILED(hr))
        return;
    hr = sp->lpVtbl->QueryService(sp, &SID_VdNotifSvc, &IID_IVdNotifSvc,
                                  (void **)&sVdNotifSvc);
    sp->lpVtbl->Release(sp);
    if (FAILED(hr))
        return;
    hr = sVdNotifSvc->lpVtbl->Register(sVdNotifSvc, &sVdNotif, &sVdNotifCookie);
    if (FAILED(hr)) {
        sVdNotifSvc->lpVtbl->Release(sVdNotifSvc);
        sVdNotifSvc = NULL;
        return;
    }
    Log(LOG_TRACE, L"VdNotify: registered, cookie=%lu", sVdNotifCookie);
}

void VdNotifyShutdown(void) {
    if (sVdNotifSvc) {
        if (sVdNotifCookie)
            sVdNotifSvc->lpVtbl->Unregister(sVdNotifSvc, sVdNotifCookie);
        sVdNotifSvc->lpVtbl->Release(sVdNotifSvc);
        sVdNotifSvc = NULL;
    }
    sVdNotifCookie = 0;
}

void InitVdInternal(void) {
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

/* ============================================================
 * GetDesktopN - return IVirtualDesktop* at 0-based index n
 * ============================================================ */

static IVirtualDesktop *GetDesktopN(int n) {
    if (!VdmInternal)
        return NULL;

    if (InterlockedExchange(&g_vdDirty, 0)) {
        Log(LOG_PERF, L"VdCache: dirty flag set, rebuilding");
        VdCacheInvalidate();
    }

    long long t0 = StartMeasuring();

    if (!sVdArr) {
        IObjectArray *arr = NULL;
        HRESULT hrGet = VdmInternal->lpVtbl->GetDesktops(VdmInternal, &arr);
        if (FAILED(hrGet)) {
            Log(LOG_TRACE, L"GetDesktops failed hr=0x%lX, reinit", hrGet);
            IVDMI_Release(VdmInternal);
            VdmInternal = NULL;
            VdNotifyShutdown();
            InitVdInternal();
            if (VdmInternal) {
                VdNotifyInit();
                hrGet = VdmInternal->lpVtbl->GetDesktops(VdmInternal, &arr);
            }
            if (FAILED(hrGet) || !VdmInternal)
                return NULL;
        }
        Log(LOG_PERF, L"GetDesktops: %.2f ms", FinishMeasuring(t0));
        arr->lpVtbl->GetCount(arr, &sVdCount);
        sVdArr = arr;
    } else {
        Log(LOG_PERF, L"VdCache hit: count=%u", sVdCount);
    }

    if ((UINT)n >= sVdCount)
        return NULL;

    IVirtualDesktop *desk = NULL;
    HRESULT hrGet = sVdArr->lpVtbl->GetAt(sVdArr, (UINT)n, &IID_IVirtualDesktop,
                                          (void **)&desk);
    if (FAILED(hrGet)) {
        Log(LOG_TRACE, L"GetAt(%d) failed hr=0x%lX, invalidating", n, hrGet);
        VdCacheInvalidate();
        return NULL;
    }
    return desk;
}

/* ============================================================
 * ProcessVdCommand
 *
 * vd:N        - switch to desktop N (1-indexed)
 * vd:send:N   - move foreground window to desktop N then switch
 * ============================================================ */

int ProcessVdCommand(const wchar_t *arg) {
    long long tTotal = StartMeasuring();
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

    HRESULT hr;
    if (send) {
        long long tS = StartMeasuring();
        hr = VdmInternal->lpVtbl->SwitchDesktopAndMoveForegroundView(
            VdmInternal, desk);
        Log(LOG_PERF, L"SwitchDesktopAndMoveForegroundView: %.2f ms",
            FinishMeasuring(tS));
    } else {
        long long tD = StartMeasuring();
        BypassForegroundLock();
        SetForegroundWindow(GetShellWindow());
        Log(LOG_PERF, L"defocus: %.2f ms", FinishMeasuring(tD));
        long long tS = StartMeasuring();
        hr = VdmInternal->lpVtbl->SwitchDesktop(VdmInternal, desk);
        Log(LOG_PERF, L"SwitchDesktop: %.2f ms", FinishMeasuring(tS));
    }
    desk->lpVtbl->Release(desk);

    if (FAILED(hr)) {
        Log(LOG_PERF, L"SwitchDesktop failed hr=0x%lX, invalidating cache", hr);
        VdCacheInvalidate();
    }

    Log(LOG_PERF, L"ProcessVdCommand total: %.2f ms", FinishMeasuring(tTotal));
    return SUCCEEDED(hr) ? 0 : 1;
}
