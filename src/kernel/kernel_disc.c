/*
 * kernel_disc.c - the disc volume's files (XDVDFS), read in place
 *
 * A front end that mounts a disc image (xbox_frontend_disc) serves D:\ and
 * \Device\CdRom0\ from it instead of a host folder. kernel_file.c sends every
 * path xbox_disc_path claims here, and every call on a handle xbox_disc_owns.
 *
 * The answers follow the console's read-only volume: a create disposition
 * other than open, or any write access, is STATUS_ACCESS_DENIED before the
 * name is looked up; a missing name is STATUS_OBJECT_NAME_NOT_FOUND when its
 * folder exists and STATUS_OBJECT_PATH_NOT_FOUND when it does not; a
 * directory query returns one entry per call.
 *
 * A file's handle is a host event, set and never reset: guest waits on a file
 * handle (the completion of a request it made) return at once, and NtClose
 * closes it like any other handle.
 */

#include "kernel.h"
#include "xbox/xbox_frontend.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#define STATUS_INFO_LENGTH_MISMATCH_  ((NTSTATUS)0xC0000004L)
#define STATUS_FILE_IS_A_DIRECTORY_   ((NTSTATUS)0xC00000BAL)
#define STATUS_NOT_A_DIRECTORY_       ((NTSTATUS)0xC0000103L)
#define STATUS_IO_DEVICE_ERROR_       ((NTSTATUS)0xC0000185L)

#define DISC_SECTOR 2048u
#define DISC_MAX_OPEN 512
#define FILE_USE_FILE_POINTER_POSITION_ 0xFFFFFFFEu

/* Every access bit that asks to change the volume. */
#define DISC_WRITE_ACCESS (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL | \
                           XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA | 0x0010u /* FILE_WRITE_EA */ | \
                           0x0040u /* FILE_DELETE_CHILD */ | XBOX_FILE_WRITE_ATTRIBUTES | XBOX_DELETE)

typedef struct {
    HANDLE        handle;           /* NULL for a free slot */
    XboxDiscEntry entry;
    uint64_t      position;
    char          path[MAX_PATH];   /* below the disc root, for opens relative to it */
    /* A directory's enumeration: where the next query starts, the pattern the
     * first query gave, and whether any entry matched it yet. */
    uint32_t      next_child;
    char          pattern[MAX_PATH];
    BOOL          scanning;
    BOOL          matched;
} DiscOpen;

static DiscOpen s_open[DISC_MAX_OPEN];
static SRWLOCK  s_lock = SRWLOCK_INIT;

static BOOL is_directory(const XboxDiscEntry *e)
{
    return (e->attributes & XBOX_FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* The attributes a query reports: the on-disc byte, or NORMAL for a file that
 * carries none. */
static ULONG reported_attributes(const XboxDiscEntry *e)
{
    return e->attributes ? e->attributes : XBOX_FILE_ATTRIBUTE_NORMAL;
}

static LONGLONG allocation_size(const XboxDiscEntry *e)
{
    return (LONGLONG)((e->size + DISC_SECTOR - 1) & ~(uint64_t)(DISC_SECTOR - 1));
}

/* Caller holds s_lock. */
static DiscOpen *slot_of(HANDLE handle)
{
    if (!handle)
        return NULL;
    for (int i = 0; i < DISC_MAX_OPEN; i++)
        if (s_open[i].handle == handle)
            return &s_open[i];
    return NULL;
}

static void complete(PXBOX_IO_STATUS_BLOCK iosb, NTSTATUS status, ULONG_PTR information)
{
    if (iosb) {
        iosb->Status = status;
        iosb->Information = information;
    }
}

static NTSTATUS missing_status(const XboxDiscVolume *v, const char *below)
{
    char parent[MAX_PATH];
    const char *slash = strrchr(below, '\\');
    XboxDiscEntry e;

    if (!slash)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    snprintf(parent, sizeof(parent), "%.*s", (int)(slash - below), below);
    if (v->find(v, parent, &e) && is_directory(&e))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    return STATUS_OBJECT_PATH_NOT_FOUND;
}

NTSTATUS xbox_disc_create(const char *below, ACCESS_MASK access, ULONG disposition,
                          ULONG options, PHANDLE handle, PXBOX_IO_STATUS_BLOCK iosb)
{
    const XboxDiscVolume *v = xbox_frontend_disc();
    XboxDiscEntry e;
    DiscOpen *slot = NULL;
    HANDLE h;
    NTSTATUS st;

    if (!v || !handle) {
        complete(iosb, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }
    if ((disposition != XBOX_FILE_OPEN && disposition != XBOX_FILE_OPEN_IF) || (access & DISC_WRITE_ACCESS)) {
        complete(iosb, STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }
    if (!v->find(v, below, &e)) {
        st = missing_status(v, below);
        complete(iosb, st, 0);
        return st;
    }
    if ((options & XBOX_FILE_DIRECTORY_FILE) && !is_directory(&e)) {
        complete(iosb, STATUS_NOT_A_DIRECTORY_, 0);
        return STATUS_NOT_A_DIRECTORY_;
    }
    if ((options & XBOX_FILE_NON_DIRECTORY_FILE) && is_directory(&e)) {
        complete(iosb, STATUS_FILE_IS_A_DIRECTORY_, 0);
        return STATUS_FILE_IS_A_DIRECTORY_;
    }

    h = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!h) {
        complete(iosb, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < DISC_MAX_OPEN && !slot; i++)
        if (!s_open[i].handle)
            slot = &s_open[i];
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->handle = h;
        slot->entry = e;
        snprintf(slot->path, sizeof(slot->path), "%s", below);
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (!slot) {
        CloseHandle(h);
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "disc: more than %d files open at once", DISC_MAX_OPEN);
        complete(iosb, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *handle = h;
    complete(iosb, STATUS_SUCCESS, 1 /* FILE_OPENED */);
    return STATUS_SUCCESS;
}

int xbox_disc_owns(HANDLE handle)
{
    int owned;
    AcquireSRWLockShared(&s_lock);
    owned = slot_of(handle) != NULL;
    ReleaseSRWLockShared(&s_lock);
    return owned;
}

/* The slot's path is fixed from create to close, and the caller holds the
 * handle for the length of its call. */
const char *xbox_disc_handle_path(HANDLE handle)
{
    DiscOpen *slot;
    AcquireSRWLockShared(&s_lock);
    slot = slot_of(handle);
    ReleaseSRWLockShared(&s_lock);
    return slot ? slot->path : NULL;
}

NTSTATUS xbox_disc_read(HANDLE handle, PXBOX_IO_STATUS_BLOCK iosb, PVOID buffer,
                        ULONG length, PLARGE_INTEGER offset)
{
    const XboxDiscVolume *v = xbox_frontend_disc();
    XboxDiscEntry e;
    uint64_t at;
    int64_t got;
    DiscOpen *slot;

    AcquireSRWLockShared(&s_lock);
    slot = slot_of(handle);
    if (slot) {
        e = slot->entry;
        at = slot->position;
    }
    ReleaseSRWLockShared(&s_lock);
    if (!slot || !v) {
        complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    if (is_directory(&e)) {
        complete(iosb, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (offset && !(offset->HighPart == -1 && offset->LowPart == FILE_USE_FILE_POINTER_POSITION_))
        at = (uint64_t)offset->QuadPart;
    if (at >= e.size) {
        complete(iosb, STATUS_END_OF_FILE, 0);
        return length ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    }
    if ((uint64_t)length > e.size - at)
        length = (ULONG)(e.size - at);
    got = v->read(v, e.id, at, buffer, length);
    if (got < 0) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "disc: image read failed in %s at %llu (+%u)",
                 xbox_disc_handle_path(handle), (unsigned long long)at, length);
        complete(iosb, STATUS_IO_DEVICE_ERROR_, 0);
        return STATUS_IO_DEVICE_ERROR_;
    }
    AcquireSRWLockExclusive(&s_lock);
    slot = slot_of(handle);
    if (slot)
        slot->position = at + (uint64_t)got;
    ReleaseSRWLockExclusive(&s_lock);
    complete(iosb, STATUS_SUCCESS, (ULONG_PTR)got);
    return STATUS_SUCCESS;
}

static void fill_network_open(const XboxDiscEntry *e, PXBOX_FILE_NETWORK_OPEN_INFORMATION info)
{
    memset(info, 0, sizeof(*info));
    info->EndOfFile.QuadPart = (LONGLONG)e->size;
    info->AllocationSize.QuadPart = allocation_size(e);
    info->FileAttributes = reported_attributes(e);
}

NTSTATUS xbox_disc_query_information(HANDLE handle, PXBOX_IO_STATUS_BLOCK iosb, PVOID info,
                                     ULONG length, XBOX_FILE_INFORMATION_CLASS info_class)
{
    XboxDiscEntry e;
    uint64_t position = 0;
    DiscOpen *slot;
    ULONG need;

    AcquireSRWLockShared(&s_lock);
    slot = slot_of(handle);
    if (slot) {
        e = slot->entry;
        position = slot->position;
    }
    ReleaseSRWLockShared(&s_lock);
    if (!slot) {
        complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    switch (info_class) {
        case XboxFileBasicInformation:            need = sizeof(XBOX_FILE_BASIC_INFORMATION); break;
        case XboxFileStandardInformation:         need = sizeof(XBOX_FILE_STANDARD_INFORMATION); break;
        case XboxFilePositionInformation:         need = sizeof(XBOX_FILE_POSITION_INFORMATION); break;
        case XboxFileNetworkOpenInformation:      need = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION); break;
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "disc: NtQueryInformationFile class %d not served", (int)info_class);
            complete(iosb, STATUS_INVALID_INFO_CLASS, 0);
            return STATUS_INVALID_INFO_CLASS;
    }
    if (length < need) {
        complete(iosb, STATUS_INFO_LENGTH_MISMATCH_, 0);
        return STATUS_INFO_LENGTH_MISMATCH_;
    }
    memset(info, 0, need);
    switch (info_class) {
        case XboxFileBasicInformation:
            ((PXBOX_FILE_BASIC_INFORMATION)info)->FileAttributes = reported_attributes(&e);
            break;
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION s = (PXBOX_FILE_STANDARD_INFORMATION)info;
            s->AllocationSize.QuadPart = allocation_size(&e);
            s->EndOfFile.QuadPart = (LONGLONG)e.size;
            s->NumberOfLinks = 1;
            s->Directory = is_directory(&e);
            break;
        }
        case XboxFilePositionInformation:
            ((PXBOX_FILE_POSITION_INFORMATION)info)->CurrentByteOffset.QuadPart = (LONGLONG)position;
            break;
        case XboxFileNetworkOpenInformation:
            fill_network_open(&e, (PXBOX_FILE_NETWORK_OPEN_INFORMATION)info);
            break;
        default:
            break;
    }
    complete(iosb, STATUS_SUCCESS, need);
    return STATUS_SUCCESS;
}

NTSTATUS xbox_disc_set_information(HANDLE handle, PXBOX_IO_STATUS_BLOCK iosb, PVOID info,
                                   ULONG length, XBOX_FILE_INFORMATION_CLASS info_class)
{
    DiscOpen *slot;

    if (info_class != XboxFilePositionInformation) {
        complete(iosb, STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }
    if (length < sizeof(XBOX_FILE_POSITION_INFORMATION)) {
        complete(iosb, STATUS_INFO_LENGTH_MISMATCH_, 0);
        return STATUS_INFO_LENGTH_MISMATCH_;
    }
    if (((PXBOX_FILE_POSITION_INFORMATION)info)->CurrentByteOffset.QuadPart < 0) {
        complete(iosb, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }
    AcquireSRWLockExclusive(&s_lock);
    slot = slot_of(handle);
    if (slot)
        slot->position = (uint64_t)((PXBOX_FILE_POSITION_INFORMATION)info)->CurrentByteOffset.QuadPart;
    ReleaseSRWLockExclusive(&s_lock);
    if (!slot) {
        complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    complete(iosb, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS xbox_disc_query_volume(HANDLE handle, PXBOX_IO_STATUS_BLOCK iosb, PVOID info,
                                ULONG length, XBOX_FS_INFORMATION_CLASS info_class)
{
    const XboxDiscVolume *v = xbox_frontend_disc();
    PXBOX_FILE_FS_SIZE_INFORMATION size = (PXBOX_FILE_FS_SIZE_INFORMATION)info;

    (void)handle;
    if (info_class != XboxFileFsSizeInformation) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "disc: NtQueryVolumeInformationFile class %d not served", (int)info_class);
        complete(iosb, STATUS_INVALID_INFO_CLASS, 0);
        return STATUS_INVALID_INFO_CLASS;
    }
    if (length < sizeof(*size)) {
        complete(iosb, STATUS_INFO_LENGTH_MISMATCH_, 0);
        return STATUS_INFO_LENGTH_MISMATCH_;
    }
    size->TotalAllocationUnits.QuadPart = (LONGLONG)(v ? v->image_bytes / DISC_SECTOR : 0);
    size->AvailableAllocationUnits.QuadPart = 0;
    size->SectorsPerAllocationUnit = 1;
    size->BytesPerSector = DISC_SECTOR;
    complete(iosb, STATUS_SUCCESS, sizeof(*size));
    return STATUS_SUCCESS;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* `*` any run, `?` any one character, case-insensitive; "*.*" is every name,
 * as the DOS form titles inherit from Win32 means it. */
static BOOL name_matches(const char *pattern, const char *name)
{
    const char *star = NULL, *resume = NULL;

    if (strcmp(pattern, "*.*") == 0)
        return TRUE;
    while (*name) {
        if (*pattern == '*') {
            star = pattern++;
            resume = name;
        } else if (*pattern == '?' || lower(*pattern) == lower(*name)) {
            pattern++;
            name++;
        } else if (star) {
            pattern = star + 1;
            name = ++resume;
        } else {
            return FALSE;
        }
    }
    while (*pattern == '*')
        pattern++;
    return *pattern == '\0';
}

NTSTATUS xbox_disc_query_directory(HANDLE handle, PXBOX_IO_STATUS_BLOCK iosb, PVOID info,
                                   ULONG length, XBOX_FILE_INFORMATION_CLASS info_class,
                                   PXBOX_ANSI_STRING pattern, BOOLEAN restart)
{
    const XboxDiscVolume *v = xbox_frontend_disc();
    const ULONG header = (ULONG)(ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName;
    PXBOX_FILE_DIRECTORY_INFORMATION out = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    XboxDiscEntry dir, child;
    char want[MAX_PATH], name[256];
    uint32_t index;
    BOOL first, found = FALSE;
    DiscOpen *slot;
    NTSTATUS st;

    if (info_class != XboxFileDirectoryInformation) {
        complete(iosb, STATUS_INVALID_INFO_CLASS, 0);
        return STATUS_INVALID_INFO_CLASS;
    }
    if (length < header) {
        complete(iosb, STATUS_INFO_LENGTH_MISMATCH_, 0);
        return STATUS_INFO_LENGTH_MISMATCH_;
    }
    AcquireSRWLockExclusive(&s_lock);
    slot = slot_of(handle);
    if (slot && (restart || !slot->scanning)) {
        size_t n = pattern && pattern->Buffer ? pattern->Length : 0;
        if (n >= sizeof(slot->pattern))
            n = sizeof(slot->pattern) - 1;
        if (n)
            memcpy(slot->pattern, pattern->Buffer, n);
        else
            slot->pattern[n++] = '*';
        slot->pattern[n] = '\0';
        slot->next_child = 0;
        slot->scanning = TRUE;
        slot->matched = FALSE;
    }
    if (slot) {
        dir = slot->entry;
        index = slot->next_child;
        first = !slot->matched;
        snprintf(want, sizeof(want), "%s", slot->pattern);
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (!slot || !v) {
        complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    if (!is_directory(&dir)) {
        complete(iosb, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }

    while (v->child(v, dir.id, index, &child, name)) {
        index++;
        if (name_matches(want, name)) {
            found = TRUE;
            break;
        }
    }
    AcquireSRWLockExclusive(&s_lock);
    slot = slot_of(handle);
    if (slot) {
        slot->next_child = index;
        slot->matched |= found;
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (!found) {
        st = first ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        complete(iosb, st, 0);
        return st;
    }

    {
        ULONG name_len = (ULONG)strlen(name);
        ULONG fits = length - header < name_len ? length - header : name_len;
        memset(out, 0, header);
        out->EndOfFile.QuadPart = (LONGLONG)child.size;
        out->AllocationSize.QuadPart = allocation_size(&child);
        out->FileAttributes = reported_attributes(&child);
        out->FileNameLength = name_len;
        memcpy(out->FileName, name, fits);
        st = fits < name_len ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        complete(iosb, st, header + fits);
        return st;
    }
}

NTSTATUS xbox_disc_full_attributes(const char *below, PXBOX_FILE_NETWORK_OPEN_INFORMATION info)
{
    const XboxDiscVolume *v = xbox_frontend_disc();
    XboxDiscEntry e;

    if (!v || !info)
        return STATUS_INVALID_PARAMETER;
    if (!v->find(v, below, &e))
        return missing_status(v, below);
    fill_network_open(&e, info);
    return STATUS_SUCCESS;
}

void xbox_disc_close(HANDLE handle)
{
    DiscOpen *slot;
    AcquireSRWLockExclusive(&s_lock);
    slot = slot_of(handle);
    if (slot)
        slot->handle = NULL;
    ReleaseSRWLockExclusive(&s_lock);
    if (slot)
        CloseHandle(handle);
}

#endif /* _WIN32 */
