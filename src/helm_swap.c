#include "helm.h"

static BOOL IsSnappedTo(HWND fg, const wchar_t *dir) {
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    RECT r;
    if (!GetMonitorInfoW(mon, &mi) || !GetWindowRect(fg, &r))
        return FALSE;
    const RECT *wa = &mi.rcWork;
    int midX = (wa->left + wa->right) / 2;
    int midY = (wa->top + wa->bottom) / 2;
    const int tol = 24;
    if (wcscmp(dir, L"left") == 0)
        return abs(r.left - wa->left) < tol && abs(r.right - midX) < tol;
    if (wcscmp(dir, L"right") == 0)
        return abs(r.right - wa->right) < tol && abs(r.left - midX) < tol;
    if (wcscmp(dir, L"up") == 0)
        return abs(r.top - wa->top) < tol && abs(r.bottom - midY) < tol;
    if (wcscmp(dir, L"down") == 0)
        return abs(r.bottom - wa->bottom) < tol && abs(r.top - midY) < tol;
    return FALSE;
}

/*
 * Inject Win+dir to trigger Windows' own snap.
 * If fg is snapped to the opposite half, send the sequence twice:
 * first press unsnaps, second snaps to target. Both are batched into one
 * SendInput so they serialize without a sleep.
 */
static void NativeSnap(HWND fg, const wchar_t *dir) {
    BOOL useAlt = (wcscmp(dir, L"up") == 0 || wcscmp(dir, L"down") == 0);
    WORD vk = wcscmp(dir, L"left") == 0    ? VK_LEFT
              : wcscmp(dir, L"right") == 0 ? VK_RIGHT
              : wcscmp(dir, L"up") == 0    ? VK_UP
                                           : VK_DOWN;
    const wchar_t *opp = wcscmp(dir, L"left") == 0    ? L"right"
                         : wcscmp(dir, L"right") == 0 ? L"left"
                         : wcscmp(dir, L"up") == 0    ? L"down"
                                                      : L"up";

    int repeat = IsSnappedTo(fg, opp) ? 2 : 1;
    INPUT inp[20];
    int n = 0;

    for (int r = 0; r < repeat; r++) {
        inp[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = VK_LWIN}};
        if (useAlt)
            inp[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = VK_MENU}};
        inp[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = vk}};
        inp[n++] = (INPUT){.type = INPUT_KEYBOARD,
                           .ki = {.wVk = vk, .dwFlags = KEYEVENTF_KEYUP}};
        if (useAlt)
            inp[n++] =
                (INPUT){.type = INPUT_KEYBOARD,
                        .ki = {.wVk = VK_MENU, .dwFlags = KEYEVENTF_KEYUP}};
        inp[n++] = (INPUT){.type = INPUT_KEYBOARD,
                           .ki = {.wVk = VK_LWIN, .dwFlags = KEYEVENTF_KEYUP}};
    }
    SendInput(n, inp, sizeof(INPUT));
}

/*
 * helm swap[:left|right|up|down]
 *
 * With direction: find snapped neighbour in that direction.
 *   Found  → swap positions
 *   None   → NativeSnap fg to that half
 * Without direction: search both horizontal sides, swap with first found.
 */
int ProcessSwapCommand(const wchar_t *arg) {
    const wchar_t *dir = (arg[0] == L':') ? arg + 1 : NULL;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    if (fg == GetShellWindow() || fg == GetDesktopWindow())
        return 0;
    if (dir && IsSnappedTo(fg, dir))
        return 0;

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
        NativeSnap(fg, dir);
        return 0;
    }

    RECT adjRect;
    GetWindowRect(adj.found, &adjRect);

    SetWindowPos(fg, NULL, adjRect.left, adjRect.top,
                 adjRect.right - adjRect.left, adjRect.bottom - adjRect.top,
                 SWP_RESIZE);
    SetWindowPos(adj.found, NULL, fgRect.left, fgRect.top,
                 fgRect.right - fgRect.left, fgRect.bottom - fgRect.top,
                 SWP_RESIZE);
    return 0;
}
