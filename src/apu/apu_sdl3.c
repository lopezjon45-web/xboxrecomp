/**
 * SDL3 Audio Output Backend (POSIX hosts, XBOXRECOMP_SDL3)
 *
 * apu_sdl2.c's xa2_* contract on SDL3, for a port whose window, input and
 * Vulkan surface are SDL3: a process cannot hold SDL2 and SDL3 at once (both
 * own the platform's audio and video state, and they export the same symbol
 * names). The APU frame thread hands over one EP period (256 stereo frames)
 * at a time and the device queue paces it (mcpx_apu_pace_step reads
 * xa2_queued_samples), exactly as with SDL2. SDL3's audio stream is the
 * queue: SDL_PutAudioStreamData pushes, the device thread pulls one device
 * buffer at a time, and SDL_GetAudioStreamQueued is what is left.
 *
 * The marks and knobs are apu_sdl2.c's: RECOMP_AUDIO_BUF_SAMPLES (default
 * 512, the device buffer, asked of SDL through
 * SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) and RECOMP_AUDIO_BUF_COUNT (default
 * 4, a pacing mark here, not a slot limit).
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIO_DRIVER (SDL3's name) or SDL_AUDIODRIVER is already set: it drains
 * the queue in real time without a device. SDL3 reads its own copy of the
 * environment, taken before this runs, so the choice is made with a hint.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

#if !defined(_WIN32)

#include <SDL3/SDL.h>
#include <time.h>

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS      2
#define SDLA_FRAME_BYTES   ((int)(SDLA_CHANNELS * sizeof(int16_t)))
#define SDLA_BUF_SAMPLES_DEFAULT 512
#define SDLA_NUM_BUFS_DEFAULT    4
#define SDLA_BUF_SAMPLES_MIN     128
#define SDLA_BUF_SAMPLES_MAX     4096
#define SDLA_NUM_BUFS_MIN        2
#define SDLA_NUM_BUFS_MAX        8

static SDL_AudioStream *g_stream;
static int       g_buf_samples = SDLA_BUF_SAMPLES_DEFAULT;
static int       g_num_bufs    = SDLA_NUM_BUFS_DEFAULT;
static int       g_initialized;
static int       g_audio_opened;    /* this backend brought SDL audio up */
static int       g_frames_written;  /* periods handed to the queue */
static unsigned  g_underruns;
static uint64_t  g_samples_queued;  /* samples handed to the queue, ever */
static int       g_dry;
static int64_t   g_underrun_logged_ms;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int xa2_init(void)
{
    SDL_AudioSpec spec;
    const char *e, *driver;
    char frames[16];
    int granted = 0;

    if (g_initialized) return 1;

    g_buf_samples = env_int("RECOMP_AUDIO_BUF_SAMPLES", SDLA_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int("RECOMP_AUDIO_BUF_COUNT", SDLA_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);

    /* Headless: no speakers unless the caller names a driver. */
    e = getenv("RECOMP_HEADLESS");
    if (e && *e && *e != '0' && !getenv("SDL_AUDIO_DRIVER") && !getenv("SDL_AUDIODRIVER"))
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    /* The device buffer, asked before the device opens; SDL may grant
     * another size, which is read back below. */
    snprintf(frames, sizeof frames, "%d", g_buf_samples);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[SDL3] audio init failed: %s\n", SDL_GetError());
            return 0;
        }
        g_audio_opened = 1;
    }

    /* No format changes allowed: the mixer's 48 kHz s16 stereo is what the
     * stream takes, and SDL converts for the device if it has to. No
     * callback: a push queue, as SDL_QueueAudio was. */
    SDL_zero(spec);
    spec.freq     = SDLA_SAMPLE_RATE;
    spec.format   = SDL_AUDIO_S16;
    spec.channels = SDLA_CHANNELS;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                         NULL, NULL);
    if (!g_stream) {
        fprintf(stderr, "[SDL3] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }
    {
        SDL_AudioSpec dev_spec;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream),
                                     &dev_spec, &granted) && granted > 0
                && granted != g_buf_samples) {
            fprintf(stderr, "[SDL3] device buffer is %d samples (asked %d)\n",
                    granted, g_buf_samples);
            g_buf_samples = granted;
            /* The queue marks and the pacer's stall window assume buffers no
             * larger than the env limit; a larger grant is still fed. */
            if (g_buf_samples > SDLA_BUF_SAMPLES_MAX)
                g_buf_samples = SDLA_BUF_SAMPLES_MAX;
        }
    }

    g_frames_written = 0;
    g_samples_queued = 0;
    g_dry = 0;
    g_initialized = 1;
    SDL_ResumeAudioStreamDevice(g_stream);

    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[SDL3] audio initialized: driver %s (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE, g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            driver && !strcmp(driver, "dummy") ? " [silent: no device output]" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_stream) {
        SDL_PauseAudioStreamDevice(g_stream);
        SDL_ClearAudioStream(g_stream);
        SDL_DestroyAudioStream(g_stream);
        g_stream = NULL;
    }
    if (g_audio_opened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_opened = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL3] shut down (%d periods written, %u underruns)\n",
                g_frames_written, g_underruns);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

/* Hand one EP period to the queue. SDL copies the samples. Nothing is
 * refused by depth: pacing by queue depth holds the queue at the marks. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!g_initialized || !g_stream || num_samples <= 0) return 0;
    if (!SDL_PutAudioStreamData(g_stream, samples, num_samples * SDLA_FRAME_BYTES)) {
        fprintf(stderr, "[SDL3] SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    g_samples_queued += (uint64_t)num_samples;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

/* Samples queued and not yet pulled by the device thread, as apu_sdl2.c
 * counts them; also its underrun detector: an empty queue after audio has
 * started is a dry spell, counted once and reported at most once a second.
 * A drain announced by xa2_expect_drain (the frame thread pausing) is not. */
int xa2_queued_samples(void)
{
    int bytes;

    if (!g_initialized || !g_stream) return 0;
    bytes = SDL_GetAudioStreamQueued(g_stream);
    if (bytes < 0) bytes = 0;
    if (bytes == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            g_underruns++;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL3] underrun x%u\n", g_underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return bytes / SDLA_FRAME_BYTES;
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_underruns;
}

/* What the device thread has pulled off the queue; it pulls a device buffer
 * at a time, so this leads the speaker by up to one buffer. */
uint64_t xa2_samples_played(void)
{
    int bytes;
    uint64_t queued;

    if (!g_initialized || !g_stream) return 0;
    bytes = SDL_GetAudioStreamQueued(g_stream);
    queued = bytes > 0 ? (uint64_t)(bytes / SDLA_FRAME_BYTES) : 0;
    return g_samples_queued > queued ? g_samples_queued - queued : 0;
}

#endif /* !_WIN32 */
