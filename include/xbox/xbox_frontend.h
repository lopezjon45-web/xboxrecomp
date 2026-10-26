/*
 * xbox_frontend.h -- what the kernel bridges need from whichever CPU front end runs the guest.
 *
 * Exactly one front end defines these, picked by CMake's XBOXRECOMP_FRONTEND:
 *
 *   lifted   statically recompiled titles: recomp_lookup + fn() on the g_esp register file
 *            (src/kernel/frontend_lifted.c, compiled only in this mode)
 *   direct   guest code runs natively (xbrt). The program defines these and the symbols of the files in
 *            src/kernel/lifted-only.txt, which this mode does not compile; XBOXRECOMP_FRONTEND_DIRECT is 1.
 */
#ifndef XBOX_FRONTEND_H
#define XBOX_FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XboxCallConv { XBOX_CALL_STDCALL, XBOX_CALL_CDECL, XBOX_CALL_FASTCALL, XBOX_CALL_THISCALL } XboxCallConv;

/* Run guest code at `va` with `argc` dword arguments (argv[0] is the first, pushed last) on the calling thread and
 * return edx:eax. The guest stack pointer is the same after the call as before it, whatever the convention. Any host
 * thread may call it: a thread with no guest stack yet is given one. */
uint64_t xbox_frontend_guest_call(uint32_t va, unsigned argc, const uint32_t *argv, XboxCallConv conv);

/* PsCreateSystemThread(Ex)'s host side: a thread whose first act is guest_call(entry, argv, STDCALL). Returns the
 * host thread HANDLE the bridge puts behind the guest's handle (waits, suspend, resume and close act on it), or NULL
 * with *status set. NULL with *status == 0 means the routine already ran to its end on the calling thread. */
void *xbox_frontend_spawn_thread(uint32_t entry, unsigned argc, const uint32_t *argv, uint32_t stack_size,
                                 int priority, int suspended, uint32_t *status);

/* PsTerminateSystemThread. Returns only on a thread the front end does not end (the lifted main thread, which
 * unwinds to the host's main()). */
void xbox_frontend_exit_thread(uint32_t exit_status);

/* The calling thread's KPCR VA: fs:[0] is its SEH chain head and fs:[0x24] its IRQL. */
uint32_t xbox_frontend_kpcr(void);

/* Where the kernel data exports live (KeTickCount, XboxHardwareInfo, LaunchDataPage, object types, keys), a
 * committed 4 KB guest page. */
uint32_t xbox_frontend_kernel_data_base(void);

/* Whether the bridges may read and write [va, va + bytes) on the guest's behalf. */
int xbox_frontend_va_mapped(uint32_t va, uint32_t bytes);

/* HalReturnToFirmware: the session is leaving. Does not return to the guest. */
#if defined(_MSC_VER)
__declspec(noreturn)
#else
__attribute__((noreturn))
#endif
void xbox_frontend_return_to_hub(uint32_t reason);

/* RtlRaiseException: deliver the guest's EXCEPTION_RECORD at `record` to the title's fs:[0] handlers. Returns 1 when
 * the front end will resume the guest in its dispatcher instead of returning from the call, 0 when it cannot. */
int xbox_frontend_raise(uint32_t record);

/* A kernel call the runtime cannot carry out as asked. A front end that stops on a missing feature stops the guest
 * here, naming `what` and the call site, and does not return; one that does not logs it and returns, and the bridge
 * answers as it did before. */
void xbox_frontend_refuse(const char *what);

/* Whether an NV2A model holds the GPU's registers; the timer thread's GPU work needs one. */
int xbox_frontend_nv2a_attached(void);

/* Whether the GPU's interrupt line (vector 3) is up in a front end's own NV2A model, one that is not the fork's (so
 * xbox_frontend_nv2a_attached is 0). The timer thread calls the title's service routine while it is. */
int xbox_Nv2aIrqPending(void);

/* One file or directory of the disc. */
typedef struct XboxDiscEntry {
    uint32_t id;           /* the volume's name for it in read and child; stable for the session */
    uint32_t attributes;   /* the XDVDFS attribute byte (0x10 directory, 0x01 read-only, ...) */
    uint64_t size;         /* bytes; 0 for a directory */
} XboxDiscEntry;

/* The disc volume behind \Device\CdRom0 and D:\ (kernel_disc.c), read in place from the image. Every function may be
 * called from any guest thread at once. */
typedef struct XboxDiscVolume XboxDiscVolume;
struct XboxDiscVolume {
    /* `path` is below the disc root: backslash-separated, any case, no leading backslash; "" is the root directory.
     * Returns 1 and fills *out, or 0 when nothing has that name. */
    int (*find)(const XboxDiscVolume *v, const char *path, XboxDiscEntry *out);
    /* Copies up to `len` bytes of file `id` from `offset`. Returns the count, short only at the end of the file, or
     * -1 when the image could not be read. */
    int64_t (*read)(const XboxDiscVolume *v, uint32_t id, uint64_t offset, void *out, uint32_t len);
    /* Entry `index` of directory `id`, in the volume's order, and its name (NUL-terminated). Returns 0 past the last
     * entry. */
    int (*child)(const XboxDiscVolume *v, uint32_t id, uint32_t index, XboxDiscEntry *out, char name[256]);
    uint64_t image_bytes;
};

/* The mounted disc, or NULL when D:\ is a host folder (xbox_path_init's game_dir). */
const XboxDiscVolume *xbox_frontend_disc(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FRONTEND_H */
