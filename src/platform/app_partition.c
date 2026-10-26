/* The ABSENT half of app_partition.h: names WindowsApp.lib does not export, on functions the partition has. */
#if XBOXRECOMP_UWP

#include "app_partition.h"
#include "xbox/xbox_shell.h"
#include <stdio.h>
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

#define GUARD_MAX 4
static wchar_t s_guarded[MAX_PATH];
static wchar_t s_allowed[GUARD_MAX][MAX_PATH];
static int s_allowed_count;

/* Whether `path` is `folder` or below it, case-insensitively, at a separator. */
static int under(const wchar_t *path, const wchar_t *folder)
{
    size_t n = wcslen(folder);
    return n && _wcsnicmp(path, folder, n) == 0 && (path[n] == 0 || path[n] == L'\\' || folder[n - 1] == L'\\');
}

static void full_path(const wchar_t *path, wchar_t out[MAX_PATH])
{
    DWORD n = GetFullPathNameW(path, MAX_PATH, out, NULL);
    if (!n || n >= MAX_PATH)
        wcsncpy(out, path, MAX_PATH - 1), out[MAX_PATH - 1] = 0;
    n = (DWORD)wcslen(out);
    while (n > 3 && out[n - 1] == L'\\')
        out[--n] = 0;
}

void xbox_app_guard_writes(const wchar_t *guarded, const wchar_t *const *allowed, int count)
{
    full_path(guarded, s_guarded);
    s_allowed_count = count < GUARD_MAX ? count : GUARD_MAX;
    for (int i = 0; i < s_allowed_count; i++)
        full_path(allowed[i], s_allowed[i]);
}

/* A folder on the way to an allowed one may be made too: the saves folder needs its parents. */
static int write_allowed(const wchar_t *path, int folder)
{
    wchar_t full[MAX_PATH];
    if (!s_guarded[0] || !path)
        return 1;
    full_path(path, full);
    if (!under(full, s_guarded))
        return 1;
    for (int i = 0; i < s_allowed_count; i++)
        if (under(full, s_allowed[i]) || (folder && under(s_allowed[i], full)))
            return 1;
    {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 20)
            fprintf(stderr, "  [FILE] refused a write outside the runtime's folders: %S\n", full);
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return 0;
}

HANDLE WINAPI xbox_app_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                   DWORD disposition, DWORD flags, HANDLE template_file)
{
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
                         FILE_WRITE_EA | DELETE;
    if (((access & writes) || disposition != OPEN_EXISTING) && !write_allowed(name, 0))
        return INVALID_HANDLE_VALUE;
    return CreateFileFromAppW(name, access, share, security, disposition, flags, template_file);
}

BOOL WINAPI xbox_app_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security)
{
    return write_allowed(name, 1) && CreateDirectoryFromAppW(name, security);
}

BOOL WINAPI xbox_app_DeleteFileW(LPCWSTR name)
{
    return write_allowed(name, 0) && DeleteFileFromAppW(name);
}

BOOL WINAPI xbox_app_RemoveDirectoryW(LPCWSTR name)
{
    return write_allowed(name, 0) && RemoveDirectoryFromAppW(name);
}

BOOL WINAPI xbox_app_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    return write_allowed(to, 0) && CopyFileFromAppW(from, to, fail_if_exists);
}

int WINAPI SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES *psa)
{
    wchar_t wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, pszPath, -1, wide, MAX_PATH))
        return ERROR_BAD_PATHNAME;
    return SHCreateDirectoryExW(hwnd, wide, psa);
}

#endif /* XBOXRECOMP_UWP */
