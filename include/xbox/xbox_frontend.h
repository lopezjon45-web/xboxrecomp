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

/* Whether an NV2A model holds the GPU's registers; the timer thread's GPU work needs one. */
int xbox_frontend_nv2a_attached(void);

/* The disc volume behind \Device\CdRom0, or NULL when D:\ is a host folder (xbox_path_init's game_dir). */
typedef struct XboxDiscVolume XboxDiscVolume;
const XboxDiscVolume *xbox_frontend_disc(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FRONTEND_H */
