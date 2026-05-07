#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <debugapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <strsafe.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static inline void _dbg_out(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0)
        OutputDebugStringA(buf);
}
#define printf(fmt, ...) _dbg_out(fmt, ##__VA_ARGS__)

static const char *KANATA_HOST = "127.0.0.1";
static int KANATA_PORT = 53456;
static const wchar_t *PIPE_NAME = L"\\\\.\\pipe\\helm";
static const wchar_t *READY_EVENT_NAME = L"helm-daemon-ready";

static BOOL ExtractMessage(const char *json, char *out, int outSize) {
    const char *key = "\"message\":";
    const char *start = strstr(json, key);
    if (!start)
        return FALSE;
    start += strlen(key);
    while (*start == ' ' || *start == '\t')
        start++;

    const char *valStart = NULL, *valEnd = NULL;
    if (*start == '"') {
        valStart = start + 1;
        valEnd = strchr(valStart, '"');
    } else if (*start == '[') {
        start++;
        while (*start == ' ' || *start == '\t')
            start++;
        if (*start == '"') {
            valStart = start + 1;
            valEnd = strchr(valStart, '"');
        }
    }
    if (!valStart || !valEnd)
        return FALSE;

    int len = (int)(valEnd - valStart);
    if (len >= outSize)
        len = outSize - 1;
    memcpy(out, valStart, len);
    out[len] = 0;
    return TRUE;
}

/*
 * Write msg to the helm pipe.
 * Retries top to 3 times on ERROR_PIPE_BUSY (all server instances occupied).
 */
static BOOL SendToPipe(const char *msg) {
    wchar_t wmsg[512];
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg, 512);

    if (!WaitNamedPipeW(PIPE_NAME, 200))
        return FALSE;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 3 && pipe == INVALID_HANDLE_VALUE; i++) {
        pipe = CreateFileW(PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0,
                           NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY) {
                printf("[PIPE] CreateFile failed: %lu\n", GetLastError());
                return FALSE;
            }
            printf("[PIPE] Busy, retrying...\n");
            WaitNamedPipeW(PIPE_NAME, 200);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE)
        return FALSE;

    DWORD written;
    BOOL ok = WriteFile(pipe, wmsg, (DWORD)(wcslen(wmsg) * sizeof(wchar_t)),
                        &written, NULL);
    CloseHandle(pipe);
    return ok;
}

static void SpawnSibling(const wchar_t *exe, const wchar_t *args) {
    wchar_t self[MAX_PATH], dir[MAX_PATH], target[MAX_PATH],
        cmd[MAX_PATH + 128];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t *slash = wcsrchr(self, L'\\');
    if (slash)
        *(slash + 1) = L'\0';
    StringCchCopyW(dir, MAX_PATH, self);
    StringCchPrintfW(target, MAX_PATH, L"%s%s", dir, exe);
    StringCchPrintfW(cmd, MAX_PATH + 128, L"\"%s\" %s", target,
                     args ? args : L"");

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, dir, &si,
                       &pi)) {
        printf("[LAUNCH] %S started.\n", exe);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        printf("[LAUNCH] %S failed. Err=%lu\n", exe, GetLastError());
    }
}

static BOOL SendToPipeWithRetry(const char *msg) {
    for (int i = 0; i < 2; i++) {
        if (SendToPipe(msg))
            return TRUE;
        Sleep(80);
    }
    printf("[PIPE] FAIL. Spawning helm daemon...\n");
    SpawnSibling(L"helm.exe", L"--server");

    HANDLE ev = NULL;
    for (int i = 0; i < 10; i++) {
        ev = OpenEventW(SYNCHRONIZE, FALSE, READY_EVENT_NAME);
        if (ev)
            break;
        Sleep(50);
    }
    if (ev) {
        WaitForSingleObject(ev, 1000);
        CloseHandle(ev);
    } else {
        Sleep(300);
    }

    if (SendToPipe(msg)) {
        printf("[PIPE] Retry success.\n");
        return TRUE;
    }
    printf("[PIPE] RETRY FAIL. Dropped: %s\n", msg);
    return FALSE;
}

static void RunBridge(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    char recvBuf[2048];
    int recvLen = 0;

    while (1) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(KANATA_PORT);
        inet_pton(AF_INET, KANATA_HOST, &addr.sin_addr);

        printf("Connecting to Kanata TCP %s:%d...\n", KANATA_HOST, KANATA_PORT);
        while (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            closesocket(sock);
            Sleep(2000);
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }
        printf("Connected. Waiting for events...\n");
        recvLen = 0;

        while (1) {
            if ((size_t)recvLen >= sizeof(recvBuf) - 1) {
                char *nl = strchr(recvBuf, '\n');
                if (nl) {
                    int keep = recvLen - (int)(nl - recvBuf + 1);
                    memmove(recvBuf, nl + 1, keep);
                    recvLen = keep;
                } else {
                    recvLen = 0;
                }
            }
            int ret =
                recv(sock, recvBuf + recvLen, sizeof(recvBuf) - recvLen - 1, 0);
            if (ret <= 0) {
                printf("Disconnected. ret=%d\n", ret);
                break;
            }
            recvLen += ret;
            recvBuf[recvLen] = 0;

            char *lineStart = recvBuf;
            char *newline;
            while ((newline = strchr(lineStart, '\n')) != NULL) {
                *newline = 0;
                if (strstr(lineStart, "\"MessagePush\"")) {
                    char msg[512] = {0};
                    if (ExtractMessage(lineStart, msg, sizeof(msg))) {
                        printf("[PIPE] Sending: %s\n", msg);
                        SendToPipeWithRetry(msg);
                    } else {
                        printf("[PARSE] FAIL on: %s\n", lineStart);
                    }
                }
                lineStart = newline + 1;
            }
            int remaining = recvLen - (int)(lineStart - recvBuf);
            if (remaining > 0)
                memmove(recvBuf, lineStart, remaining);
            recvLen = remaining;
        }
        closesocket(sock);
        Sleep(2000);
    }
    WSACleanup();
}

int main(int argc, char *argv[]) {
    FreeConsole();
    if (argc > 1)
        KANATA_PORT = atoi(argv[1]);

    /* Spawn siblings from the same directory as this binary.
     * Useful when started from Task Scheduler without a working directory. */

    // wchar_t kargs[64];
    // StringCchPrintfW(kargs, 64, L"-c config.kbd -p %d", KANATA_PORT);

    // SpawnSibling(L"kanata.exe", kargs);
    // SpawnSibling(L"helm.exe", L"--server");

    RunBridge();
    return 0;
}
