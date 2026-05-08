#include "helm.h"

/* Assigns newEdge to newFg.fg_field, finds the adjacent window on
 * adj.edge_field == edge_val, and pushes it symmetrically.  adj_w must be
 * the adjacent window's size along this axis (right-left or bottom-top) so
 * the MIN_DIM guard is evaluated correctly regardless of direction. */
#define SZ_EDGE(near, edge_field, edge_val, fg_field, min_adj, adj_w)          \
    adj.edge_field = (edge_val);                                               \
    EnumWindows(FindAdjacentProc, (LPARAM) & adj);                             \
    newFg.fg_field = newEdge;                                                  \
    if (adj.found) {                                                           \
        GetWindowRect(adj.found, &newAdj);                                     \
        int off = newAdj.near - (edge_val);                                    \
        newAdj.near = newEdge + off;                                           \
        if ((adj_w) < MIN_DIM) {                                               \
            newAdj.near = (min_adj);                                           \
            newFg.fg_field = newAdj.near - off;                                \
        }                                                                      \
    }

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
    if (IsZoomed(fg)) {
        RECT r;
        GetWindowRect(fg, &r);
        SetWindowLongW(fg, GWL_STYLE,
                       GetWindowLongW(fg, GWL_STYLE) & ~WS_MAXIMIZE);
        SetWindowPos(fg, NULL, r.left, r.top, r.right - r.left,
                     r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi))
        return 1;
    const RECT wa = mi.rcWork;
    const int monW = wa.right - wa.left;
    const int monH = wa.bottom - wa.top;

    RECT fg0;
    GetWindowRect(fg, &fg0);

    /* Redirect to the opposite edge only if the window is already touching the
     * monitor boundary (e.g. sz:left:10 on a left-snapped window shrinks from
     * the right).  If the move would merely overshoot, clamp to the boundary
     * instead - handled per-direction below. */
    const wchar_t *dir = arg;
    if (wcsncmp(arg, L"left", 4) == 0 && fg0.left <= wa.left &&
        fg0.left - (pct * monW) / 100 < wa.left) {
        dir = L"right";
        pct = -pct;
    } else if (wcsncmp(arg, L"right", 5) == 0 && fg0.right >= wa.right &&
               fg0.right + (pct * monW) / 100 > wa.right) {
        dir = L"left";
        pct = -pct;
    } else if (wcsncmp(arg, L"up", 2) == 0 && fg0.top <= wa.top &&
               fg0.top - (pct * monH) / 100 < wa.top) {
        dir = L"down";
        pct = -pct;
    } else if (wcsncmp(arg, L"down", 4) == 0 && fg0.bottom >= wa.bottom &&
               fg0.bottom + (pct * monH) / 100 > wa.bottom) {
        dir = L"up";
        pct = -pct;
    }

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

    if (wcsncmp(dir, L"right", 5) == 0) {
        newEdge = fg0.right + (pct * monW) / 100;
        if (newEdge > wa.right)
            newEdge = wa.right;
        if (newEdge < fg0.left + MIN_DIM)
            newEdge = fg0.left + MIN_DIM;
        SZ_EDGE(left, edgeX, fg0.right, right, newAdj.right - MIN_DIM,
                newAdj.right - newAdj.left);
    } else if (wcsncmp(dir, L"left", 4) == 0) {
        newEdge = fg0.left - (pct * monW) / 100;
        if (newEdge < wa.left)
            newEdge = wa.left;
        if (newEdge > fg0.right - MIN_DIM)
            newEdge = fg0.right - MIN_DIM;
        SZ_EDGE(right, rightEdgeX, fg0.left, left, newAdj.left + MIN_DIM,
                newAdj.right - newAdj.left);
    } else if (wcsncmp(dir, L"down", 4) == 0) {
        newEdge = fg0.bottom + (pct * monH) / 100;
        if (newEdge > wa.bottom)
            newEdge = wa.bottom;
        if (newEdge < fg0.top + MIN_DIM)
            newEdge = fg0.top + MIN_DIM;
        SZ_EDGE(top, edgeY, fg0.bottom, bottom, newAdj.bottom - MIN_DIM,
                newAdj.bottom - newAdj.top);
    } else if (wcsncmp(dir, L"up", 2) == 0) {
        newEdge = fg0.top - (pct * monH) / 100;
        if (newEdge < wa.top)
            newEdge = wa.top;
        if (newEdge > fg0.bottom - MIN_DIM)
            newEdge = fg0.bottom - MIN_DIM;
        SZ_EDGE(bottom, bottomEdgeY, fg0.top, top, newAdj.top + MIN_DIM,
                newAdj.bottom - newAdj.top);
    } else {
        return 1;
    }

    SetWindowPos(fg, NULL, newFg.left, newFg.top, newFg.right - newFg.left,
                 newFg.bottom - newFg.top, SWP_RESIZE);
    if (adj.found)
        SetWindowPos(adj.found, NULL, newAdj.left, newAdj.top,
                     newAdj.right - newAdj.left, newAdj.bottom - newAdj.top,
                     SWP_RESIZE);
    return 0;
}
