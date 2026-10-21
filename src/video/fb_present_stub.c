/*
 * The framebuffer window, absent: POSIX hosts built with XBOXRECOMP_SDL3.
 *
 * fb_present_sdl.c is the framebuffer window off Windows, and it is SDL2. A
 * port that builds the host side on SDL3 has its own window (its presenter
 * draws the GPU's render target, not guest RAM), and one process cannot hold
 * SDL2 and SDL3 at once: both own the platform's video and audio state and
 * export the same names. So in that build nothing here opens a window. The
 * kernel and the executor still report display modes and flips through these
 * calls, as they do on every host; they are no-ops, as on the hosts the
 * window did not reach before v0.14.0.
 *
 * xbox_HostWindowMain runs the guest on the calling thread: there is no
 * window to keep on the main thread.
 */
#include <stdint.h>

#if !defined(_WIN32)

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va; (void)pitch;
}

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    (void)sy;
}

void xbox_FramebufferWindowStart(void)
{
}

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va; (void)pitch;
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    (void)name; (void)max_chars;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    (void)draws;
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    return guest_main();
}

#endif /* !_WIN32 */
