/*
 * Alien Breed 3D I - PC Port
 * stub_audio.c - SDL2 sound effects (load from data/sounds, play via SDL)
 *
 * Sample names and order from Amiga LoadFromDisk.s SFX_NAMES.
 * Loads sounds/<name>.wav (e.g. scream.wav, shotgun.wav). IDs 28+ fall back to <id>.wav.
 */

#include "stub_audio.h"
#include "stub_io.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES      64
#define MAX_CHANNELS     8
#define NUM_NAMED_SFX    28
#define DEFAULT_FREQ     44100
#define DEFAULT_FORMAT  AUDIO_S16SYS
#define DEFAULT_CHANNELS 1

/* Amiga LoadFromDisk.s SFX_NAMES order: sample_id -> disk/sounds/<name> (we use sounds/<name>.wav) */
static const char *const sfx_names[NUM_NAMED_SFX] = {
    "scream",      /* 0 */
    "fire!",       /* 1 ShootName */
    "munch",       /* 2 */
    "shoot.dm",    /* 3 PooGunName */
    "collect",     /* 4 */
    "newdoor",     /* 5 */
    "splash",      /* 6 BassName */
    "footstep3",   /* 7 StompName */
    "lowscream",   /* 8 */
    "baddiegun",   /* 9 */
    "switch",      /* 10 */
    "switch1.sfx", /* 11 ReloadName */
    "noammo",      /* 12 */
    "splotch",     /* 13 */
    "splatpop",    /* 14 */
    "boom",        /* 15 */
    "newhiss",     /* 16 */
    "howl1",       /* 17 */
    "howl2",       /* 18 */
    "pant",        /* 19 */
    "whoosh",      /* 20 */
    "shotgun",     /* 21 ShotGunName */
    "flame",       /* 22 */
    "MuffledFoot", /* 23 */
    "footclop",    /* 24 */
    "footclank",   /* 25 */
    "teleport",    /* 26 */
    "HALFWORMPAIN" /* 27 */
};

/* One preloaded sample (converted to device format) */
typedef struct {
    Uint8  *data;
    Uint32  length;   /* bytes */
    int     loaded;
} LoadedSample;

/* One mixing channel (playing a sample) */
typedef struct {
    const Uint8 *sample_data;
    Uint32       sample_len;
    Uint32       position;   /* bytes played */
    int          volume;     /* 0-255 -> SDL_MixAudio uses 0-128 */
} Channel;

static SDL_AudioDeviceID g_device = 0;
static SDL_AudioSpec     g_spec;
static LoadedSample      g_samples[MAX_SAMPLES];
static Channel           g_channels[MAX_CHANNELS];
static int               g_next_channel = 0;
static int               g_audio_ready = 0;

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    memset(stream, 0, (size_t)len);

    for (int c = 0; c < MAX_CHANNELS; c++) {
        Channel *ch = &g_channels[c];
        if (ch->sample_data == NULL || ch->position >= ch->sample_len)
            continue;

        Uint32 remain = ch->sample_len - ch->position;
        Uint32 to_mix = (Uint32)len;
        if (to_mix > remain)
            to_mix = remain;

        /* SDL_MixAudioFormat expects volume 0-128 */
        int mix_vol = ch->volume;
        if (mix_vol > 255) mix_vol = 255;
        mix_vol = (mix_vol * 128) / 255;

        SDL_MixAudioFormat(stream, ch->sample_data + ch->position,
                           g_spec.format, (Uint32)to_mix, mix_vol);
        ch->position += to_mix;

        if (ch->position >= ch->sample_len) {
            ch->sample_data = NULL;
        }
    }
}

static int load_one_sample(int id)
{
    char subpath[80];
    char path[512];
    if (id < NUM_NAMED_SFX) {
        snprintf(subpath, sizeof(subpath), "sounds/%s.wav", sfx_names[id]);
    } else {
        snprintf(subpath, sizeof(subpath), "sounds/%d.wav", id);
    }
    io_make_data_path(path, sizeof(path), subpath);

    SDL_AudioSpec want;
    Uint8 *buf = NULL;
    Uint32 len = 0;

    if (!SDL_LoadWAV(path, &want, &buf, &len)) {
        return 0;  /* not found - skip silently for high IDs */
    }

    /* Convert to device format so we can mix in callback */
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, want.format, want.channels, want.freq,
                           g_spec.format, g_spec.channels, g_spec.freq) < 0) {
        printf("[AUDIO] sample %d: unsupported format (%s)\n", id, path);
        SDL_FreeWAV(buf);
        return 0;
    }

    cvt.len = (int)len;
    cvt.buf = (Uint8 *)SDL_malloc((size_t)len * cvt.len_mult);
    if (!cvt.buf) {
        printf("[AUDIO] sample %d: out of memory\n", id);
        SDL_FreeWAV(buf);
        return 0;
    }
    memcpy(cvt.buf, buf, (size_t)len);
    SDL_FreeWAV(buf);

    if (SDL_ConvertAudio(&cvt) < 0) {
        printf("[AUDIO] sample %d: convert failed\n", id);
        SDL_free(cvt.buf);
        return 0;
    }

    g_samples[id].data = cvt.buf;
    g_samples[id].length = (Uint32)(cvt.len_cvt);
    g_samples[id].loaded = 1;
    printf("[AUDIO] loaded %d (%s): %s (%u bytes)\n",
           id, id < NUM_NAMED_SFX ? sfx_names[id] : "?", path, (unsigned)g_samples[id].length);
    return 1;
}

static void free_samples(void)
{
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (g_samples[i].loaded && g_samples[i].data) {
            SDL_free(g_samples[i].data);
            g_samples[i].data = NULL;
            g_samples[i].length = 0;
            g_samples[i].loaded = 0;
        }
    }
}

void audio_init(void)
{
    printf("[AUDIO] init\n");
    memset(g_samples, 0, sizeof(g_samples));
    memset(g_channels, 0, sizeof(g_channels));
    g_next_channel = 0;
    g_audio_ready = 0;
    g_device = 0;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        printf("[AUDIO] SDL_Init AUDIO failed: %s\n", SDL_GetError());
        return;
    }

    g_spec.freq = DEFAULT_FREQ;
    g_spec.format = DEFAULT_FORMAT;
    g_spec.channels = (Uint8)DEFAULT_CHANNELS;
    g_spec.samples = 512;
    g_spec.callback = audio_callback;
    g_spec.userdata = NULL;

    g_device = SDL_OpenAudioDevice(NULL, 0, &g_spec, &g_spec, 0);
    if (g_device == 0) {
        printf("[AUDIO] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }

    /* Load samples by Amiga name (sounds/scream.wav, sounds/shotgun.wav, ...) */
    {
        char first_path[512];
        io_make_data_path(first_path, sizeof(first_path), "sounds/scream.wav");
        printf("[AUDIO] Loading sound effects from data/sounds (e.g. %s)\n", first_path);
    }
    int loaded_count = 0;
    for (int i = 0; i < MAX_SAMPLES; i++) {
        loaded_count += load_one_sample(i);
    }

    SDL_PauseAudioDevice(g_device, 0);
    g_audio_ready = 1;
    if (loaded_count > 0) {
        printf("[AUDIO] Loaded %d sound effect(s): ", loaded_count);
        for (int i = 0, n = 0; i < MAX_SAMPLES && n < loaded_count; i++) {
            if (g_samples[i].loaded) {
                printf("%s%s", n ? ", " : "", i < NUM_NAMED_SFX ? sfx_names[i] : "?");
                n++;
            }
        }
        printf("\n");
    } else {
        printf("[AUDIO] No WAV files found in data/sounds (scream.wav, shotgun.wav, ...)\n");
    }
}

void audio_shutdown(void)
{
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    free_samples();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_audio_ready = 0;
    printf("[AUDIO] shutdown\n");
}

void audio_init_player(void)       { /* stub */ }
void audio_stop_player(void)       { /* stub */ }
void audio_rem_player(void)        { /* stub */ }
void audio_load_module(const char *filename) { (void)filename; }
void audio_init_module(void)       { /* stub */ }
void audio_play_module(void)       { /* stub */ }
void audio_unload_module(void)     { /* stub */ }

void audio_play_sfx(int sfx_id, int volume, int channel)
{
    (void)channel;
    audio_play_sample(sfx_id, volume);
}

void audio_play_sample(int sample_id, int volume)
{
    if (!g_audio_ready || g_device == 0)
        return;
    if (sample_id < 0 || sample_id >= MAX_SAMPLES)
        return;
    if (!g_samples[sample_id].loaded || g_samples[sample_id].data == NULL) {
        /* Log once per missing sample (no spam) - helps debug "no sound" */
        static unsigned char logged[MAX_SAMPLES];
        if (sample_id < MAX_SAMPLES && !logged[sample_id]) {
            logged[sample_id] = 1;
            if (sample_id < NUM_NAMED_SFX) {
                printf("[AUDIO] play_sample(%d (%s), %d) - not loaded (add %s.wav to data/sounds)\n",
                       sample_id, sfx_names[sample_id], volume, sfx_names[sample_id]);
            } else {
                printf("[AUDIO] play_sample(%d, %d) - not loaded (add %d.wav to data/sounds)\n",
                       sample_id, volume, sample_id);
            }
        }
        return;
    }

    SDL_LockAudioDevice(g_device);

    /* Pick a channel (round-robin so new sounds override oldest) */
    Channel *ch = &g_channels[g_next_channel];
    g_next_channel = (g_next_channel + 1) % MAX_CHANNELS;

    ch->sample_data = g_samples[sample_id].data;
    ch->sample_len  = g_samples[sample_id].length;
    ch->position    = 0;
    ch->volume      = volume;

    SDL_UnlockAudioDevice(g_device);
}

void audio_stop_all(void)
{
    if (!g_audio_ready || g_device == 0) return;
    SDL_LockAudioDevice(g_device);
    for (int c = 0; c < MAX_CHANNELS; c++) {
        g_channels[c].sample_data = NULL;
        g_channels[c].position = 0;
    }
    SDL_UnlockAudioDevice(g_device);
}

void audio_mt_init(void)           { /* stub */ }
void audio_mt_end(void)            { /* stub */ }
