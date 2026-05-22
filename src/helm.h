#pragma once

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <wchar.h>

/* ============================================================
 * Logging
 * ============================================================ */

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

enum { LOG_OFF = 0, LOG_PERF = 1, LOG_TRACE = 2 };

#ifdef STRIP_LOGS
#define InitLogging()
#define InitQPC()
#define StartMeasuring() (0LL)
#define FinishMeasuring(start) (0.0)
#define Log(level, fmt, ...)
#else
extern int LOG_LEVEL;

void InitLogging(void);
void InitQPCImpl(void);
long long StartMeasuringImpl(void);
double FinishMeasuringImpl(long long start);
void LogCore(int level, const wchar_t *fmt, ...);

#define InitQPC()                                                              \
    do {                                                                       \
        if (LOG_LEVEL >= LOG_PERF)                                             \
            InitQPCImpl();                                                     \
    } while (0)
#define StartMeasuring() (LOG_LEVEL >= LOG_PERF ? StartMeasuringImpl() : 0LL)
#define FinishMeasuring(start)                                                 \
    (LOG_LEVEL >= LOG_PERF ? FinishMeasuringImpl(start) : 0.0)
#define Log(level, fmt, ...)                                                   \
    do {                                                                       \
        if ((level) <= LOG_LEVEL)                                              \
            LogCore((level), fmt, ##__VA_ARGS__);                              \
    } while (0)
#endif

/* ============================================================
 * COM interfaces
 * ============================================================ */

#define COM_IUNK_VTBL(T)                                                       \
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **);          \
    ULONG(STDMETHODCALLTYPE *AddRef)(T *);                                     \
    ULONG(STDMETHODCALLTYPE *Release)(T *)

/* --- IVirtualDesktopManager --- */
static const CLSID CLSID_VDM = {
    0xaa509086,
    0x5ca9,
    0x4c25,
    {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
static const IID IID_IVDM = {0xa5cd92ff,
                             0x29be,
                             0x454c,
                             {0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b}};
typedef struct IVDM IVDM;
typedef struct {
    COM_IUNK_VTBL(IVDM);
    HRESULT(STDMETHODCALLTYPE *IsWindowOnCurrentVirtualDesktop)(IVDM *, HWND,
                                                                BOOL *);
    HRESULT(STDMETHODCALLTYPE *GetWindowDesktopId)(IVDM *, HWND, GUID *);
    HRESULT(STDMETHODCALLTYPE *MoveWindowToDesktop)(IVDM *, HWND, REFGUID);
} IVDMVtbl;
struct IVDM {
    const IVDMVtbl *lpVtbl;
};
#define IVDM_IsCurrent(p, h, out)                                              \
    (p)->lpVtbl->IsWindowOnCurrentVirtualDesktop(p, h, out)
#define IVDM_Release(p) (p)->lpVtbl->Release(p)
#define IVDMI_Release(p) (p)->lpVtbl->Release(p)

/* --- IServiceProvider --- */
static const IID IID_ISvcProv = {
    0x6D5140C1,
    0x7436,
    0x11CE,
    {0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA}};
typedef struct ISvcProv ISvcProv;
typedef struct {
    COM_IUNK_VTBL(ISvcProv);
    HRESULT(STDMETHODCALLTYPE *QueryService)(ISvcProv *, REFGUID sid,
                                             REFIID iid, void **);
} ISvcProvVtbl;
struct ISvcProv {
    const ISvcProvVtbl *lpVtbl;
};

/* --- IObjectArray --- */
typedef struct IObjectArray IObjectArray;
typedef struct {
    COM_IUNK_VTBL(IObjectArray);
    HRESULT(STDMETHODCALLTYPE *GetCount)(IObjectArray *, UINT *);
    HRESULT(STDMETHODCALLTYPE *GetAt)(IObjectArray *, UINT, REFIID, void **);
} IObjectArrayVtbl;
struct IObjectArray {
    const IObjectArrayVtbl *lpVtbl;
};

/*
 * IVirtualDesktop - Win11 22H2+ vtable layout:
 *   [3] IsViewVisible(IApplicationView*, BOOL*)
 *   [4] GetID(GUID*)
 */
static const IID IID_IVirtualDesktop = {
    0x3F07F4BE,
    0xB107,
    0x441A,
    {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};
typedef struct IVirtualDesktop IVirtualDesktop;
typedef struct {
    COM_IUNK_VTBL(IVirtualDesktop);
    HRESULT(STDMETHODCALLTYPE *IsViewVisible)(IVirtualDesktop *, void *,
                                              BOOL *);
    HRESULT(STDMETHODCALLTYPE *GetID)(IVirtualDesktop *, GUID *);
} IVirtualDesktopVtbl;
struct IVirtualDesktop {
    const IVirtualDesktopVtbl *lpVtbl;
};

/*
 * IVirtualDesktopManagerInternal
 *
 * IID_IVDMI changes with Windows builds:
 *   Win10:              {F31574D6-B682-4CDC-BD56-1827860ABEC6}
 *   Win11 21H2-22H1:   {B2F925B9-5A0F-4D2E-9F4D-2B1507593C10}
 *   Win11 23H2-25H2+:  {53F5CA0B-158F-4124-900C-057158060B27}
 *
 * Vtable slots 3+ (23H2-25H2):
 *   [3]  GetCount
 *   [4]  MoveViewToDesktop
 *   [5]  CanViewMoveDesktops
 *   [6]  GetCurrentDesktop
 *   [7]  GetDesktops
 *   [8]  GetAdjacentDesktop
 *   [9]  SwitchDesktop
 *   [10] SwitchDesktopAndMoveForegroundView  (24H2+ only)
 *   [11] CreateDesktopW
 */
static const CLSID CLSID_ImmersiveShell = {
    0xC2F03A33,
    0x21F5,
    0x47FA,
    {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
static const GUID CLSID_VDMI = {/* stable service SID */
                                0xC5E0CDCA,
                                0x7B6E,
                                0x41B2,
                                {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46,
                                 0x7B}};
static const IID IID_IVDMI = {/* update when Windows build changes it */
                              0x53F5CA0B,
                              0x158F,
                              0x4124,
                              {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
typedef struct IVDMI IVDMI;
typedef struct {
    COM_IUNK_VTBL(IVDMI);
    HRESULT(STDMETHODCALLTYPE *GetCount)(IVDMI *, UINT *);
    HRESULT(STDMETHODCALLTYPE *MoveViewToDesktop)(IVDMI *, void *,
                                                  IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *CanViewMoveDesktops)(IVDMI *, void *, BOOL *);
    HRESULT(STDMETHODCALLTYPE *GetCurrentDesktop)(IVDMI *, IVirtualDesktop **);
    HRESULT(STDMETHODCALLTYPE *GetDesktops)(IVDMI *, IObjectArray **);
    HRESULT(STDMETHODCALLTYPE *GetAdjacentDesktop)(IVDMI *, IVirtualDesktop *,
                                                   int, IVirtualDesktop **);
    HRESULT(STDMETHODCALLTYPE *SwitchDesktop)(IVDMI *, IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *SwitchDesktopAndMoveForegroundView)(
        IVDMI *, IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *CreateDesktopW)(IVDMI *, IVirtualDesktop **);
    HRESULT(STDMETHODCALLTYPE *RemoveDesktop)(IVDMI *, IVirtualDesktop *,
                                              IVirtualDesktop *);
    HRESULT(STDMETHODCALLTYPE *FindDesktop)(IVDMI *, GUID *,
                                            IVirtualDesktop **);
} IVDMIVtbl;
struct IVDMI {
    const IVDMIVtbl *lpVtbl;
};

/* ============================================================
 * Shared globals  (defined in helm.c, extern everywhere else)
 * ============================================================ */

extern IVDM *Vdm;          /* public VDM — current-desktop check  */
extern IVDMI *VdmInternal; /* internal VDM — switch / send        */

/* ============================================================
 * PID / HWND cache types
 * ============================================================ */

#define MAX_PIDS 2048
#define EXE_NAME_MAX 64
#define HWND_CACHE_SIZE 16
#define PIDCACHE_TTL_MS 300

typedef struct {
    DWORD pid;
    wchar_t exe[EXE_NAME_MAX];
} PidEntry;

/* ============================================================
 * Window-resize constants
 * ============================================================ */

/* SWP_ASYNCWINDOWPOS: daemon has no message queue; without ASYNC,
 * SetWindowPos on another thread's window can deadlock waiting for ack. */
#define SWP_RESIZE                                                             \
    (SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS)
#define MIN_DIM 50

/* ============================================================
 * AdjacentCtx — shared between helm_sz.c and helm_swap.c
 *
 * Set exactly ONE of edgeX/edgeY/rightEdgeX/bottomEdgeY; leave others -1.
 * refRect = fg rect; used to filter windows sharing the edge coord but not
 * overlapping in the perpendicular axis.
 * ============================================================ */

typedef struct {
    int edgeX, edgeY;
    int rightEdgeX;
    int bottomEdgeY;
    int tolerance;
    HWND skip;
    HWND found;
    RECT refRect;
} AdjacentCtx;

/* ============================================================
 * Function declarations
 * ============================================================ */

/* helm_cache.c */
void MaybeRebuildPidCache(void);
const wchar_t *GetExeFromPid(DWORD pid);
HWND LookupHwndCache(const wchar_t *exe, const wchar_t *cls);
void StoreHwndCache(const wchar_t *exe, HWND hwnd);
void ForceRebuildPidCache(void);
DWORD GetExplorerPid(void);

/* helm_app.c */
void BypassForegroundLock(void);
int ProcessAppCommand(const wchar_t *arg, BOOL global, BOOL admin);
BOOL IsElevated(void);

/* helm_vd.c */
void InitVdInternal(void);
int ProcessVdCommand(const wchar_t *arg);

/* helm_sz.c */
BOOL CALLBACK FindAdjacentProc(HWND hwnd, LPARAM lp);
int ProcessSzCommand(const wchar_t *arg);

/* helm_swap.c */
int ProcessSwapCommand(const wchar_t *arg);

/* helm_max_min.c */
int ProcessMaxCommand(void);
int ProcessMinCommand(void);

/* helm_uri.c */
int ProcessUriCommand(const wchar_t *arg);
