#include "helm.h"

/*
 * Directly maximizes the foreground window, bypassing the Windows snap state
 * machine. SW_MAXIMIZE skips the intermediate "unsnap to floating" step that
 * Win+Up goes through, maximizing in a single keystroke.
 */
int ProcessMaxCommand(void) {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;
    if (IsIconic(fg))
        ShowWindow(fg, SW_RESTORE);
    ShowWindow(fg, SW_MAXIMIZE);
    return 0;
}

/*
 * Same thing as max, but for minimizing. Minimizes windows unconditionally.
 */
int ProcessMinCommand(void) {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;
    if (IsIconic(fg))
        ShowWindow(fg, SW_RESTORE);
    ShowWindow(fg, SW_MINIMIZE);
    return 0;
}
