#include "helm.h"

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

    enum Dir { DIR_CYCLE, DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN } d;
    if (wcscmp(dir, L"cycle") == 0)
        d = DIR_CYCLE;
    else if (wcscmp(dir, L"left") == 0)
        d = DIR_LEFT;
    else if (wcscmp(dir, L"right") == 0)
        d = DIR_RIGHT;
    else if (wcscmp(dir, L"up") == 0)
        d = DIR_UP;
    else if (wcscmp(dir, L"down") == 0)
        d = DIR_DOWN;
    else
        return 1;

    MonList list = {0};
    EnumDisplayMonitors(NULL, NULL, EnumMonProc, (LPARAM)&list);
    if (list.count <= 1)
        return 0;

    POINT cpt;
    GetCursorPos(&cpt);
    HMONITOR cur = MonitorFromPoint(cpt, MONITOR_DEFAULTTONEAREST);
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
    if (d == DIR_CYCLE) {
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
            if (d == DIR_LEFT)
                match = (dx < 0) &&
                        (abs(dy) < (abs(dx) + 1) / 2 +
                                    (curRc.bottom - curRc.top) / 4);
            else if (d == DIR_RIGHT)
                match = (dx > 0) &&
                        (abs(dy) < (abs(dx) + 1) / 2 +
                                    (curRc.bottom - curRc.top) / 4);
            else if (d == DIR_UP)
                match = (dy < 0) &&
                        (abs(dx) < (abs(dy) + 1) / 2 +
                                    (curRc.right - curRc.left) / 4);
            else if (d == DIR_DOWN)
                match = (dy > 0) &&
                        (abs(dx) < (abs(dy) + 1) / 2 +
                                    (curRc.right - curRc.left) / 4);

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
    if (h && h != GetDesktopWindow() && h != GetShellWindow()) {
        BypassForegroundLock();
        SetForegroundWindow(h);
    }
    return 0;
}
