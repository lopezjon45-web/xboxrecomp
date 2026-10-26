/* The ABSENT half of app_partition.h: names WindowsApp.lib does not export, on functions the partition has. */
#if XBOXRECOMP_UWP

#include "app_partition.h"
#include "xbox/xbox_shell.h"
#include <string.h>

BOOL WINAPI PulseEvent(HANDLE hEvent)
{
    if (!SetEvent(hEvent))
        return FALSE;
    return ResetEvent(hEvent);
}

HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath)
{
    (void)hwnd; (void)hToken; (void)dwFlags;
    if ((csidl & 0xff) != CSIDL_LOCAL_APPDATA)
        return E_INVALIDARG;
    wcsncpy(pszPath, xbox_shell_data_root(), MAX_PATH - 1);
    pszPath[MAX_PATH - 1] = 0;
    return S_OK;
}

/* The leaf first, then separator by separator from the root: a component that exists is left alone, one that is
 * missing and is not a drive or UNC root gets CreateDirectoryW. ERROR_ALREADY_EXISTS from that call is a race
 * with another creator, not a failure. */
int WINAPI SHCreateDirectoryExW(HWND hwnd, LPCWSTR pszPath, const SECURITY_ATTRIBUTES *psa)
{
    wchar_t buf[MAX_PATH];
    size_t n = wcslen(pszPath);
    (void)hwnd;
    if (n == 0 || n >= MAX_PATH)
        return ERROR_BAD_PATHNAME;
    if (GetFileAttributesW(pszPath) != INVALID_FILE_ATTRIBUTES)
        return ERROR_ALREADY_EXISTS;
    wcscpy(buf, pszPath);
    for (size_t i = 1; i <= n; i++) {
        if (i < n && buf[i] != L'\\' && buf[i] != L'/')
            continue;
        wchar_t saved = buf[i];
        buf[i] = 0;
        if (wcslen(buf) > 2 && !(wcslen(buf) == 3 && buf[1] == L':') &&
            GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES &&
            !CreateDirectoryW(buf, (LPSECURITY_ATTRIBUTES)psa)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS)
                return (int)e;
        }
        buf[i] = saved;
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI xbox_app_GetFileAttributesW(LPCWSTR lpFileName)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExFromAppW(lpFileName, GetFileExInfoStandard, &data))
        return INVALID_FILE_ATTRIBUTES;
    return data.dwFileAttributes;
}

int WINAPI SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES *psa)
{
    wchar_t wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, pszPath, -1, wide, MAX_PATH))
        return ERROR_BAD_PATHNAME;
    return SHCreateDirectoryExW(hwnd, wide, psa);
}

#endif /* XBOXRECOMP_UWP */
