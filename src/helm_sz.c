#include "helm.h"

BOOL CALLBACK FindAdjacentProc(HWND hwnd, LPARAM lp) {
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

int ProcessSzCommand(const wchar_t *arg) {
    const wchar_t *colon = wcschr(arg, L':');
    if (!colon)
        return 1;
    int pct = _wtoi(colon + 1);
    if (pct == 0)
        return 0;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;
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
        adj.edgeX = fg0.right;
        EnumWindows(FindAdjacentProc, (LPARAM)&adj);
        newFg.right = newEdge;
        if (adj.found) {
            GetWindowRect(adj.found, &newAdj);
            int off = newAdj.left - fg0.right; /* typically -14 on Win11 */
            newAdj.left = newEdge + off;
            if (newAdj.right - newAdj.left < MIN_DIM) {
                newAdj.left = newAdj.right - MIN_DIM;
                newFg.right = newAdj.left - off;
            }
        }
    } else if (wcsncmp(arg, L"left", 4) == 0) {
        newEdge = fg0.left - (pct * monW) / 100;
        if (newEdge < wa.left)
            newEdge = wa.left;
        if (newEdge > fg0.right - MIN_DIM)
            newEdge = fg0.right - MIN_DIM;
        adj.rightEdgeX = fg0.left;
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
        adj.edgeY = fg0.bottom;
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
        adj.bottomEdgeY = fg0.top;
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

    /* Sequential SetWindowPos: fg move always lands; adj failure loses only
     * adj. */
    SetWindowPos(fg, NULL, newFg.left, newFg.top, newFg.right - newFg.left,
                 newFg.bottom - newFg.top, SWP_RESIZE);
    if (adj.found)
        SetWindowPos(adj.found, NULL, newAdj.left, newAdj.top,
                     newAdj.right - newAdj.left, newAdj.bottom - newAdj.top,
                     SWP_RESIZE);
    return 0;
}
