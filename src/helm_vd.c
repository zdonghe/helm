#include "helm.h"

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

/* Returns desktop at 0-based index n; caller must Release. */
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
 * vd:N        — switch to desktop N (1-indexed)
 * vd:send:N   — move foreground window to desktop N then switch
 */
int ProcessVdCommand(const wchar_t *arg) {
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
        /* Defocus before switching so apps like Firefox/Discord handle
         * WM_ACTIVATE(INACTIVE) cleanly without flashing on the new desktop. */
        BypassForegroundLock();
        SetForegroundWindow(GetShellWindow());
        VdmInternal->lpVtbl->SwitchDesktop(VdmInternal, desk);
    }
    desk->lpVtbl->Release(desk);
    return 0;
}
