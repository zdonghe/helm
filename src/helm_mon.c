#include "helm.h"

/*
 * Monitor-switching command.
 *
 *   mon:left|right|up|down   move cursor (and focus) to the nearest monitor
 *                            in the given direction
 *   mon:cycle                cycle to the next monitor in enumeration order
 *
 * Spatial matching uses a directional cone: the target must be in the correct
 * half-plane (e.g. dx < 0 for "left") and within a tolerance proportional to
 * the perpendicular distance, plus a quarter of the current monitor's extent
 * along the perpendicular axis.  Ties are broken by Euclidean distance.
 */

#define MAX_MONITORS 16

typedef struct {
    RECT rc;
    HMONITOR hmon;
} MonInfo;

typedef struct {
    MonInfo mons[MAX_MONITORS];
    int count;
} MonList;

static BOOL CALLBACK EnumMonProc(HMONITOR hmon, HDC hdc, LPRECT lprc,
                                 LPARAM lp) {
    MonList *list = (MonList *)lp;
    if (list->count >= MAX_MONITORS)
        return FALSE;
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (GetMonitorInfoW(hmon, &mi)) {
        list->mons[list->count].rc = mi.rcWork;
        list->mons[list->count].hmon = hmon;
        list->count++;
    }
    return TRUE;
}

static int CenterX(const RECT *r) { return (r->left + r->right) / 2; }
static int CenterY(const RECT *r) { return (r->top + r->bottom) / 2; }

int ProcessMonCommand(const wchar_t *arg) {
    if (!arg || !*arg || *arg != L':')
        return 1;
    const wchar_t *dir = arg + 1;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;
    if (fg == GetShellWindow() || fg == GetDesktopWindow())
        return 0;

    MonList list = {0};
    EnumDisplayMonitors(NULL, NULL, EnumMonProc, (LPARAM)&list);
    if (list.count <= 1)
        return 0;

    HMONITOR cur = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    int ci = -1;
    for (int i = 0; i < list.count; i++) {
        if (list.mons[i].hmon == cur) {
            ci = i;
            break;
        }
    }
    if (ci < 0)
        return 1;

    int ti;
    if (wcscmp(dir, L"cycle") == 0) {
        ti = (ci + 1) % list.count;
    } else {
        RECT curRc = list.mons[ci].rc;
        int cx = CenterX(&curRc);
        int cy = CenterY(&curRc);
        int best = -1;
        long long bestDist = LLONG_MAX;

        for (int i = 0; i < list.count; i++) {
            if (i == ci)
                continue;
            const RECT *r = &list.mons[i].rc;
            int dx = CenterX(r) - cx;
            int dy = CenterY(r) - cy;

            BOOL match = FALSE;
            if (wcscmp(dir, L"left") == 0)
                match = (dx < 0) &&
                        (abs(dy) < (abs(dx) + 1) / 2 +
                                    (curRc.bottom - curRc.top) / 4);
            else if (wcscmp(dir, L"right") == 0)
                match = (dx > 0) &&
                        (abs(dy) < (abs(dx) + 1) / 2 +
                                    (curRc.bottom - curRc.top) / 4);
            else if (wcscmp(dir, L"up") == 0)
                match = (dy < 0) &&
                        (abs(dx) < (abs(dy) + 1) / 2 +
                                    (curRc.right - curRc.left) / 4);
            else if (wcscmp(dir, L"down") == 0)
                match = (dy > 0) &&
                        (abs(dx) < (abs(dy) + 1) / 2 +
                                    (curRc.right - curRc.left) / 4);
            else
                return 1;

            if (match) {
                long long dist = (long long)dx * dx + (long long)dy * dy;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = i;
                }
            }
        }

        if (best < 0)
            return 0;
        ti = best;
    }

    const RECT *tgt = &list.mons[ti].rc;
    int tx = CenterX(tgt);
    int ty = CenterY(tgt);
    SetCursorPos(tx, ty);

    POINT pt = {tx, ty};
    HWND h = WindowFromPoint(pt);
    if (h)
        h = GetAncestor(h, GA_ROOT);
    if (h && IsIconic(h))
        ShowWindow(h, SW_RESTORE);
    if (h) {
        BypassForegroundLock();
        if (!SetForegroundWindow(h))
            BringWindowToTop(h);
    }
    return 0;
}
