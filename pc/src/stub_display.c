/*
 * Alien Breed 3D I - PC Port
 * stub_display.c - SDL2 display backend
 *
 * Creates a window, takes the chunky buffer from the software renderer,
 * converts it through a palette to RGB, and presents it on screen.
 */

#include "stub_display.h"
#include "renderer.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * SDL2 state
 * ----------------------------------------------------------------------- */
static SDL_Window   *g_window   = NULL;
static SDL_Renderer *g_sdl_ren  = NULL;
static SDL_Texture  *g_texture  = NULL;

/* Scale factor for the window. Halved when RENDER_SCALE doubles so window size stays the same. */
#define WINDOW_SCALE  4
#define WINDOW_H      (RENDER_HEIGHT * WINDOW_SCALE)
#define WINDOW_W      (WINDOW_H * 4 / 3)   /* 4:3 aspect ratio */

/* Legacy palette no longer needed - colors come from the .wad LUT data
 * and are written directly to the rgb_buffer as ARGB8888 pixels. */

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */
void display_init(void)
{
    printf("[DISPLAY] SDL2 init\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("[DISPLAY] SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    g_window = SDL_CreateWindow(
        "Alien Breed 3D I",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_SHOWN
    );
    if (!g_window) {
        printf("[DISPLAY] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }

    g_sdl_ren = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_sdl_ren) {
        printf("[DISPLAY] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return;
    }

    /* Texture to upload the chunky buffer into */
    g_texture = SDL_CreateTexture(g_sdl_ren,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        RENDER_WIDTH, RENDER_HEIGHT);
    if (!g_texture) {
        printf("[DISPLAY] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return;
    }

    SDL_SetTextureScaleMode(g_texture, SDL_ScaleModeNearest);

    renderer_init();

    printf("[DISPLAY] SDL2 ready: %dx%d window (game: %dx%d)\n",
           WINDOW_W, WINDOW_H, RENDER_WIDTH, RENDER_HEIGHT);
}

void display_shutdown(void)
{
    renderer_shutdown();
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_sdl_ren)  SDL_DestroyRenderer(g_sdl_ren);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
    printf("[DISPLAY] SDL2 shutdown\n");
}

/* -----------------------------------------------------------------------
 * Screen management (no-ops for SDL2)
 * ----------------------------------------------------------------------- */
void display_alloc_text_screen(void)        { }
void display_release_text_screen(void)      { }
void display_alloc_copper_screen(void)      { }
void display_release_copper_screen(void)    { }
void display_alloc_title_memory(void)       { }
void display_release_title_memory(void)     { }
void display_alloc_panel_memory(void)       { }
void display_release_panel_memory(void)     { }

void display_setup_title_screen(void)       { }
void display_load_title_screen(void)        { }
void display_clear_opt_screen(void)         { }
void display_draw_opt_screen(int screen_num) { (void)screen_num; }
void display_fade_up_title(int amount)      { (void)amount; }
void display_fade_down_title(int amount)    { (void)amount; }
void display_clear_title_palette(void)      { }

void display_init_copper_screen(void)       { }

/* -----------------------------------------------------------------------
 * Main rendering
 * ----------------------------------------------------------------------- */
void display_draw_display(GameState *state)
{
    /* 1. Software-render the 3D scene into the rgb buffer */
    renderer_draw_display(state);

    /* 2. Copy 32-bit ARGB rgb_buffer directly to SDL texture */
    if (!g_texture || !g_sdl_ren) return;

    const uint32_t *src = renderer_get_rgb_buffer();
    if (!src) return;

    uint32_t *pixels;
    int pitch;
    if (SDL_LockTexture(g_texture, NULL, (void**)&pixels, &pitch) < 0) return;

    for (int y = 0; y < RENDER_HEIGHT; y++) {
        const uint32_t *src_row = src + y * RENDER_WIDTH;
        uint32_t *dst_row = (uint32_t*)((uint8_t*)pixels + y * pitch);
        memcpy(dst_row, src_row, RENDER_WIDTH * sizeof(uint32_t));
    }

    SDL_UnlockTexture(g_texture);

    /* 3. Present to screen */
    SDL_RenderClear(g_sdl_ren);
    SDL_RenderCopy(g_sdl_ren, g_texture, NULL, NULL);
    SDL_RenderPresent(g_sdl_ren);

    /* Debug: show player position in window title */
    {
        PlayerState *dbg_plr = &state->plr1;
        char title[128];
        snprintf(title, sizeof(title), "AB3D - Pos(%d,%d) Zone=%d Ang=%d",
                 (int)(dbg_plr->xoff >> 16), (int)(dbg_plr->zoff >> 16),
                 dbg_plr->zone, dbg_plr->angpos);
        SDL_SetWindowTitle(g_window, title);
    }
}

void display_swap_buffers(void)
{
    /* Handled in display_draw_display */
}

void display_wait_vblank(void)
{
    /* VSync is handled by SDL_RENDERER_PRESENTVSYNC */
}

/* -----------------------------------------------------------------------
 * HUD
 * ----------------------------------------------------------------------- */
void display_energy_bar(int16_t energy)
{
    uint32_t *rgb = g_renderer.rgb_buffer;
    if (!rgb) return;
    int bar_y = RENDER_HEIGHT - 2;
    int bar_w = (energy > 0) ? ((int)energy * (RENDER_WIDTH - 4) / 127) : 0;
    for (int x = 2; x < 2 + bar_w && x < RENDER_WIDTH - 2; x++) {
        rgb[bar_y * RENDER_WIDTH + x] = 0xFF00CC00;       /* bright green */
        rgb[(bar_y - 1) * RENDER_WIDTH + x] = 0xFF00CC00;
    }
}

void display_ammo_bar(int16_t ammo)
{
    uint32_t *rgb = g_renderer.rgb_buffer;
    if (!rgb) return;
    int bar_y = RENDER_HEIGHT - 5;
    int max_ammo = 999;
    int bar_w = (ammo > 0) ? ((int)ammo * (RENDER_WIDTH - 4) / max_ammo) : 0;
    if (bar_w > RENDER_WIDTH - 4) bar_w = RENDER_WIDTH - 4;
    for (int x = 2; x < 2 + bar_w && x < RENDER_WIDTH - 2; x++) {
        rgb[bar_y * RENDER_WIDTH + x] = 0xFFCCCC00;       /* bright yellow */
        rgb[(bar_y - 1) * RENDER_WIDTH + x] = 0xFFCCCC00;
    }
}

/* -----------------------------------------------------------------------
 * Text (minimal for now)
 * ----------------------------------------------------------------------- */
void display_draw_line_of_text(const char *text, int line)
{
    (void)text;
    (void)line;
}

void display_clear_text_screen(void)
{
}
