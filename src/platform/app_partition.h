/*
 * app_partition.h -- the Win32 the runtime keeps using that WINAPI_FAMILY_PC_APP headers take away.
 *
 * Force-included (/FI) into every compile of an XBOXRECOMP_UWP build by the toolchain-side CMake, so it reaches
 * files that include <windows.h> directly as well as those that go through xbox_winnt.h or host.h. With it,
 * kernel_file.c, kernel_path.c, kernel_sync.c, kernel_bridge.c, main.c's VEH install, pc_mods.cpp and recorder.c
 * compile exactly as upstream wrote them. It plays the part win32_compat.h plays on POSIX hosts.
 *
 * Two kinds of name, and nothing else belongs in this file:
 *
 *   PRESENT  The header hides the declaration but WindowsApp.lib exports the import (measured: strings on
 *            WindowsApp.lib, SDK 10.0.26100, 2026-10-25). Declared by hand. Each entry names its API set and the
 *            shell.txt line that proves it runs on the console (the shell's selftest prints one line per entry
 *            and refuses to start the game when one fails).
 *   ABSENT   Not in WindowsApp.lib at all. Implemented in app_partition.c on functions the partition has, with
 *            the original's contract.
 *
 * Windows, devices and pads are not here. They are the shell's (xbox/xbox_shell.h).
 */
#ifndef XBOX_APP_PARTITION_H
#define XBOX_APP_PARTITION_H

#if XBOXRECOMP_UWP

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PRESENT ------------------------------------------------------------------------------------------------ */

/* api-ms-win-core-errorhandling-l1-1-1. Proof: "selftest veh ok". Used by main.c (veh_handler) and
 * xbox_memory_layout.c (ac97_write_veh, watch_veh). */
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
WINBASEAPI ULONG WINAPI RemoveVectoredExceptionHandler(PVOID Handle);

/* api-ms-win-core-file-l1-1-0. Proof: "selftest createfile ok". LocalState only: on E: these fail with error 5
 * (vault-one-s/probe/FINDINGS.md), and the runtime never opens E: (the shell's importer does, with FromApp). */
WINBASEAPI HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                     LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                     DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
WINBASEAPI HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                     LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                     DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

/* api-ms-win-core-kernel32-legacy-l1-1-0. Proof: "selftest section ok". xbox_memory_layout.c, the RAM section. */
WINBASEAPI HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes, DWORD flProtect,
                                            DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);

/* api-ms-win-core-threadpool-legacy-l1-1-0. Proof: "selftest timerqueue ok". kernel_sync.c's Xbox kernel
 * timers. If this line ever reads FAIL on the console, these three move to ABSENT with a threadpool-timer
 * implementation in app_partition.c; kernel_sync.c still does not change. */
WINBASEAPI HANDLE WINAPI CreateTimerQueue(void);
WINBASEAPI BOOL WINAPI CreateTimerQueueTimer(PHANDLE phNewTimer, HANDLE TimerQueue, WAITORTIMERCALLBACK Callback,
                                             PVOID Parameter, DWORD DueTime, DWORD Period, ULONG Flags);
WINBASEAPI BOOL WINAPI DeleteTimerQueueTimer(HANDLE TimerQueue, HANDLE Timer, HANDLE CompletionEvent);

/* api-ms-win-mm-time-l1-1-0. Proof: "selftest timeperiod ok". input_host.c reader_thread. */
#ifndef _MMRESULT_
#define _MMRESULT_
typedef UINT MMRESULT;
#endif
#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0
#endif
__declspec(dllimport) MMRESULT WINAPI timeBeginPeriod(UINT uPeriod);
__declspec(dllimport) MMRESULT WINAPI timeEndPeriod(UINT uPeriod);

/* api-ms-win-core-memory-l1-1-6. In the app headers already; named here because the layout's proof line is
 * "selftest layout ok" (placeholder reserved, split, a section mapped twice, a hole faults). */

/* ---- ABSENT (app_partition.c) ------------------------------------------------------------------------------- */

/* Signal then reset. Defined under its own name and reached through a macro: kernel32.lib, linked ahead of
 * WindowsApp.lib (see the shell's CMake), carries a PulseEvent thunk that would clash with the stand-in; the
 * shell-folder names below are in no linked library. Releases the threads already waiting on a manual-reset event, or one waiter of an
 * auto-reset event, and leaves the event clear. kernel_bridge.c NtPulseEvent/KePulseEvent. */
BOOL WINAPI xbox_app_PulseEvent(HANDLE hEvent);
#define PulseEvent xbox_app_PulseEvent

/* CSIDL_LOCAL_APPDATA answers xbox_shell_data_root() (LocalState); any other folder is E_INVALIDARG.
 * kernel_path.c, reached only when the program passes no save folder (Def Jam always passes one). */
#ifndef CSIDL_LOCAL_APPDATA
#define CSIDL_LOCAL_APPDATA 0x001c
#endif
HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath);

/* Create the missing folders on the path with CreateDirectoryW. A folder that exists is never passed to it
 * (GetFileAttributesW first), so an ancestor the package may only read is not asked for a create. Returns
 * ERROR_SUCCESS when the leaf was created, ERROR_ALREADY_EXISTS when it existed, otherwise the first other
 * error, like the originals. kernel_path.c, texture_pack.c. */
int WINAPI SHCreateDirectoryExW(HWND hwnd, LPCWSTR pszPath, const SECURITY_ATTRIBUTES *psa);
int WINAPI SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES *psa);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_UWP */
#endif /* XBOX_APP_PARTITION_H */
