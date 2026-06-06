#include "helm.h"

static BOOL SendChord(BOOL shift) {
    INPUT in[6];
    WORD seq[3];
    int s = 0;
    seq[s++] = VK_CONTROL;
    if (shift)
        seq[s++] = VK_SHIFT;
    seq[s++] = 'V';

    int n = 0;
    for (int i = 0; i < s; i++)
        in[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = seq[i]}};
    for (int i = s - 1; i >= 0; i--)
        in[n++] = (INPUT){.type = INPUT_KEYBOARD,
                          .ki = {.wVk = seq[i], .dwFlags = KEYEVENTF_KEYUP}};

    UINT sent = SendInput(n, in, sizeof(INPUT));
    return sent == (UINT)n;
}

int ProcessPasteCommand(BOOL plain) {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 1;

    wchar_t cls[128] = {0};
    GetClassNameW(fg, cls, countof(cls));
    BOOL term = lstrcmpiW(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0;

    Log(LOG_TRACE, L"paste: term=%d plain=%d cls=%ls", term, plain, cls);
    return SendChord(term ? TRUE : plain) ? 0 : 1;
}
