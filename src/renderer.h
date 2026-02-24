/*
 * Alien Breed 3D I - PC Port
 * renderer.h - Software 3D renderer (chunky buffer)
 *
 * Translated from: AB3DI.s (DrawDisplay, RotateLevelPts, RotateObjectPts),
 *                  WallRoutine3.ChipMem.s, ObjDraw3.ChipRam.s, BumpMap.s
 *
 * The renderer draws into a chunky (8-bit indexed) pixel buffer.
 * Displaying that buffer to the actual screen is platform-specific
 * and handled by stub_display / a future SDL/OpenGL backend.
 */

#ifndef RENDERER_H
#define RENDERER_H

#include "game_state.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Framebuffer dimensions (from AB3DI.s)
 *
 * The Amiga version renders 96 columns of game view (columns 0-95)
 * into a buffer that is 104 longwords wide (416 bytes).
 * Screen height is 80 lines.
 * -----------------------------------------------------------------------*/
#define RENDER_SCALE     8     /* Resolution multiplier (1 = Amiga); halve stub_display WINDOW_SCALE when doubling so window size unchanged */
#define RENDER_SCALE_LOG2 3    /* log2(RENDER_SCALE); keep in sync when changing RENDER_SCALE (1->0, 2->1, 4->2, 8->3) */
#define RENDER_WIDTH         (96  * RENDER_SCALE)   /* Visible game columns */
#define RENDER_HEIGHT        (80  * RENDER_SCALE)   /* Visible game lines   */
#define RENDER_DEFAULT_WIDTH  RENDER_WIDTH          /* Default/resize reference width  */
#define RENDER_DEFAULT_HEIGHT RENDER_HEIGHT         /* Default/resize reference height */

/* Wall vertical texture: divisor so one world-space wall height maps to one texture repeat at any resolution. */
#define WALL_VERTICAL_TEX_DIVISOR  ((int32_t)PROJ_Y_SCALE * (int32_t)RENDER_SCALE)
#define RENDER_STRIDE    RENDER_WIDTH            /* Bytes per line (1 byte per pixel tag) */
#define RENDER_BUF_SIZE  (RENDER_STRIDE * RENDER_HEIGHT)
#define RENDER_STRIDE    RENDER_WIDTH   /* stride = width; runtime uses renderer width */
#define RENDER_BUF_SIZE  (RENDER_STRIDE * RENDER_HEIGHT)

/* Projection scales.
 *
 * PROJ_X_SCALE: horizontal projection (256 = Amiga default). The horizontal
 *   focal length is PROJ_X_SCALE / 2 (= 128). Do not change unless you want
 *   to alter horizontal FOV.
 *
 * PROJ_Y: vertical projection. PROJ_Y_NUMERATOR / PROJ_Y_DENOM gives base scale;
 *   the divide by PROJ_Y_DENOM is applied per frame with height scaling. */
#define PROJ_X_SCALE     256
#define PROJ_Y_NUMERATOR (256 * 11)
#define PROJ_Y_DENOM     24                /* adjust this value to change the vertical FOV, can need tweaking */
#define PROJ_Y_SCALE     (PROJ_Y_NUMERATOR / PROJ_Y_DENOM)

/* World Y fixed-point: zone/floor/object heights use this many fractional bits for projection. */
#define WORLD_Y_FRAC_BITS  8
#define WORLD_Y_SUBUNITS   (1 << WORLD_Y_FRAC_BITS)

/* Sprite size: (world * SPRITE_SIZE_SCALE / z) * SPRITE_SIZE_MULTIPLIER.
 * Amiga ObjDraw3: (byte<<7)/z = (byte*128)/z; we scale by RENDER_SCALE for resolution. No extra doubling. */
#define SPRITE_SIZE_SCALE      (128 * RENDER_SCALE)
#define SPRITE_SIZE_MULTIPLIER 1

/* -----------------------------------------------------------------------
 * Rotated point arrays
 *
 * RotateLevelPts writes to Rotated[] and OnScreen[].
 * RotateObjectPts writes to ObjRotated[].
 * ----------------------------------------------------------------------- */
#define MAX_POINTS       2048
#define MAX_OBJ_POINTS   256

typedef struct {
    int32_t x;       /* View-space X (fixed 16.16 or scaled) */
    int32_t z;       /* View-space Z (depth) */
} RotatedPoint;

typedef struct {
    int16_t screen_x; /* Screen column */
    int16_t flags;    /* Behind camera, etc. */
} OnScreenPoint;

typedef struct {
    int16_t x;       /* View-space X (16-bit) */
    int32_t z;       /* View-space Z (depth, 32-bit so far sprites scale) */
    int32_t x_fine;  /* View-space X (high precision for xwobble) */
} ObjRotatedPoint;

/* -----------------------------------------------------------------------
 * Per-column clipping table
 *
 * top/bot: vertical span of drawn wall in that column.
 * z: depth of that wall (view-space Z). When drawing sprites we only skip
 *    pixels in [top,bot] if sprite Z >= clip.z (sprite behind wall).
 * Allocated with width elements (renderer width).
 * ----------------------------------------------------------------------- */
typedef struct {
    int16_t *top;
    int16_t *bot;
    int32_t *z;   /* depth of wall in column; 0 = no wall */
} ColumnClip;

/* -----------------------------------------------------------------------
 * Wall texture table (walltiles)
 *
 * From WallChunk.s: 40 texture slots.
 * Each texture is 64x32 palette (2048 bytes) + 64x32 chunky (2048 bytes).
 * PaletteAddr = walltiles[id]
 * ChunkAddr   = walltiles[id] + 64*32
 * ----------------------------------------------------------------------- */
#define MAX_WALL_TILES  40
#define WALL_TEX_SIZE   (64 * 32 * 2)  /* palette + pixels */

/* -----------------------------------------------------------------------
 * Object/sprite graphics (Objects table from ObjDraw3.ChipRam.s)
 *
 * objVectNumber (object data offset 8) indexes this table.
 * Each slot can have a .wad (chunky pixels); we use 32x32 per frame.
 * ----------------------------------------------------------------------- */
#define MAX_SPRITE_TYPES  20
#define MAX_SPRITE_FRAMES 40  /* max frames per sprite type */

/* -----------------------------------------------------------------------
 * Renderer state
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Current framebuffer size (matches window when resizable) */
    int width;
    int height;

    /* Vertical projection scale: numerator / (PROJ_Y_DENOM * width/RENDER_DEFAULT_WIDTH) * height/RENDER_DEFAULT_HEIGHT, recomputed each frame. */
    int32_t proj_y_scale;

    /* Framebuffer (double-buffered) */
    uint8_t *buffer;          /* Current render target (width * height bytes) */
    uint8_t *back_buffer;     /* Back buffer for swap */

    /* 32-bit ARGB framebuffer (double-buffered).
     * Size: width * height * sizeof(uint32_t). */
    uint32_t *rgb_buffer;
    uint32_t *rgb_back_buffer;

    /* View transform */
    int16_t sinval, cosval;   /* Sin/cos of view angle */
    int16_t xoff, zoff;       /* Camera X/Z */
    int32_t yoff;             /* Camera Y (fixed) */
    int16_t wallyoff;         /* Wall texture Y offset */
    int16_t flooryoff;        /* Floor Y offset */
    int16_t xoff34, zoff34;   /* 3/4 offsets */
    int32_t xwobble;          /* Head bob X wobble */

    /* Rotated geometry */
    RotatedPoint  rotated[MAX_POINTS];
    OnScreenPoint on_screen[MAX_POINTS];
    ObjRotatedPoint obj_rotated[MAX_OBJ_POINTS];

    /* Column clipping */
    ColumnClip clip;

    /* Unused (Amiga uses no depth buffer; painter's + stream order only). */
    int16_t *depth_buffer;

    /* Zone rendering state */
    int16_t left_clip;
    int16_t right_clip;
    int16_t top_clip;
    int16_t bot_clip;
    /* Multi-floor: optional extra clip for walls only. -1 = use normal top_clip/bot_clip. */
    int16_t wall_top_clip;  /* lower room: 0 so walls can extend to top when very close */
    int16_t wall_bot_clip;  /* upper room: split_y so walls meet floor at split */

    /* Current room rendering state */
    int32_t top_of_room;
    int32_t bot_of_room;
    int32_t split_height;

    /* Wall texture table (from WallChunk.s walltiles) */
    const uint8_t *walltiles[MAX_WALL_TILES]; /* Pointers to pixel data (past 2048-byte LUT) */

    /* Object sprite graphics (from LoadFromDisk.s LoadObjects, ObjDraw3 Objects).
     *
     * Amiga format: .wad contains packed pixel data (3 five-bit pixels per
     * 16-bit word).  .ptr contains column pointer table (4 bytes per column:
     * byte 0 = which "third" 0/1/2, bytes 1-3 = offset into .wad).
     * .pal contains brightness-graded palette (15 levels × 32 colors × 2 bytes).
     *
     * sprite_wad[vect] = raw .wad pixel data
     * sprite_ptr[vect] = column pointer table (.ptr file)
     * sprite_pal_data[vect] = brightness palette (.pal file) */
    const uint8_t *sprite_wad[MAX_SPRITE_TYPES];
    size_t         sprite_wad_size[MAX_SPRITE_TYPES];
    const uint8_t *sprite_ptr[MAX_SPRITE_TYPES];
    size_t         sprite_ptr_size[MAX_SPRITE_TYPES];
    const uint8_t *sprite_pal_data[MAX_SPRITE_TYPES];
    size_t         sprite_pal_size[MAX_SPRITE_TYPES];

    /* Wall palette/LUT table.
     * Each entry points to the 2048-byte brightness LUT at the START of
     * the .wad file data.  Indexed as:
     *   color_word = lut[SCALE[d6] + texel5 * 2]  (big-endian 16-bit)
     * The word is a 12-bit Amiga color (0x0RGB). */
    const uint8_t *wall_palettes[MAX_WALL_TILES];

    /* Wall texture dimensions from loaded file (rows = 1<<wall_valshift[i], valand = rows-1).
     * Set by io_load_walls from actual file size; 0 means use level data. */
    uint8_t wall_valand[MAX_WALL_TILES];
    uint8_t wall_valshift[MAX_WALL_TILES];

    /* Current wall palette pointer (set per-wall in draw_zone) */
    const uint8_t *cur_wall_pal;

    /* Floor tile texture (from floortile - 256x256 sheet, 8-bit texels).
     * Individual 64x64 tiles are at floortile + whichtile offset. */
    const uint8_t *floor_tile;

    /* Floor brightness LUT (from FloorPalScaled - 15 levels * 512 bytes).
     * Maps texel -> brightness-scaled color. NULL if not loaded. */
    const uint8_t *floor_pal;

    /* Gun overlay (newgunsinhand.wad + .ptr + .pal). NULL if not loaded. */
    const uint8_t *gun_wad;   /* raw graphic data */
    const uint8_t *gun_ptr;   /* 96 columns × 4 bytes (mode + 24-bit offset per column) per frame */
    const uint8_t *gun_pal;   /* 32 × 16-bit 12-bit Amiga color (64 bytes) */
    size_t         gun_wad_size;

    /* Palette (legacy, used as fallback) */
    uint32_t palette[256];
} RendererState;

/* Global renderer instance */
extern RendererState g_renderer;

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

/* Initialize renderer (allocate buffers) */
void renderer_init(void);

/* Shutdown renderer (free buffers) */
void renderer_shutdown(void);

/* Resize framebuffer (call on window resize) */
void renderer_resize(int w, int h);

/* Clear the framebuffer to a color */
void renderer_clear(uint8_t color);

/* Swap front/back buffers */
void renderer_swap(void);

/* Main entry point: renders the full 3D scene to the chunky buffer.
 * Translated from AB3DI.s DrawDisplay.
 * After this call, g_renderer.buffer contains the rendered frame. */
void renderer_draw_display(GameState *state);

/* Sub-routines (called by draw_display) */
void renderer_rotate_level_pts(GameState *state);
void renderer_rotate_object_pts(GameState *state);
void renderer_draw_zone(GameState *state, int16_t zone_id, int use_upper);
void renderer_draw_wall(int16_t x1, int16_t z1, int16_t x2, int16_t z2,
                        int16_t top, int16_t bot,
                        const uint8_t *texture, int16_t tex_start,
                        int16_t tex_end, int16_t brightness,
                        uint8_t valand, uint8_t valshift, int16_t horand,
                        int16_t totalyoff, int16_t fromtile,
                        int16_t tex_id, int16_t wall_height_for_tex);
void renderer_draw_floor_span(int16_t y, int16_t x_left, int16_t x_right,
                              int32_t floor_height, const uint8_t *texture,
                              int16_t brightness, int is_water);
void renderer_draw_sprite(int16_t screen_x, int16_t screen_y,
                          int16_t width, int16_t height, int16_t z,
                          const uint8_t *wad, size_t wad_size,
                          const uint8_t *ptr_data, size_t ptr_size,
                          const uint8_t *pal, size_t pal_size,
                          uint32_t ptr_offset, uint16_t down_strip,
                          int src_cols, int src_rows,
                          int16_t brightness, int sprite_type,
                          int32_t clip_top_sy, int32_t clip_bot_sy);
void renderer_draw_gun(GameState *state);

/* Get pointer to the current rendered frame for display */
const uint8_t *renderer_get_buffer(void);
const uint32_t *renderer_get_rgb_buffer(void);
int renderer_get_width(void);
int renderer_get_height(void);
int renderer_get_stride(void);

#endif /* RENDERER_H */
