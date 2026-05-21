#include "helm.h"

/*
 * uri:<uri>
 *
 * Opens the URI in the default handler (browser, ms-settings, mailto, etc).
 */
int ProcessUriCommand(const wchar_t *arg) {
    if (!arg || !arg[0])
        return 1;
    if (IsElevated() && ShellExecAsUser(arg))
        return 0;
    ShellExecuteW(NULL, L"open", arg, NULL, NULL, SW_SHOWNORMAL);
    return 0;
}
