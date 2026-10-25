/*
 * xbox_shell.h -- what a host shell provides when the program has no Win32 window.
 *
 * Built only with XBOXRECOMP_UWP (the Xbox One S app partition). There the window is a CoreWindow, the device is
 * D3D11On12 over a D3D12 queue, pads are Windows.Gaming.Input and the data lives in the package's LocalState. The
 * runtime sees none of that: it gets a D3D11 device, one render target per frame, XInput-shaped pads and a folder.
 *
 * The shell implements every function here except xbox_shell_app_main, which the program provides (the old
 * WinMain body). The header lives in the toolkit because the toolkit is the lowest layer that calls it.
 *
 * Threads. The shell checks each rule and calls xbox_shell_fatal on a breach:
 *   UI thread          the shell's. Creates the device and swap chain before the game thread exists, draws the
 *                      import screen, then only dispatches CoreWindow events.
 *   game thread        created by the shell; runs xbox_shell_app_main.
 *   presenting thread  the first thread to call xbox_shell_frame_begin after the game starts (in Def Jam the
 *                      runtime's nv2a_ack_thread: flip -> d3d11_translator_frame_end -> dev_Present).
 *                      frame_begin and frame_present are only ever called from it.
 *   pad thread         the first thread to call xbox_shell_pad_read (input_host.c reader_thread). pad_read and
 *                      pad_rumble are only ever called from it.
 *   any thread         xbox_shell_display_hz, xbox_shell_data_root, xbox_shell_fatal.
 */
#ifndef XBOX_SHELL_H
#define XBOX_SHELL_H

#if !XBOXRECOMP_UWP
#error "xbox_shell.h is the app-partition host contract; desktop builds own their window"
#endif

#include <windows.h>
#include <d3d11.h>
#include <xinput.h>   /* XINPUT_GAMEPAD only: the pad shape input_host.c already speaks */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Provided by the program, called by the shell ------------------------------------------------------- */

/* The program's entry, on the game thread. Before calling it the shell has created the device, finished the
 * data import, set the current directory to xbox_shell_data_root(), set DEFJAM_DATA to the same folder, and
 * written console defaults into settings.ini where a key is absent. The return value is logged; the shell then
 * exits the app. */
int xbox_shell_app_main(void);

/* ---- Graphics ------------------------------------------------------------------------------------------- */

/* The device and its immediate context, AddRef'd for the caller. Feature level 11_1 is guaranteed: the shell
 * never starts the game on less. Game thread (d3d8_CreateDevice). The context is then used by the presenting
 * thread, as the desktop build uses its own. */
HRESULT xbox_shell_gfx_device(ID3D11Device **device, ID3D11DeviceContext **immediate);

/* One frame's window target. Borrowed: valid from frame_begin until frame_present returns. Never Release it,
 * never keep it across frames (the back buffer changes every present). */
typedef struct XboxShellFrame {
    ID3D11RenderTargetView *rtv;     /* the current back buffer, acquired for D3D11 */
    unsigned width, height;          /* of the back buffer, in pixels */
    unsigned buffer;                 /* which back buffer; the shell checks begin and present agree */
    int acquired;                    /* set by frame_begin, cleared by frame_present */
} XboxShellFrame;

/* Acquire the current back buffer for D3D11 and return its target. Returns 0 (frame->acquired == 0) when there
 * is nothing to draw into, including while the window is hidden; the caller then skips drawing but still calls
 * frame_present. Presenting thread. */
int xbox_shell_frame_begin(XboxShellFrame *frame);

/* Release the wrapped back buffer (when frame->acquired), flush the D3D11 context to the D3D12 queue and present
 * with this sync interval. Everything that draws into frame->rtv must already be recorded. Clears
 * frame->acquired and frame->rtv. Returns S_OK without presenting when nothing was acquired. Presenting thread. */
HRESULT xbox_shell_frame_present(XboxShellFrame *frame, int sync_interval);

/* The display's refresh rate in Hz, read once by the shell at start. Any thread. */
unsigned xbox_shell_display_hz(void);

/* ---- Pads ----------------------------------------------------------------------------------------------- */

/* Slot 0-3. Returns 1 and fills *out when a pad is in that slot, 0 when none. Slots are stable: a pad keeps its
 * slot until it is unplugged. Hot-plug is the shell's business (it rescans at most once a second, on this
 * thread). Pad thread. */
int xbox_shell_pad_read(unsigned slot, XINPUT_GAMEPAD *out);

/* Rumble, in XINPUT_VIBRATION's units. No-op for an empty slot. Pad thread. */
void xbox_shell_pad_rumble(unsigned slot, unsigned short low, unsigned short high);

/* ---- Process -------------------------------------------------------------------------------------------- */

/* LocalState, absolute, no trailing separator. Valid for the life of the process. Any thread. */
const wchar_t *xbox_shell_data_root(void);

/* Write the message to LocalState\shell.txt (flushed) and the run log, then end the app. Any thread. */
__declspec(noreturn) void xbox_shell_fatal(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_SHELL_H */
