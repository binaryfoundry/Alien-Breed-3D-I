/*
 * Alien Breed 3D I - PC Port
 * stub_display.h - Stubbed display/rendering subsystem
 *
 * Replaces all Amiga-specific display code:
 *   - Copper list setup (BigFieldCop, TextCop, titlecop, etc.)
 *   - Screen allocation (AllocCopperScrnMemory, AllocTextScrn)
 *   - DMA/blitter operations
 *   - Chunky-to-planar conversion
 *   - DrawDisplay, wall/floor/object rendering
 *   - Sprite handling
 *   - Palette/color management
 *   - Panel (HUD) drawing
 *
 * All functions are no-ops for now. When a real renderer is added
 * (SDL2, OpenGL, etc.), these stubs get replaced.
 */

#ifndef STUB_DISPLAY_H
#define STUB_DISPLAY_H

#include "game_state.h"

/* Lifecycle */
void display_init(void);
void display_shutdown(void);
int display_is_fullscreen(void);  /* non-zero if window is fullscreen */
void display_on_resize(int w, int h);  /* call on window resize to resize framebuffer */

/* Screen management */
void display_alloc_text_screen(void);
void display_release_text_screen(void);
void display_alloc_copper_screen(void);
void display_release_copper_screen(void);
void display_alloc_title_memory(void);
void display_release_title_memory(void);
void display_alloc_panel_memory(void);
void display_release_panel_memory(void);

/* Title screen */
void display_setup_title_screen(void);
void display_load_title_screen(void);
void display_clear_opt_screen(void);
void display_draw_opt_screen(int screen_num);
void display_fade_up_title(int amount);
void display_fade_down_title(int amount);
void display_clear_title_palette(void);

/* In-game rendering */
void display_init_copper_screen(void);
void display_draw_display(GameState *state);  /* Renders 3D scene to chunky buffer */
void display_swap_buffers(void);
void display_wait_vblank(void);

/* HUD */
void display_energy_bar(int16_t energy);
void display_ammo_bar(int16_t ammo);

/* Text */
void display_draw_line_of_text(const char *text, int line);
void display_clear_text_screen(void);

#endif /* STUB_DISPLAY_H */
