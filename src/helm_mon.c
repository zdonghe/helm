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

enum Dir { DIR_INVALID = -1, DIR_CYCLE, DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN };

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

static MonList EnumMonitors(void) {
    MonList list = {0};
    EnumDisplayMonitors(NULL, NULL, EnumMonProc, (LPARAM)&list);
    return list;
}

static int FindCurrentMonIndex(MonList *list, HMONITOR cur) {
    for (int i = 0; i < list->count; i++) {
        if (list->mons[i].hmon == cur)
            return i;
    }
    return -1;
}

static int FindTargetMonIndex(MonList *list, int ci, enum Dir d) {
    if (d == DIR_CYCLE)
        return (ci + 1) % list->count;

    RECT curRc = list->mons[ci].rc;
    int cx = CenterX(&curRc);
    int cy = CenterY(&curRc);
    int best = -1;
    long long bestDist = LLONG_MAX;

    for (int i = 0; i < list->count; i++) {
        if (i == ci)
            continue;
        const RECT *r = &list->mons[i].rc;
        int dx = CenterX(r) - cx;
        int dy = CenterY(r) - cy;

        BOOL match = FALSE;
        if (d == DIR_LEFT)
            match = (dx < 0) && (abs(dy) < (abs(dx) + 1) / 2 +
                                               (curRc.bottom - curRc.top) / 4);
        else if (d == DIR_RIGHT)
            match = (dx > 0) && (abs(dy) < (abs(dx) + 1) / 2 +
                                               (curRc.bottom - curRc.top) / 4);
        else if (d == DIR_UP)
            match = (dy < 0) && (abs(dx) < (abs(dy) + 1) / 2 +
                                               (curRc.right - curRc.left) / 4);
        else if (d == DIR_DOWN)
            match = (dy > 0) && (abs(dx) < (abs(dy) + 1) / 2 +
                                               (curRc.right - curRc.left) / 4);

        if (match) {
            long long dist = (long long)dx * dx + (long long)dy * dy;
            if (dist < bestDist) {
                bestDist = dist;
                best = i;
            }
        }
    }

    return best;
}

static enum Dir ParseDir(const wchar_t *s) {
    if (wcscmp(s, L"cycle") == 0)
        return DIR_CYCLE;
    if (wcscmp(s, L"left") == 0)
        return DIR_LEFT;
    if (wcscmp(s, L"right") == 0)
        return DIR_RIGHT;
    if (wcscmp(s, L"up") == 0)
        return DIR_UP;
    if (wcscmp(s, L"down") == 0)
        return DIR_DOWN;
    return DIR_INVALID;
}

static void GotoMonitor(const RECT *tgt) {
    int tx = CenterX(tgt);
    int ty = CenterY(tgt);
    SetCursorPos(tx, ty);
    HWND h = WindowFromPoint((POINT){tx, ty});
    if (h)
        h = GetAncestor(h, GA_ROOT);
    if (h && h != GetDesktopWindow() && h != GetShellWindow()) {
        BypassForegroundLock();
        SetForegroundWindow(h);
    }
}

static void SendToMonitor(const RECT *tgt, enum Dir d) {
    WORD vk = d == DIR_LEFT    ? VK_LEFT
              : d == DIR_RIGHT ? VK_RIGHT
              : d == DIR_UP    ? VK_UP
              : d == DIR_DOWN  ? VK_DOWN
                               : VK_RIGHT;

    INPUT release[] = {
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_MENU, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_LMENU, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_RMENU, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_CONTROL, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_LCONTROL, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_RCONTROL, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_SHIFT, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_LSHIFT, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_RSHIFT, .dwFlags = KEYEVENTF_KEYUP}},
    };
    SendInput(9, release, sizeof(INPUT));

    INPUT inp[6] = {
        {.type = INPUT_KEYBOARD, .ki = {.wVk = VK_LWIN}},
        {.type = INPUT_KEYBOARD, .ki = {.wVk = VK_SHIFT}},
        {.type = INPUT_KEYBOARD, .ki = {.wVk = vk}},
        {.type = INPUT_KEYBOARD, .ki = {.wVk = vk, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_SHIFT, .dwFlags = KEYEVENTF_KEYUP}},
        {.type = INPUT_KEYBOARD,
         .ki = {.wVk = VK_LWIN, .dwFlags = KEYEVENTF_KEYUP}},
    };
    SendInput(6, inp, sizeof(INPUT));

    SetCursorPos(CenterX(tgt), CenterY(tgt));
}

int ProcessMonCommand(const wchar_t *arg) {
    long long tTotal = StartMeasuring();
    BOOL send = FALSE;
    if (wcsncmp(arg, L"send:", 5) == 0) {
        send = TRUE;
        arg += 5;
    }
    if (!arg || !*arg)
        return 1;

    enum Dir d = ParseDir(arg);
    if (d == DIR_INVALID)
        return 1;

    long long tE = StartMeasuring();
    MonList list = EnumMonitors();
    Log(LOG_PERF, L"EnumMonitors: %.2f ms", FinishMeasuring(tE));
    if (list.count <= 1)
        return 0;

    HMONITOR cur;
    if (send) {
        HWND fg = GetForegroundWindow();
        if (!fg)
            return 1;
        cur = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT cpt;
        GetCursorPos(&cpt);
        cur = MonitorFromPoint(cpt, MONITOR_DEFAULTTONEAREST);
    }
    int ci = FindCurrentMonIndex(&list, cur);
    if (ci < 0)
        return 1;

    long long tT = StartMeasuring();
    int ti = FindTargetMonIndex(&list, ci, d);
    Log(LOG_PERF, L"FindTargetMonIndex: %.2f ms", FinishMeasuring(tT));
    if (ti < 0)
        return 0;

    long long tA = StartMeasuring();
    const RECT *tgt = &list.mons[ti].rc;
    if (send)
        SendToMonitor(tgt, d);
    else
        GotoMonitor(tgt);
    Log(LOG_PERF, L"action: %.2f ms", FinishMeasuring(tA));

    Log(LOG_PERF, L"ProcessMonCommand total: %.2f ms", FinishMeasuring(tTotal));
    return 0;
}
