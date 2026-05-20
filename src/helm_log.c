#include "helm.h"

#ifndef STRIP_LOGS

#include <stdarg.h>

int LOG_LEVEL = LOG_OFF;

static FILE *DebugLogFile = NULL;
static long long g_qpc_freq;

void InitQPCImpl(void) {
    QueryPerformanceFrequency((LARGE_INTEGER *)&g_qpc_freq);
}

long long StartMeasuringImpl(void) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

double FinishMeasuringImpl(long long start) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (double)(li.QuadPart - start) * 1000.0 / (double)g_qpc_freq;
}

void InitLogging(void) {
    wchar_t buf[16];
    DWORD n = GetEnvironmentVariableW(L"HELM_LOG", buf, countof(buf));
    if (n > 0 && n < countof(buf)) {
        int lvl = _wtoi(buf);
        if (lvl >= LOG_OFF && lvl <= LOG_TRACE)
            LOG_LEVEL = lvl;
    }
    if (LOG_LEVEL == LOG_OFF)
        return;

    wchar_t path[MAX_PATH];
    DWORD pn = GetEnvironmentVariableW(L"HELM_LOG_FILE", path, MAX_PATH);
    if (pn == 0 || pn >= MAX_PATH) {
        DWORD mn = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (mn == 0 || mn >= MAX_PATH)
            return;
        wchar_t *slash = path;
        for (wchar_t *p = path; *p; p++)
            if (*p == L'\\' || *p == L'/')
                slash = p;
        slash[1] = L'\0';
        if (FAILED(StringCchCatW(path, MAX_PATH, L"helm.log")))
            return;
    }
    DebugLogFile = _wfopen(path, L"a,ccs=UTF-8");

    InitQPC();
}

void LogCore(int level, const wchar_t *fmt, ...) {
    if (!DebugLogFile)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t buffer[2048];
    const wchar_t *labels[] = {L"[OFF]", L"[PERF]", L"[TRACE]"};
    const wchar_t *label = (level >= 0 && level < 3) ? labels[level] : L"[???]";

    StringCchPrintfW(buffer, countof(buffer), L"[%02d:%02d:%02d] %ls ",
                     st.wHour, st.wMinute, st.wSecond, label);

    size_t pos = wcslen(buffer);
    va_list args;
    va_start(args, fmt);
    StringCchVPrintfW(buffer + pos, countof(buffer) - pos, fmt, args);
    va_end(args);

    size_t len = wcslen(buffer);
    while (len > 0 && (buffer[len - 1] == L'\n' || buffer[len - 1] == L'\r'))
        len--;
    if (len < countof(buffer) - 2) {
        buffer[len] = L'\n';
        buffer[len + 1] = L'\0';
    }

    fputws(buffer, DebugLogFile);
    fflush(DebugLogFile);
    OutputDebugStringW(buffer);
}

#endif
