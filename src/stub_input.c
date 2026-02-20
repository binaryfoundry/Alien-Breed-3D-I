/*
 * Alien Breed 3D I - PC Port
 * stub_input.c - SDL2 input backend
 *
 * Maps SDL2 keyboard scancodes to Amiga raw keycodes that the
 * game's key_map[] array expects.
 */

#include "stub_input.h"
#include "stub_display.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Amiga rawkey codes used by the game (from player.c default_keys
 * and game_loop.c KEY_ESC, ControlLoop.s).
 * ----------------------------------------------------------------------- */
#define AMIGA_KEY_ESC       0x45
#define AMIGA_KEY_SPACE     0x40
#define AMIGA_KEY_UP        0x4C
#define AMIGA_KEY_DOWN      0x4D
#define AMIGA_KEY_LEFT      0x4F
#define AMIGA_KEY_RIGHT     0x4E
#define AMIGA_KEY_RSHIFT    0x61
#define AMIGA_KEY_RALT      0x65
#define AMIGA_KEY_RAMIGA    0x67
#define AMIGA_KEY_D         0x22
#define AMIGA_KEY_L         0x28
#define AMIGA_KEY_PERIOD    0x39   /* . > */
#define AMIGA_KEY_SLASH     0x3A   /* / ? */
#define AMIGA_KEY_P         0x19   /* Pause */
#define AMIGA_KEY_1         0x01
#define AMIGA_KEY_2         0x02
#define AMIGA_KEY_3         0x03
#define AMIGA_KEY_4         0x04
#define AMIGA_KEY_5         0x05
#define AMIGA_KEY_6         0x06
#define AMIGA_KEY_7         0x07
#define AMIGA_KEY_8         0x08

/* -----------------------------------------------------------------------
 * SDL scancode -> Amiga rawkey mapping
 * ----------------------------------------------------------------------- */
static uint8_t sdl_to_amiga(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:   return AMIGA_KEY_ESC;
    case SDL_SCANCODE_SPACE:    return AMIGA_KEY_SPACE;
    case SDL_SCANCODE_UP:       return AMIGA_KEY_UP;
    case SDL_SCANCODE_W:        return AMIGA_KEY_UP;       /* WASD: W = forward */
    case SDL_SCANCODE_DOWN:     return AMIGA_KEY_DOWN;
    case SDL_SCANCODE_S:        return AMIGA_KEY_DOWN;     /* WASD: S = backward */
    case SDL_SCANCODE_LEFT:     return AMIGA_KEY_LEFT;
    case SDL_SCANCODE_RIGHT:    return AMIGA_KEY_RIGHT;
    case SDL_SCANCODE_A:        return AMIGA_KEY_PERIOD;   /* WASD: A = strafe left */
    case SDL_SCANCODE_D:        return AMIGA_KEY_SLASH;    /* WASD: D = strafe right -- override duck */
    case SDL_SCANCODE_LSHIFT:   return AMIGA_KEY_RSHIFT;   /* Shift = run */
    case SDL_SCANCODE_RSHIFT:   return AMIGA_KEY_RSHIFT;
    case SDL_SCANCODE_LALT:     return AMIGA_KEY_RALT;     /* Alt = fire */
    case SDL_SCANCODE_RALT:     return AMIGA_KEY_RALT;
    case SDL_SCANCODE_LCTRL:    return AMIGA_KEY_RALT;     /* Ctrl = fire too */
    case SDL_SCANCODE_RCTRL:    return AMIGA_KEY_RALT;
    case SDL_SCANCODE_PERIOD:   return AMIGA_KEY_PERIOD;   /* . = strafe left */
    case SDL_SCANCODE_SLASH:    return AMIGA_KEY_SLASH;    /* / = strafe right */
    case SDL_SCANCODE_C:        return AMIGA_KEY_D;        /* C = duck (original D) */
    case SDL_SCANCODE_L:        return AMIGA_KEY_L;        /* L = look behind */
    case SDL_SCANCODE_P:        return AMIGA_KEY_P;        /* P = pause */
    case SDL_SCANCODE_1:        return AMIGA_KEY_1;
    case SDL_SCANCODE_2:        return AMIGA_KEY_2;
    case SDL_SCANCODE_3:        return AMIGA_KEY_3;
    case SDL_SCANCODE_4:        return AMIGA_KEY_4;
    case SDL_SCANCODE_5:        return AMIGA_KEY_5;
    case SDL_SCANCODE_6:        return AMIGA_KEY_6;
    case SDL_SCANCODE_7:        return AMIGA_KEY_7;
    case SDL_SCANCODE_8:        return AMIGA_KEY_8;
    default:                    return 0xFF; /* unmapped */
    }
}

/* -----------------------------------------------------------------------
 * Mouse state
 * ----------------------------------------------------------------------- */
static MouseState g_mouse = {0};
static bool g_quit_requested = false;

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */
void input_init(void)
{
    printf("[INPUT] SDL2 input init\n");
    g_quit_requested = false;
    /* In fullscreen, capture mouse from the start; in windowed, user double-clicks to capture. */
    if (display_is_fullscreen()) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
    }
}

void input_shutdown(void)
{
    SDL_SetRelativeMouseMode(SDL_FALSE);
    printf("[INPUT] SDL2 input shutdown\n");
}

/* -----------------------------------------------------------------------
 * Per-frame polling
 * ----------------------------------------------------------------------- */
void input_update(uint8_t *key_map, uint8_t *last_pressed)
{
    /* Reset mouse deltas each frame */
    g_mouse.dx = 0;
    g_mouse.dy = 0;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            g_quit_requested = true;
            /* Map to ESC so the game exits cleanly */
            if (key_map) key_map[AMIGA_KEY_ESC] = 1;
            break;

        case SDL_KEYDOWN:
        {
            uint8_t amiga = sdl_to_amiga(ev.key.keysym.scancode);
            /* In windowed mode, Escape releases mouse capture and is not sent to the game */
            if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE && SDL_GetRelativeMouseMode()) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                /* Consume key so game doesn't quit */
            } else if (amiga != 0xFF && key_map) {
                key_map[amiga] = 1;
                if (last_pressed) *last_pressed = amiga;
            }
            break;
        }

        case SDL_KEYUP:
        {
            uint8_t amiga = sdl_to_amiga(ev.key.keysym.scancode);
            if (amiga != 0xFF && key_map) {
                key_map[amiga] = 0;
            }
            break;
        }

        case SDL_MOUSEMOTION:
            /* Only use mouse motion for look when captured */
            if (SDL_GetRelativeMouseMode()) {
                g_mouse.dx += (int16_t)ev.motion.xrel;
                g_mouse.dy += (int16_t)ev.motion.yrel;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            /* Double-click in window enables relative mouse mode (capture) */
            if (ev.button.clicks == 2) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
            }
            if (ev.button.button == SDL_BUTTON_LEFT) {
                g_mouse.left_button = true;
                if (SDL_GetRelativeMouseMode() && key_map) key_map[AMIGA_KEY_RALT] = 1;
            }
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                g_mouse.right_button = true;
                if (SDL_GetRelativeMouseMode() && key_map) key_map[AMIGA_KEY_SPACE] = 1;
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                g_mouse.left_button = false;
                if (SDL_GetRelativeMouseMode() && key_map) key_map[AMIGA_KEY_RALT] = 0;
            }
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                g_mouse.right_button = false;
                if (SDL_GetRelativeMouseMode() && key_map) key_map[AMIGA_KEY_SPACE] = 0;
            }
            break;

        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                display_on_resize(ev.window.data1, ev.window.data2);
            }
            break;
        }
    }
}

void input_read_mouse(MouseState *out)
{
    if (out) *out = g_mouse;
}

void input_read_joy1(JoyState *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

void input_read_joy2(JoyState *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

bool input_key_pressed(const uint8_t *key_map, uint8_t keycode)
{
    if (!key_map || keycode >= 128) return false;
    return key_map[keycode] != 0;
}

void input_clear_keyboard(uint8_t *key_map)
{
    if (key_map) memset(key_map, 0, 128);
}
