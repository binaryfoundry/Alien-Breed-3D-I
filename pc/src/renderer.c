/*
 * Alien Breed 3D I - PC Port
 * renderer.c - Software 3D renderer (chunky buffer)
 *
 * Translated from: AB3DI.s DrawDisplay (lines 3395-3693),
 *                  WallRoutine3.ChipMem.s, ObjDraw3.ChipRam.s
 *
 * The renderer draws into a flat 8-bit indexed pixel buffer.
 * The buffer is RENDER_STRIDE bytes wide and RENDER_HEIGHT lines tall.
 * Each byte is a palette index (0-31 for game colors).
 *
 * The rendering pipeline:
 *   1. Clear framebuffer
 *   2. Compute view transform (sin/cos from angpos)
 *   3. RotateLevelPts: transform level vertices to view space
 *   4. RotateObjectPts: transform object positions to view space
 *   5. For each zone (back-to-front from OrderZones):
 *      a. Set left/right clip from LEVELCLIPS
 *      b. Determine split (upper/lower room)
 *      c. DoThisRoom: iterate zone graph data, dispatch:
 *         - Walls  -> column-by-column textured drawing
 *         - Floors -> span-based textured drawing
 *         - Objects -> scaled sprite drawing
 *   6. Draw gun overlay
 *   7. Swap buffers
 */

#include "renderer.h"
#include "math_tables.h"
#include "game_data.h"
#include "game_types.h"
#include "visibility.h"
#include "stub_audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* Default world width/height per sprite graphic (vect_num), not object type.
 * So we size by which graphic we draw: barrel graphic (vect 7) always gets barrel size, etc.
 * Values from Amiga .s where type sets move.w #...,6(a0); reduced slightly for other aliens so they don’t look oversized. */
static const struct { int w; int h; } sprite_world_size_by_vect[MAX_SPRITE_TYPES] = {
    /*  0 alien       */ { 32,  32 },
    /*  1 pickups     */ { 40,  40 },   /* medikit, health (ammo overridden to 16×16 below) */
    /*  2 bigbullet   */ { 16,  16 },
    /*  3 (unused)    */ { 32,  32 },
    /*  4 flying      */ { 96,  96 },   /* FlyingScalyBall.s #$6060 */
    /*  5 keys        */ { 20,  20 },
    /*  6 rockets     */ { 20,  20 }, /* ammo pickup */
    /*  7 barrel      */ { 48,  50 },
    /*  8 explosion   */ { 64,  64 },
    /*  9 guns        */ { 40,  40 },
    /* 10 marine      */ { 32,  32 },   /* Robot.s #$2020; MutantMarine vect 10 */
    /* 11 bigalien    */ { 80,  80 },   /* BigUglyAlien / BIGSCARYALIEN */
    /* 12 lamps       */ { 40,  40 },
    /* 13 worm        */ { 90, 100 },   /* HalfWorm.s #90*256+100 */
    /* 14 bigclaws    */ {128, 128},   /* BigClaws.s #$8080 */
    /* 15 tree        */ {128, 128},   /* Tree.s #128*256+128; small red same vect */
    /* 16 tough marine*/ { 69,  69 }, /* ToughMarine.s #$4545 */
    /* 17 flame marine*/ { 69,  69 }, /* FlameMarine.s #$4545 */
    /* 18,19         */ { 32,  32 }, { 32,  32 },
};

/* Per-sprite "feet" anchor: source rows from bottom of graphic to the feet row.
 * 0 = feet at bottom of image (default). If the art has empty space below the feet,
 * set this so we place the sprite with that row on the floor (e.g. barrel ~8). */
static const int sprite_feet_rows_from_bottom_by_vect[MAX_SPRITE_TYPES] = {
    /*  0 alien       */ 0,
    /*  1 pickups     */ 4,   /* medikit, ammo etc – slight hover fix */
    /*  2 bigbullet   */ 0,
    /*  3 (unused)    */ 0,
    /*  4 flying      */ 0,
    /*  5 keys        */ 0,
    /*  6 rockets     */ 4,   /* ammo pickup – slight hover fix */
    /*  7 barrel      */ 4,
    /*  8 explosion   */ 0,
    /*  9 guns        */ 0,
    /* 10 marine      */ 0,
    /* 11 bigalien    */ 0,
    /* 12 lamps       */ 0,
    /* 13 worm        */ 0,
    /* 14 bigclaws    */ 0,
    /* 15 tree        */ 0,
    /* 16 tough marine*/ 0,
    /* 17 flame marine*/ 0,
    /* 18,19         */ 0, 0,
};

/* Floor/ceiling UV step: Amiga uses d1>>6 per pixel over 96 columns.
 * At RENDER_SCALE we have 96*RENDER_SCALE columns, so step must be (d1>>6)/RENDER_SCALE
 * to keep the same total U range. Integer: use shift 6 + RENDER_SCALE_LOG2. */
#define FLOOR_STEP_SHIFT  (6 + RENDER_SCALE_LOG2)
/* Extra pixels to extend floor/ceiling beyond polygon edge (0 = strict bounds; 1 = close sub-pixel gaps). */
#define FLOOR_EDGE_EXTRA  0
#define CEILING_EDGE_EXTRA 0
#define PORTAL_EDGE_EXTRA 0

/* Raise view height for rendering only (gameplay uses plr->yoff unchanged).
 * Makes the camera draw from higher so the floor appears further away, matching Amiga. */
#define VIEW_HEIGHT_LIFT  (2 * 1024)

/* -----------------------------------------------------------------------
 * Global renderer state
 * ----------------------------------------------------------------------- */
RendererState g_renderer;

/* -----------------------------------------------------------------------
 * Tunable: floor/ceiling texture camera-offset scale.
 *
 * Controls how much the floor texture scrolls when the camera moves.
 * The Amiga uses <<16 (=65536.0).
 * The ideal value is somewhere between <<14 (=16384.0) and <<16.
 *
 *   Too low  → floor texture drifts behind camera (slides under you)
 *   Too high → floor texture outruns camera (scrolls too fast)
 *
 * Adjust this until strafing produces no visible texture drift.
 * ----------------------------------------------------------------------- */
static float floor_cam_offset_scale = 16384.0f;  /* <<14 = 16384 */

/* -----------------------------------------------------------------------
 * Big-endian read helpers (level data is Amiga big-endian)
 * ----------------------------------------------------------------------- */
static inline int16_t rd16(const uint8_t *p) {
    return (int16_t)((p[0] << 8) | p[1]);
}
static inline int32_t rd32(const uint8_t *p) {
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* -----------------------------------------------------------------------
 * SCALE table (from Macros.i)
 *
 * Maps Amiga dimming index d6 (0-32) to byte offset into the per-texture
 * 2048-byte brightness LUT.  Each brightness block is 64 bytes (32 words).
 *   d6=0 → offset 0 (dimmest)
 *   d6=32 → offset 1024 (brightest for typical walls)
 * ----------------------------------------------------------------------- */
static const uint16_t wall_scale_table[33] = {
    64*0,  64*1,  64*1,  64*2,  64*2,  64*3,  64*3,  64*4,
    64*4,  64*5,  64*5,  64*6,  64*6,  64*7,  64*7,  64*8,
    64*8,  64*9,  64*9,  64*10, 64*10, 64*11, 64*11, 64*12,
    64*12, 64*13, 64*13, 64*14, 64*14, 64*15, 64*15, 64*16,
    64*16
};

/* Water animation: phase 0..255, advance every 2nd frame for a slow cycle. */
static uint32_t g_water_phase = 0;

/* -----------------------------------------------------------------------
 * Convert a 12-bit Amiga color word (0x0RGB) to ARGB8888.
 * ----------------------------------------------------------------------- */
static inline uint32_t amiga12_to_argb(uint16_t w)
{
    uint32_t r4 = (w >> 8) & 0xF;
    uint32_t g4 = (w >> 4) & 0xF;
    uint32_t b4 = w & 0xF;
    return 0xFF000000u | (r4 * 0x11u << 16) | (g4 * 0x11u << 8) | (b4 * 0x11u);
}

/* Sprite brightness → palette level mapping (from ObjDraw3.ChipRam.s objscalecols).
 * Raw brightness d6 (0..61+) maps to palette byte offset (64*level).
 * dcb.w 2,64*0 / dcb.w 4,64*1 / ... / dcb.w 20,64*14
 * Index clamped to 0..61; returns byte offset into .pal data. */
static const uint16_t obj_scale_cols[] = {
    64*0,  64*0,                                                   /* d6=0..1 */
    64*1,  64*1,  64*1,  64*1,                                     /* d6=2..5 */
    64*2,  64*2,  64*2,  64*2,                                     /* d6=6..9 */
    64*3,  64*3,  64*3,  64*3,                                     /* d6=10..13 */
    64*4,  64*4,  64*4,  64*4,                                     /* d6=14..17 */
    64*5,  64*5,  64*5,  64*5,                                     /* d6=18..21 */
    64*6,  64*6,  64*6,  64*6,                                     /* d6=22..25 */
    64*7,  64*7,  64*7,  64*7,                                     /* d6=26..29 */
    64*8,  64*8,  64*8,  64*8,                                     /* d6=30..33 */
    64*9,  64*9,  64*9,  64*9,                                     /* d6=34..37 */
    64*10, 64*10, 64*10, 64*10,                                    /* d6=38..41 */
    64*11, 64*11, 64*11, 64*11,                                    /* d6=42..45 */
    64*12, 64*12, 64*12, 64*12,                                    /* d6=46..49 */
    64*13, 64*13, 64*13, 64*13,                                    /* d6=50..53 */
    64*14, 64*14, 64*14, 64*14, 64*14, 64*14, 64*14, 64*14,       /* d6=54..61 */
    64*14, 64*14, 64*14, 64*14, 64*14, 64*14, 64*14, 64*14,
    64*14, 64*14, 64*14, 64*14
};
#define OBJ_SCALE_COLS_SIZE (sizeof(obj_scale_cols) / sizeof(obj_scale_cols[0]))
/* Minimum brightness index for sprites so they don't appear pitch-black (0..23 → very dark). */
#define SPRITE_BRIGHT_MIN 12

/* Gun ptr frame offsets (GUNS_FRAMES): 8 guns × 4 frames = 32 entries.
 * Each entry is byte offset into gun_ptr for that (gun, frame) column list. */
#define GUN_COLS 96
#define GUN_STRIDE (GUN_COLS * 4)
#define GUN_LINES 58
static const uint32_t gun_ptr_frame_offsets[32] = {
    GUN_STRIDE * 20, GUN_STRIDE * 21, GUN_STRIDE * 22, GUN_STRIDE * 23, /* gun 0 */
    GUN_STRIDE * 4,  GUN_STRIDE * 5,  GUN_STRIDE * 6,  GUN_STRIDE * 7,  /* gun 1 */
    GUN_STRIDE * 16, GUN_STRIDE * 17, GUN_STRIDE * 18, GUN_STRIDE * 19, /* gun 2 */
    GUN_STRIDE * 12, GUN_STRIDE * 13, GUN_STRIDE * 14, GUN_STRIDE * 15, /* gun 3 */
    GUN_STRIDE * 24, GUN_STRIDE * 25, GUN_STRIDE * 26, GUN_STRIDE * 27, /* gun 4 */
    0, 0, 0, 0, 0, 0, 0, 0,                                             /* guns 5,6 */
    GUN_STRIDE * 0,  GUN_STRIDE * 1,  GUN_STRIDE * 2,  GUN_STRIDE * 3,  /* gun 7 */
};

/* -----------------------------------------------------------------------
 * Initialization / Shutdown
 * ----------------------------------------------------------------------- */
void renderer_init(void)
{
    memset(&g_renderer, 0, sizeof(g_renderer));

    g_renderer.buffer = (uint8_t*)calloc(1, RENDER_BUF_SIZE);
    g_renderer.back_buffer = (uint8_t*)calloc(1, RENDER_BUF_SIZE);

    size_t rgb_size = (size_t)RENDER_WIDTH * RENDER_HEIGHT * sizeof(uint32_t);
    g_renderer.rgb_buffer = (uint32_t*)calloc(1, rgb_size);
    g_renderer.rgb_back_buffer = (uint32_t*)calloc(1, rgb_size);

    /* Per-pixel depth buffer for wall ordering */
    size_t depth_size = (size_t)RENDER_WIDTH * RENDER_HEIGHT * sizeof(int16_t);
    g_renderer.depth_buffer = (int16_t*)calloc(1, depth_size);

    /* Default clip region = full screen */
    g_renderer.top_clip = 0;
    g_renderer.bot_clip = RENDER_HEIGHT - 1;
    g_renderer.wall_top_clip = -1;
    g_renderer.wall_bot_clip = -1;
    g_renderer.left_clip = 0;
    g_renderer.right_clip = RENDER_WIDTH;

    printf("[RENDERER] Initialized: %dx%d, stride=%d, buffer=%zuB, rgb=%zuB, depth=%zuB\n",
           RENDER_WIDTH, RENDER_HEIGHT, RENDER_STRIDE,
           (size_t)RENDER_BUF_SIZE, rgb_size, depth_size);
}

void renderer_shutdown(void)
{
    free(g_renderer.buffer);
    free(g_renderer.back_buffer);
    free(g_renderer.rgb_buffer);
    free(g_renderer.rgb_back_buffer);
    g_renderer.buffer = NULL;
    g_renderer.back_buffer = NULL;
    g_renderer.rgb_buffer = NULL;
    g_renderer.rgb_back_buffer = NULL;
    printf("[RENDERER] Shutdown\n");
}

void renderer_clear(uint8_t color)
{
    if (g_renderer.buffer) {
        memset(g_renderer.buffer, color, RENDER_BUF_SIZE);
    }
    /* Clear rgb_buffer: sky above horizon, black below (floor/walls overwrite). */
    if (g_renderer.rgb_buffer) {
        uint32_t *p = g_renderer.rgb_buffer;
        int center = RENDER_HEIGHT / 2;
        /* Sky: off-white for now (Amiga had BackPicture / putinbackdrop). */
        const uint32_t sky = 0xFFEEEEEEu;  /* ARGB - a shade darker than white */
        for (int y = 0; y < center; y++) {
            for (int x = 0; x < RENDER_WIDTH; x++) {
                p[y * RENDER_WIDTH + x] = sky;
            }
        }
        for (int y = center; y < RENDER_HEIGHT; y++) {
            for (int x = 0; x < RENDER_WIDTH; x++) {
                p[y * RENDER_WIDTH + x] = 0xFF000000u;
            }
        }
    }
}

void renderer_swap(void)
{
    uint8_t *tmp = g_renderer.buffer;
    g_renderer.buffer = g_renderer.back_buffer;
    g_renderer.back_buffer = tmp;

    uint32_t *tmp2 = g_renderer.rgb_buffer;
    g_renderer.rgb_buffer = g_renderer.rgb_back_buffer;
    g_renderer.rgb_back_buffer = tmp2;
}

const uint8_t *renderer_get_buffer(void)
{
    return g_renderer.back_buffer; /* The just-drawn frame */
}

const uint32_t *renderer_get_rgb_buffer(void)
{
    return g_renderer.rgb_back_buffer; /* The just-drawn RGB frame */
}

int renderer_get_width(void)  { return RENDER_WIDTH; }
int renderer_get_height(void) { return RENDER_HEIGHT; }
int renderer_get_stride(void) { return RENDER_STRIDE; }

/* -----------------------------------------------------------------------
 * Pixel writing helpers
 * ----------------------------------------------------------------------- */
static inline void put_pixel(uint8_t *buf, int x, int y, uint8_t color)
{
    if (x >= 0 && x < RENDER_WIDTH && y >= 0 && y < RENDER_HEIGHT) {
        buf[y * RENDER_STRIDE + x] = color;
    }
}

static inline void draw_vline(uint8_t *buf, int x, int y_top, int y_bot,
                               uint8_t color)
{
    if (x < 0 || x >= RENDER_WIDTH) return;
    if (y_top < 0) y_top = 0;
    if (y_bot >= RENDER_HEIGHT) y_bot = RENDER_HEIGHT - 1;
    for (int y = y_top; y <= y_bot; y++) {
        buf[y * RENDER_STRIDE + x] = color;
    }
}

/* -----------------------------------------------------------------------
 * Rotate a single level point
 *
 * Translated from AB3DI.s RotateLevelPts (lines 4087-4166).
 *
 * For each point:
 *   dx = point.x - xoff
 *   dz = point.z - zoff
 *   rotated.x = dx * cos - dz * sin  (scaled, with xwobble added)
 *   rotated.z = dx * sin + dz * cos  (depth)
 *   on_screen.x = rotated.x / rotated.z  (perspective divide)
 * ----------------------------------------------------------------------- */
static void rotate_one_point(RendererState *r, const uint8_t *pts, int idx)
{
    int16_t sin_v = r->sinval;
    int16_t cos_v = r->cosval;
    int16_t cam_x = r->xoff;
    int16_t cam_z = r->zoff;

    int16_t px = rd16(pts + idx * 4);
    int16_t pz = rd16(pts + idx * 4 + 2);

    int16_t dx = (int16_t)(px - cam_x);
    int16_t dz = (int16_t)(pz - cam_z);

    /* Rotation (from ASM):
     * view_x = dx * cos - dz * sin    (d2 = d0*d6 - d1*d6_swapped)
     * view_z = dx * sin + dz * cos    (d1 = d0*d6_swapped + d1*d6) */
    int32_t vx = (int32_t)dx * cos_v - (int32_t)dz * sin_v;
    vx <<= 1;              /* add.l d2,d2 in ASM */
    int16_t vx16 = (int16_t)(vx >> 16);  /* swap d2 */
    int32_t vx_fine = (int32_t)vx16 << 7; /* asl.l #7 */
    vx_fine += r->xwobble;

    int32_t vz = (int32_t)dx * sin_v + (int32_t)dz * cos_v;
    vz <<= 2;              /* asl.l #2 in ASM */
    int16_t vz16 = (int16_t)(vz >> 16);  /* swap d1 */

    r->rotated[idx].x = vx_fine;
    r->rotated[idx].z = (int32_t)vz16;

    /* Project to screen column.
     * Amiga uses +47 as center (ASM line 4148: add.w #47,d2).
     * vx_fine already has <<7 (128x) baked in from the rotation above.
     * Scale by RENDER_WIDTH/96 so projection fills the doubled screen. */
    if (vz16 > 0) {
        int32_t screen_x = (vx_fine * RENDER_SCALE / vz16) + (RENDER_WIDTH / 2);
        r->on_screen[idx].screen_x = (int16_t)screen_x;
        r->on_screen[idx].flags = 0;
    } else {
        /* Behind camera */
        r->on_screen[idx].screen_x = (vx_fine > 0) ? RENDER_WIDTH + 100 : -100;
        r->on_screen[idx].flags = 1;
    }
}

/* -----------------------------------------------------------------------
 * RotateLevelPts
 *
 * Translated from AB3DI.s RotateLevelPts (lines 4087-4166).
 *
 * Uses PointsToRotatePtr: a list of 16-bit indices into the Points array,
 * terminated by a negative value. Only the listed points are transformed.
 * This list comes from the current zone data at offset ToZonePts (34).
 * ----------------------------------------------------------------------- */
#define TO_ZONE_PTS_OFFSET 34

void renderer_rotate_level_pts(GameState *state)
{
    RendererState *r = &g_renderer;

    if (!state->level.points) return;
    const uint8_t *pts = state->level.points;

    /* Get the PointsToRotatePtr from the current player's zone data.
     * It's stored as an offset from the room pointer into level data. */
    PlayerState *plr = (state->mode == MODE_SLAVE) ? &state->plr2 : &state->plr1;

    if (plr->roompt > 0 && state->level.data) {
        /* The zone data at roompt+34 (ToZonePts) contains a word offset.
         * Adding that to roompt gives the points-to-rotate list location. */
        const uint8_t *room = state->level.data + plr->roompt;
        int16_t ptr_off = rd16(room + TO_ZONE_PTS_OFFSET);
        const uint8_t *pt_list = room + ptr_off;

        /* Iterate the list of point indices */
        int safety = MAX_POINTS;
        while (safety-- > 0) {
            int16_t idx = rd16(pt_list);
            if (idx < 0) break;
            pt_list += 2;

            if (idx < MAX_POINTS) {
                rotate_one_point(r, pts, idx);
            }
        }
    } else {
        /* Fallback: no room data loaded yet - rotate all points */
        int num_pts = state->level.num_object_points;
        if (num_pts <= 0) return;
        if (num_pts > MAX_POINTS) num_pts = MAX_POINTS;

        for (int i = 0; i < num_pts; i++) {
            rotate_one_point(r, pts, i);
        }
    }
}

/* -----------------------------------------------------------------------
 * RotateObjectPts
 *
 * Translated from AB3DI.s RotateObjectPts (lines 4308-4362).
 *
 * Rotates object (enemy/pickup/bullet) positions from world space
 * to view space for sprite rendering.
 * ----------------------------------------------------------------------- */
void renderer_rotate_object_pts(GameState *state)
{
    RendererState *r = &g_renderer;

    if (!state->level.object_points || !state->level.object_data) return;

    int16_t sin_v = r->sinval;
    int16_t cos_v = r->cosval;
    int16_t cam_x = r->xoff;
    int16_t cam_z = r->zoff;

    int num_pts = state->level.num_object_points;
    if (num_pts <= 0) return;
    if (num_pts > MAX_OBJ_POINTS) num_pts = MAX_OBJ_POINTS;

    const uint8_t *pts = state->level.object_points;

    /* Amiga ObjDraw: ObjRotated is indexed by POINT number, not object index.
     * Rotate every point; when drawing, object uses (object_data[0]) as pt num
     * to look up ObjRotated[pt_num]. So keys (and others) scale correctly. */
    for (int pt = 0; pt < num_pts; pt++) {
        int16_t px = rd16(pts + pt * 8);
        int16_t pz = rd16(pts + pt * 8 + 4);

        int16_t dx = (int16_t)(px - cam_x);
        int16_t dz = (int16_t)(pz - cam_z);

        /* Same rotation as level points */
        int32_t vx = (int32_t)dx * cos_v - (int32_t)dz * sin_v;
        vx <<= 1;
        int16_t vx16 = (int16_t)(vx >> 16);

        int32_t vz = (int32_t)dx * sin_v + (int32_t)dz * cos_v;
        vz <<= 2;
        int16_t vz16 = (int16_t)(vz >> 16);  /* for divs-style use; keep 32-bit for size */

        int32_t vx_fine = (int32_t)vx16 << 7;
        vx_fine += r->xwobble;

        r->obj_rotated[pt].x = vx16;
        r->obj_rotated[pt].z = (int32_t)(vz >> 16);
        r->obj_rotated[pt].x_fine = vx_fine;
    }
}

/* -----------------------------------------------------------------------
 * Wall rendering (column-by-column)
 *
 * Translated from WallRoutine3.ChipMem.s ScreenWallstripdraw.
 *
 * Draws a vertical strip of a textured wall.
 * Parameters:
 *   x         - screen column
 *   y_top     - top of wall on screen
 *   y_bot     - bottom of wall on screen
 *   tex_col   - texture column to sample (0-127)
 *   texture   - pointer to wall pixel data (past 2048-byte LUT header)
 *   amiga_d6  - Amiga dimming index (0-32): 0=dimmest, 32=brightest
 *
 * Uses g_renderer.cur_wall_pal as the per-texture 2048-byte LUT.
 * ----------------------------------------------------------------------- */
static void draw_wall_column(int x, int y_top, int y_bot,
                             int tex_col, const uint8_t *texture,
                             int amiga_d6,
                             uint8_t valand, uint8_t valshift,
                             int16_t totalyoff, int32_t col_z,
                             int16_t wall_height_world)
{
    uint8_t *buf = g_renderer.buffer;
    uint32_t *rgb = g_renderer.rgb_buffer;
    if (!buf || !rgb) return;
    if (x < g_renderer.left_clip || x >= g_renderer.right_clip) return;

    /* Short walls (e.g. step risers) can project to same row; ensure at least 1 pixel height */
    int yt = y_top, yb = y_bot;
    if (yb <= yt) yb = yt + 1;

    /* Clip to screen. wall_top_clip/wall_bot_clip (multi-floor) let walls extend to meet ceiling/floor. */
    int effective_top = (g_renderer.wall_top_clip >= 0) ? (int)g_renderer.wall_top_clip : g_renderer.top_clip;
    int ct = (yt < effective_top) ? effective_top : yt;
    int effective_bot = (g_renderer.wall_bot_clip >= 0) ? (int)g_renderer.wall_bot_clip : g_renderer.bot_clip;
    int cb = (yb > effective_bot) ? effective_bot : yb;
    if (ct > cb) return;

    int wall_height = cb - ct;
    if (wall_height <= 0) return;

    /* Clamp d6 to SCALE table range */
    if (amiga_d6 < 0) amiga_d6 = 0;
    if (amiga_d6 > 32) amiga_d6 = 32;

    /* Get the brightness block offset from the SCALE table */
    uint16_t lut_block_off = wall_scale_table[amiga_d6];

    /* Pointer to the per-texture LUT (NULL if no palette loaded) */
    const uint8_t *pal = g_renderer.cur_wall_pal;

    /* --- Packed texture addressing ---
     *
     * Amiga wall textures pack 3 five-bit texels per 16-bit word:
     *   bits [4:0]   = texel A (PACK 0)
     *   bits [9:5]   = texel B (PACK 1)
     *   bits [14:10] = texel C (PACK 2)
     *
     * The texture data is arranged in vertical strips.  Each strip
     * covers 3 adjacent columns and has (1 << valshift) rows, with
     * 2 bytes per row (one 16-bit packed word).
     *
     *   strip_index  = tex_col / 3
     *   pack_mode    = tex_col % 3
     *   strip_offset = strip_index << (valshift + 1)
     *   row_word     = data[strip_offset + (ty & valand) * 2 .. +1]
     *
     * (Derived from WallRoutine3.ChipMem.s ScreenWallstripdraw)   */
    int strip_index = tex_col / 3;
    int pack_mode   = tex_col % 3;
    /* strip_offset = strip_index * 2 << valshift  =  strip_index << (valshift+1) */
    int strip_offset = strip_index << (valshift + 1);

    /* Texture step from world wall height. Repeats = wall_height_world/64.
     * Step maps (1<<valshift)*(h/64) texel rows over (y_bot-y_top) pixels.
     * For textures with fewer than 64 rows (e.g. switches 32 rows), use a minimum
     * h of 64 so one full texture repeat maps to the wall; otherwise a short wall
     * (e.g. h=32) would only show the middle band and cut off top/bottom. */
    int wall_pixels = yb - yt;
    if (wall_pixels < 1) wall_pixels = 1;
    int rows = 1 << valshift;
    int h = (int)wall_height_world;
    if (h < 1) h = 1;
    if (rows < 64 && h < 64) h = 64;  /* ensure full texture height visible on short walls */
    int32_t tex_step = (int32_t)(((int64_t)rows * (int64_t)h << 16) / (64 * wall_pixels));
    /* Amiga: totalyoff is added to row index then masked with VALAND (WallRoutine3 line 1803–1804).
     * So yoff is in texture row units and must wrap: use totalyoff & valand. */
    int32_t yoff = (int32_t)((unsigned)totalyoff & (unsigned)valand);
    int32_t tex_y = (ct - y_top) * tex_step + ((int32_t)yoff << 16);

    int16_t *depth = g_renderer.depth_buffer;
    int16_t col_z16 = (col_z > 32767) ? 32767 : (int16_t)col_z;

    for (int y = ct; y <= cb; y++) {
        int pix_idx = y * RENDER_WIDTH + x;

        /* Per-pixel depth test: only draw if closer than existing pixel */
        if (col_z16 >= depth[pix_idx]) {
            tex_y += tex_step;
            continue;
        }
        depth[pix_idx] = col_z16;

        /* Mask texture Y to wrap within texture height */
        int ty = (int)(tex_y >> 16) & valand;
        uint32_t argb;

        if (texture && pal) {
            /* Read the packed 16-bit word (big-endian) */
            int byte_off = strip_offset + ty * 2;
            uint16_t word = ((uint16_t)texture[byte_off] << 8)
                          | (uint16_t)texture[byte_off + 1];

            /* Unpack the 5-bit texel based on pack mode */
            uint8_t texel5;
            switch (pack_mode) {
            case 0:  texel5 = (uint8_t)(word & 31);         break;  /* bits [4:0]   */
            case 1:  texel5 = (uint8_t)((word >> 5) & 31);  break;  /* bits [9:5]   */
            default: texel5 = (uint8_t)((word >> 10) & 31); break;  /* bits [14:10] */
            }

            /* Look up 16-bit Amiga color word from the LUT.
             * LUT layout: 17 brightness blocks × 32 word entries.
             * Offset: block_offset + texel * 2 (big-endian word). */
            int lut_off = lut_block_off + texel5 * 2;
            uint16_t color_word = ((uint16_t)pal[lut_off] << 8) | pal[lut_off + 1];

            argb = amiga12_to_argb(color_word);
        } else {
            /* No texture or no LUT - fallback gray based on brightness */
            int gray = amiga_d6 * 255 / 32;
            argb = 0xFF000000u | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
        }

        buf[y * RENDER_STRIDE + x] = 2; /* tag: wall */
        rgb[pix_idx] = argb;
        tex_y += tex_step;
    }

    /* Update column clip (walls occlude floor/ceiling/sprites behind) */
    if (y_top > g_renderer.clip.top[x]) {
        g_renderer.clip.top[x] = (int16_t)y_top;
    }
    if (y_bot < g_renderer.clip.bot[x]) {
        g_renderer.clip.bot[x] = (int16_t)y_bot;
    }
}
/* -----------------------------------------------------------------------
 * Deferred wall for stream-order rendering.
 *
 * Walls are collected during stream parsing and drawn AFTER all
 * floor/ceiling entries.  This ensures floors draw on the black
 * framebuffer first, then walls overwrite on top -- matching the
 * original Amiga behaviour where floors fill the gaps left by walls.
 *
 * No depth sorting: walls are drawn in stream order (the level
 * compiler already arranges them correctly).
 * ----------------------------------------------------------------------- */
#define MAX_DEFERRED_WALLS 256
typedef struct {
    int16_t  x1, z1, x2, z2;
    int16_t  top, bot;
    const uint8_t *texture;
    int16_t  tex_start, tex_end;
    int16_t  brightness;
    uint8_t  valand, valshift;
    int16_t  horand;
    int16_t  tex_id;    /* for cur_wall_pal when drawing */
    int16_t  totalyoff; /* vertical texture offset */
    int16_t  fromtile;  /* horizontal texture offset (strip base) */
    int16_t  wall_height_for_tex; /* original level height for texture step (not overridden for doors) */
} DeferredWall;

/* Deferred water spans: draw after ALL zones' opaque geometry (global list, depth-sorted). */
#define MAX_DEFERRED_WATER_SPANS 4096
typedef struct {
    int16_t  y;
    int16_t  x_left, x_right;
    int32_t  floor_height;  /* rel_h for texture stepping */
    int16_t  depth;         /* for back-to-front sort (larger = further) */
    const uint8_t *texture;
    int16_t  brightness;
} DeferredWaterSpan;

/* Global deferred water (accumulated across all zones, drawn after zone loop). */
static DeferredWaterSpan s_deferred_water[MAX_DEFERRED_WATER_SPANS];
static int s_num_deferred_water;

/* -----------------------------------------------------------------------
 * Draw a wall segment between two rotated endpoints
 *
 * Translated from WallRoutine3.ChipMem.s walldraw/screendivide.
 *
 * Takes two endpoints in view space, projects them, and draws
 * columns from left to right with perspective-correct texturing.
 * ----------------------------------------------------------------------- */
/* Wall texture index for switches (stub_io wall_texture_table). */
#define SWITCHES_WALL_TEX_ID  11

void renderer_draw_wall(int16_t x1, int16_t z1, int16_t x2, int16_t z2,
                        int16_t top, int16_t bot,
                        const uint8_t *texture, int16_t tex_start,
                        int16_t tex_end, int16_t brightness,
                        uint8_t valand, uint8_t valshift, int16_t horand,
                        int16_t totalyoff, int16_t fromtile,
                        int16_t tex_id, int16_t wall_height_for_tex)
{
    RendererState *r = &g_renderer;

    /* Both behind camera - skip */
    if (z1 <= 0 && z2 <= 0) return;

    /* Switch walls: only draw when camera is on front of wall (one-sided). */
    if (tex_id == SWITCHES_WALL_TEX_ID) {
        int32_t cross = (int32_t)x1 * (int32_t)z2 - (int32_t)x2 * (int32_t)z1;
        if (cross >= 0) return;
    }

    /* Clip to near plane */
    int16_t cx1 = x1, cz1 = z1;
    int16_t cx2 = x2, cz2 = z2;
    int16_t ct1 = tex_start, ct2 = tex_end;

    /* Near plane distance - must be > 0 to avoid division issues.
     * Using a slightly larger value prevents overflow in tex/z calculations. */
    const int16_t NEAR_PLANE = 4;
    
    if (cz1 < NEAR_PLANE) {
        /* Clip left endpoint to near plane */
        int32_t dz = cz2 - cz1;
        if (dz == 0) { cz1 = NEAR_PLANE; cx1 = (int16_t)((cx1 + cx2) / 2); ct1 = (int16_t)((ct1 + ct2) / 2); }
        else {
            int32_t t = (NEAR_PLANE - cz1) * 65536 / dz;
            cx1 = (int16_t)(cx1 + (int32_t)(cx2 - cx1) * t / 65536);
            cz1 = NEAR_PLANE;
            ct1 = (int16_t)(ct1 + (int32_t)(ct2 - ct1) * t / 65536);
        }
    }
    if (cz2 < NEAR_PLANE) {
        int32_t dz = cz1 - cz2;
        if (dz == 0) { cz2 = NEAR_PLANE; cx2 = cx1; ct2 = ct1; }
        else {
            int32_t t = (NEAR_PLANE - cz2) * 65536 / dz;
            cx2 = (int16_t)(cx2 + (int32_t)(cx1 - cx2) * t / 65536);
            cz2 = NEAR_PLANE;
            ct2 = (int16_t)(ct2 + (int32_t)(ct1 - ct2) * t / 65536);
        }
    }

    /* Project to screen.
     * Amiga uses +47 as center offset (ASM: add.w #47,d2 in RotateLevelPts).
     * cx1/cx2 are rotated.x >> 7, so multiply back by PROJ_X_SCALE/2 (<<7) for the
     * perspective divide. Scale by RENDER_SCALE for doubled resolution. */
    int scr_x1 = (int)(((int32_t)cx1 * (PROJ_X_SCALE / 2) * RENDER_SCALE) / cz1) + (RENDER_WIDTH / 2);
    int scr_x2 = (int)(((int32_t)cx2 * (PROJ_X_SCALE / 2) * RENDER_SCALE) / cz2) + (RENDER_WIDTH / 2);

    /* If endpoints project in reverse order, swap them for left-to-right drawing.
     * This can happen after near-plane clipping. */
    if (scr_x1 > scr_x2) {
        int tmp;
        tmp = scr_x1; scr_x1 = scr_x2; scr_x2 = tmp;
        tmp = cz1; cz1 = (int16_t)cz2; cz2 = (int16_t)tmp;
        tmp = ct1; ct1 = (int16_t)ct2; ct2 = (int16_t)tmp;
    }

    if (scr_x2 < r->left_clip || scr_x1 >= r->right_clip) return;

    /* Ensure at least one column when both endpoints project to same pixel */
    int num_cols = scr_x2 - scr_x1;
    if (num_cols <= 0) {
        if (scr_x1 < r->left_clip || scr_x1 >= r->right_clip) return;
        num_cols = 1;
        scr_x2 = scr_x1 + 1;  /* so interpolation uses one column */
    }

    /* Precompute 1/z values for perspective-correct interpolation.
     * Use 64-bit intermediate to avoid overflow with very small z values. */
    int32_t inv_z1 = (int32_t)(65536LL / cz1);
    int32_t inv_z2 = (int32_t)(65536LL / cz2);
    
    /* Precompute tex/z for perspective-correct texture coordinate interpolation.
     * Use 16.16 scale (65536) so we keep sub-texel precision when the wall is far (large z);
     * with scale 256, ct*256/z quantizes badly at distance and horizontal resolution degrades. */
    int64_t tex_over_z1_64 = (int64_t)ct1 * 65536LL / cz1;
    int64_t tex_over_z2_64 = (int64_t)ct2 * 65536LL / cz2;
    
    /* Clamp to int32 range to prevent issues in interpolation */
    if (tex_over_z1_64 > INT32_MAX/2) tex_over_z1_64 = INT32_MAX/2;
    if (tex_over_z1_64 < INT32_MIN/2) tex_over_z1_64 = INT32_MIN/2;
    if (tex_over_z2_64 > INT32_MAX/2) tex_over_z2_64 = INT32_MAX/2;
    if (tex_over_z2_64 < INT32_MIN/2) tex_over_z2_64 = INT32_MIN/2;
    
    int32_t tex_over_z1 = (int32_t)tex_over_z1_64;
    int32_t tex_over_z2 = (int32_t)tex_over_z2_64;

    /* Draw columns left to right with perspective-correct interpolation */
    for (int col = 0; col < num_cols; col++) {
        int screen_x = scr_x1 + col;
        if (screen_x < r->left_clip || screen_x >= r->right_clip) continue;

        /* Interpolate in screen space (linear in 1/z) */
        int32_t t = (num_cols > 1) ? (col * 65536 / num_cols) : 0;
        
        /* Perspective-correct depth: interpolate 1/z, then invert */
        int32_t inv_z = inv_z1 + (int32_t)(inv_z2 - inv_z1) * t / 65536;
        if (inv_z <= 0) inv_z = 1;
        int32_t col_z = 65536 / inv_z;
        if (col_z < 1) col_z = 1;

        /* Project wall top/bottom at this depth.
         * Amiga ASM: screen_y = topofwall / z + 40.
         * top = (topwall - yoff) >> 8, so PROJ_Y_SCALE restores scale (matches Amiga 11:8 pixel aspect).
         * Scale by RENDER_HEIGHT/80 for doubled resolution. */
        int y_top = (int)((int32_t)top * PROJ_Y_SCALE * RENDER_SCALE / col_z) + (RENDER_HEIGHT / 2);
        int y_bot = (int)((int32_t)bot * PROJ_Y_SCALE * RENDER_SCALE / col_z) + (RENDER_HEIGHT / 2);

        /* Perspective-correct texture column: interpolate tex/z, then multiply by z.
         * Scale 65536 matches tex_over_z so result is world-space texture coord (integer). */
        int32_t tex_over_z = tex_over_z1 + (int32_t)((int64_t)(tex_over_z2 - tex_over_z1) * t / 65536);
        int32_t tex_t = (int32_t)((int64_t)tex_over_z * col_z / 65536);
        /* ASM line 167: and.w HORAND,d6 - mask first
         * ASM line 170: add.w fromtile(pc),d6 - then add horizontal offset */
        int tex_col = ((int)(tex_t) & horand) + fromtile;

        /* Distance-based brightness (Amiga convention).
         *
         * d6 = (z >> 7) + angbright, clamped to [0, 32].
         * SCALE[d6] maps to palette LUT block offsets.
         * d6=0 → dimmest, d6=32 → brightest (normal lit walls). */
        int amiga_d6 = brightness + (int)(col_z >> 7);
        if (amiga_d6 < 0) amiga_d6 = 0;
        if (amiga_d6 > 32) amiga_d6 = 32;

        /* Switch walls: depth bias so they draw in front of the wall behind them. */
        int32_t depth_z = col_z;
        if (tex_id == SWITCHES_WALL_TEX_ID && col_z > 16) depth_z = col_z - 16;

        draw_wall_column(screen_x, y_top, y_bot, tex_col, texture,
                         amiga_d6, valand, valshift, totalyoff, depth_z,
                         wall_height_for_tex);
    }
}

/* -----------------------------------------------------------------------
 * Floor/ceiling span rendering
 *
 * Translated from BumpMap.s / AB3DI.s itsafloordraw.
 *
 * Draws a horizontal span of floor or ceiling at a given height.
 * ----------------------------------------------------------------------- */
void renderer_draw_floor_span(int16_t y, int16_t x_left, int16_t x_right,
                              int32_t floor_height, const uint8_t *texture,
                              int16_t brightness, int is_water)
{
    /* Translated from AB3DI.s pastfloorbright (line 6657).
     *
     * Key insight: The Amiga packs UV into d5.w = ((V & 63) << 8) | (U & 63)
     * and samples texture at index d5.w * 4 = V*1024 + U*4.
     * This means the 256-byte-wide texture is sampled at every 4th texel.
     *
     * Step computation (per-pixel):
     *   d1 = dist * cosval (U step across screen)
     *   d2 = -dist * sinval (V step across screen)
     *
     * Starting position involves centering with 3/4 factors plus camera pos.
     */
    RendererState *rs = &g_renderer;
    uint8_t *buf = rs->buffer;
    uint32_t *rgb = rs->rgb_buffer;
    if (!buf || !rgb) return;
    if (y < 0 || y >= RENDER_HEIGHT) return;

    int xl = (x_left < rs->left_clip) ? rs->left_clip : x_left;
    int xr = (x_right >= rs->right_clip) ? rs->right_clip - 1 : x_right;
    if (xl > xr) return;

    int center = RENDER_HEIGHT / 2;  /* Match wall/floor projection center */
    int row_dist = y - center;
    if (row_dist == 0) row_dist = (y < center) ? -1 : 1;
    int abs_row_dist = (row_dist < 0) ? -row_dist : row_dist;

    /* Use same scale as walls for depth/brightness. */
    int32_t fh_8 = floor_height >> 8;
    int32_t dist;
    if (abs_row_dist <= 3) {
        dist = 32000;
    } else {
        dist = (int32_t)((int64_t)fh_8 * PROJ_Y_SCALE * RENDER_SCALE / row_dist);
        if (dist < 0) dist = -dist;
        if (dist < 16) dist = 16;
        if (dist > 30000) dist = 30000;
    }

    int amiga_d6 = brightness + (int)(dist >> 7);
    if (amiga_d6 < 0) amiga_d6 = 0;
    if (amiga_d6 > 32) amiga_d6 = 32;
    int gray = (32 - amiga_d6) * 255 / 32;

    /* ---- ASM pastfloorbright (line 6660) ----
     * d1 = d0 * cosval (change in U across whole width)
     * d2 = -(d0 * sinval) (change in V across whole width) */
    int32_t cos_v = rs->cosval;
    int32_t sin_v = rs->sinval;
    int32_t d1 = (int32_t)(((int64_t)dist * cos_v));
    int32_t d2 = (int32_t)(-((int64_t)dist * sin_v));

    /* ASM lines 6680-6695: Starting position centering.
     * Traced line-by-line from the original:
     *   d3 = d1 * 3/4
     *   d6 = d2 * 3/2, then d6 = d2 * 3/4
     *   d4 = -(d2 + d3) = start U
     *   d5 = d1 - d6 = start V */
    int32_t d3 = d1 + (d1 >> 1);      /* d3 = d1 * 3/2 (line 6684) */
    d3 >>= 1;                          /* d3 = d1 * 3/4 (line 6685) */

    int32_t d6 = (d2 >> 1) + d2;      /* d6 = d2/2 + d2 = d2 * 3/2 (lines 6689-6690) */
    int32_t start_u = -(d2 + d3);     /* d4 = -(d2 + d3) (lines 6691-6692) */
    d6 >>= 1;                          /* d6 = d2 * 3/4 (line 6694) */
    int32_t start_v = d1 - d6;         /* d5 = d1 - d6 (line 6695) */

    /* Add camera position (tunable scale - see floor_cam_offset_scale). */
    start_u += (int32_t)(rs->xoff * floor_cam_offset_scale);
    start_v += (int32_t)(rs->zoff * floor_cam_offset_scale);

    /* ASM lines 6700-6715: Offset by left edge.
     * Step per pixel = d1>>6 on Amiga (96 cols). We have RENDER_WIDTH cols so use
     * FLOOR_STEP_SHIFT so total U range matches (no skew at 2x res). */
    int32_t u_step = d1 >> FLOOR_STEP_SHIFT;
    int32_t v_step = d2 >> FLOOR_STEP_SHIFT;

    if (xl > 0) {
        start_u += ((int64_t)xl * d1) >> FLOOR_STEP_SHIFT;
        start_v += ((int64_t)xl * d2) >> FLOOR_STEP_SHIFT;
    }

    /* Current UV in 16.16 fixed point */
    int32_t u_fp = start_u;
    int32_t v_fp = start_v;

    uint8_t *row8 = buf + y * RENDER_STRIDE;
    uint32_t *row32 = rgb + y * RENDER_WIDTH;
    int16_t *depth = rs->depth_buffer + y * RENDER_WIDTH;
    
    /* Use actual distance for depth so floor/ceiling correctly occlude walls when
     * closer. Ceiling: slight bias (dist - 1) at boundary to avoid z-fighting. */
    int16_t floor_z;
    if (floor_height < 0) {
        int32_t ceiling_z = dist - 1;  /* bias so ceiling wins where wall meets ceiling */
        if (ceiling_z < 1) ceiling_z = 1;
        if (ceiling_z > 32767) ceiling_z = 32767;
        floor_z = (int16_t)ceiling_z;
    } else {
        int32_t d = dist;
        if (d < 1) d = 1;
        if (d > 32767) d = 32767;
        floor_z = (int16_t)d;
    }

    for (int x = xl; x <= xr; x++) {
        /* Per-pixel depth test: draw only if strictly closer (so bias is consistent) */
        if (floor_z >= depth[x]) {
            u_fp += u_step;
            v_fp += v_step;
            continue;
        }

        /* ASM lines 6721-6727: Extract 6-bit UV from high word.
         * swap d4: get integer part of U
         * asr.l #8,d5: shift V right
         * and.w #63,d4: U & 63
         * and.w #63*256,d5: (V >> 8) & 63, in position
         * move.b d4,d5: combine into d5.w = (V << 8) | U */
        int tu = (u_fp >> 16) & 63;
        int tv = (v_fp >> 16) & 63;

        /* Water: slow, smooth scroll only (no noisy ripple). */
        if (is_water) {
            /* Phase 0-255 -> scroll offset 0-63; advance slowly so animation is gentle */
            int scroll = (int)(g_water_phase >> 2) & 63;
            tu = (tu + scroll) & 63;
            tv = (tv + (scroll * 2) & 63) & 63;
        }

        /* ASM texture sampling: move.b (a0,d5.w*4),d0
         * d5.w = (V << 8) | U, so index = V*1024 + U*4
         * This samples every 4th texel in both directions. */
        uint32_t argb;

        if (texture) {
            /* ASM texture sampling: move.b (a0,d5.w*4),d0
             * d5.w = (V << 8) | U, so index = ((V << 8) | U) * 4
             * This samples from a 256-wide texture with 4-way interleaving. */
            int tex_idx = ((tv << 8) | tu) * 4;
            uint8_t texel = texture[tex_idx];

            if (rs->floor_pal) {
                /* Use FloorPalScaled brightness LUT.
                 * Water: use a uniform mid brightness so the whole surface is even (no dark/light bands). */
                int pal_level = amiga_d6 / 2;
                if (is_water) {
                    pal_level = 4;  /* bright water (LUT: low level = brighter) */
                }
                if (pal_level > 14) pal_level = 14;
                const uint8_t *lut = rs->floor_pal + pal_level * 512;
                uint16_t cw = (uint16_t)((lut[texel * 2] << 8) | lut[texel * 2 + 1]);
                argb = amiga12_to_argb(cw);
            } else {
                /* Fallback: use texel as grayscale with brightness */
                int lit = ((int)texel * gray) >> 8;
                if (is_water) {
                    lit = 200 + (int)(texel * 3 / 4);  /* bright water when no floor pal */
                    if (lit > 255) lit = 255;
                }
                argb = 0xFF000000u | ((uint32_t)lit << 16) | ((uint32_t)lit << 8) | (uint32_t)lit;
            }
        } else {
            /* No texture - solid gray based on brightness */
            argb = 0xFF000000u | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
        }

        /* Water: solid blue with light ripples; no dark bands (high base + min luminance). */
        if (is_water) {
            uint32_t r = 55, g = 130, b = 240;  /* raised base so no dark bands */
            if (texture) {
                int tex_idx = ((tv << 8) | tu) * 4;
                uint8_t texel = texture[tex_idx];
                uint32_t add = (uint32_t)texel * 120 >> 8;
                r += add;
                g += add;
                b += add;
            }
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            /* Min luminance so no dark bands from pattern */
            uint32_t lum = (r * 77 + g * 150 + b * 29) >> 8;
            if (lum > 0 && lum < 200) {
                uint32_t scale = (200 << 8) / lum;
                if (scale > 256) scale = 256;
                r = (r * scale) >> 8;
                g = (g * scale) >> 8;
                b = (b * scale) >> 8;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            argb = 0xFF000000u | (r << 16) | (g << 8) | b;
        }

        /* Water: blend with background. Refract (bump-map style) from pattern gradient. */
        if (is_water) {
            /* Pattern-based refraction: sample background at offset from texture gradient. */
            int refr_x = x, refr_y = y;
            if (texture) {
                int tu = (u_fp >> 16) & 63;
                int tv = (v_fp >> 16) & 63;
                int scroll = (int)(g_water_phase >> 2) & 63;
                tu = (tu + scroll) & 63;
                tv = (tv + (scroll * 2)) & 63;
                uint8_t t0 = texture[((tv << 8) | tu) * 4];
                uint8_t tu1 = texture[((tv << 8) | ((tu + 1) & 63)) * 4];
                uint8_t tv1 = texture[(((tv + 1) & 63) << 8 | tu) * 4];
                int dx = (int)(t0 - tu1) / 24;
                int dy = (int)(t0 - tv1) / 24;
                refr_x = x + dx;
                refr_y = y + dy;
            }
            refr_x = (refr_x < 0) ? 0 : (refr_x >= RENDER_WIDTH ? RENDER_WIDTH - 1 : refr_x);
            refr_y = (refr_y < 0) ? 0 : (refr_y >= RENDER_HEIGHT ? RENDER_HEIGHT - 1 : refr_y);
            uint32_t bg = rgb[refr_y * RENDER_WIDTH + refr_x];
            uint32_t br = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bb = bg & 0xFF;
            uint32_t wr = (argb >> 16) & 0xFF, wg = (argb >> 8) & 0xFF, wb = argb & 0xFF;
            /* ~55% water, 45% background = more transparent */
            uint32_t r = (wr * 141 + br * 115) >> 8;
            uint32_t g = (wg * 141 + bg_g * 115) >> 8;
            uint32_t b = (wb * 141 + bb * 115) >> 8;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            /* No dark bands: floor blended result so water stays bright */
            uint32_t lum = (r * 77 + g * 150 + b * 29) >> 8;
            if (lum > 0 && lum < 180) {
                uint32_t scale = (180 << 8) / lum;
                if (scale > 256) scale = 256;
                r = (r * scale) >> 8;
                g = (g * scale) >> 8;
                b = (b * scale) >> 8;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            argb = 0xFF000000u | (r << 16) | (g << 8) | b;
        }

        depth[x] = floor_z;
        row8[x] = 1;
        row32[x] = argb;
        u_fp += u_step;
        v_fp += v_step;
    }
}

/* -----------------------------------------------------------------------
 * Sprite rendering
 *
 * Translated from ObjDraw3.ChipRam.s BitMapObj.
 *
 * Amiga sprite WAD format: packed pixel data where each 16-bit word
 * encodes 3 five-bit pixels (bits 0-4, 5-9, 10-14).
 *
 * The .ptr file is a column pointer table: each column is 4 bytes:
 *   byte 0 = "third" (0, 1, or 2) = which 5-bit slice of each word
 *   bytes 1-3 = 24-bit offset into .wad data for this column
 *
 * The .pal file contains a brightness-graded palette:
 *   15 levels × 32 colors × 2 bytes (big-endian 12-bit Amiga color words).
 *   Level N starts at offset N * 64; color C at offset N * 64 + C * 2.
 *
 * Per-pixel decode (same as gun renderer):
 *   third 0: texel = low_byte & 0x1F
 *   third 1: texel = (word >> 5) & 0x1F
 *   third 2: texel = (high_byte >> 2) & 0x1F
 *
 * Uses painter's algorithm (drawn back-to-front by zone order).
 * ----------------------------------------------------------------------- */
void renderer_draw_sprite(int16_t screen_x, int16_t screen_y,
                          int16_t width, int16_t height, int16_t z,
                          const uint8_t *wad, size_t wad_size,
                          const uint8_t *ptr_data, size_t ptr_size,
                          const uint8_t *pal, size_t pal_size,
                          uint32_t ptr_offset, uint16_t down_strip,
                          int src_cols, int src_rows,
                          int16_t brightness)
{
    uint8_t *buf = g_renderer.buffer;
    uint32_t *rgb = g_renderer.rgb_buffer;
    int16_t *depth_buf = g_renderer.depth_buffer;
    if (!buf || !rgb || !depth_buf) return;
    if (z <= 0) return;
    if (!wad || !ptr_data) return;
    if (src_cols < 1) src_cols = 32;
    if (src_rows < 1) src_rows = 32;

    /* ASM: sub.w d3,d0 (left = center_x - half_w) before doubling d3.
     * ASM: sub.w d4,d2 (top = center_y - half_h) before doubling d4.
     * width/height passed in are already doubled (full size).
     * sx/sy are set after we may reduce to draw_w/draw_h for texture aspect. */
    int sx, sy;

    /* Brightness → palette byte offset via objscalecols (ObjDraw3.ChipRam.s line 572).
     * Raw d6 = (z>>7) + obj_bright is passed in as 'brightness'.
     * Apply a minimum so sprites stay visible (original often too dark on PC). */
    int bright_idx = brightness;
    if (bright_idx < 0) bright_idx = 0;
    if (bright_idx < SPRITE_BRIGHT_MIN) bright_idx = SPRITE_BRIGHT_MIN;
    if (bright_idx >= (int)OBJ_SCALE_COLS_SIZE) bright_idx = (int)OBJ_SCALE_COLS_SIZE - 1;
    uint32_t pal_level_off = obj_scale_cols[bright_idx];
    if (pal && pal_size < 960) pal_level_off = 0;  /* single-level or small palette */
    int gray = (bright_idx * 255) / 62;
    if (gray < 90) gray = 90;  /* floor so no-palette / fallback path stays visible */

    /* Sprite depth: bias in front so sprites on walls win z-fighting. */
    int16_t sprite_z = (z > 32767) ? 32767 : (int16_t)z;
    int16_t sprite_z_bias = (sprite_z > 16) ? (sprite_z - 16) : 1;

    /* Draw column-by-column (matching Amiga's column-strip approach).
     *
     * The PTR table has src_cols*2 columns per frame (confirmed by frame offset
     * spacing: e.g. alien frames are 64*4 bytes apart = 64 columns, src_cols=32).
     * Each column's WAD data has src_rows*2 words.
     *
     * Amiga blank-strip logic (line 763): "move.l (a5),d1 / beq blankstrip"
     * skips only when ALL 4 bytes of the PTR entry are zero (mode==0 AND wad_off==0).
     * wad_off==0 with mode!=0 is a VALID column (data at start of WAD). */
    int eff_cols = src_cols * 2;
    int eff_rows = src_rows * 2;
    uint32_t max_col = (ptr_size > ptr_offset) ? (ptr_size - ptr_offset) / 4u : 0;

    sx = screen_x - width / 2;
    sy = screen_y - height / 2;

    for (int dx = 0; dx < width; dx++) {
        int screen_col = sx + dx;
        if (screen_col < g_renderer.left_clip ||
            screen_col >= g_renderer.right_clip) continue;

        /* Map screen column to source column 0..eff_cols-1 */
        int src_col = (width > 1) ? (dx * eff_cols) / width : 0;
        if (src_col >= eff_cols) src_col = eff_cols - 1;
        /* Clamp to valid PTR range */
        if (max_col > 0 && (uint32_t)src_col >= max_col) src_col = (int)(max_col - 1);

        /* Read PTR entry for this source column */
        uint32_t entry_off = ptr_offset + (uint32_t)src_col * 4;
        if (entry_off + 4 > ptr_size) continue;

        const uint8_t *entry = ptr_data + entry_off;
        uint8_t mode = entry[0];
        uint32_t wad_off = ((uint32_t)entry[1] << 16)
                         | ((uint32_t)entry[2] << 8)
                         | (uint32_t)entry[3];

        /* Amiga: beq blankstrip - skip when entire 4-byte entry is zero */
        if (mode == 0 && wad_off == 0) continue;
        if (wad_off >= wad_size) continue;

        const uint8_t *src = wad + wad_off;

        for (int dy = 0; dy < height; dy++) {
            int screen_row = sy + dy;
            if (screen_row < 0 || screen_row >= RENDER_HEIGHT) continue;

            if (sprite_z_bias >= depth_buf[screen_row * RENDER_WIDTH + screen_col])
                continue;

            /* Map screen row to source row 0..eff_rows-1 */
            int src_row = (height > 1) ? (dy * eff_rows) / height : 0;
            if (src_row >= eff_rows) src_row = eff_rows - 1;
            int row_idx = (int)down_strip + src_row;

            /* Bounds check: each row is 2 bytes (one 16-bit word) */
            if (wad_off + (size_t)(row_idx + 1) * 2 > wad_size) continue;

            /* Decode 5-bit pixel from packed word (match gun: build 16-bit word big-endian). */
            uint16_t w = (uint16_t)((src[row_idx * 2] << 8) | src[row_idx * 2 + 1]);
            uint8_t texel = 0;
            if (mode == 0) {
                texel = (uint8_t)(w & 0x1F);
            } else if (mode == 1) {
                texel = (uint8_t)((w >> 5) & 0x1F);
            } else {
                texel = (uint8_t)((w >> 10) & 0x1F);
            }
            if (texel == 0) continue;  /* transparent */

            uint8_t *row8 = buf + screen_row * RENDER_STRIDE;
            uint32_t *row32 = rgb + screen_row * RENDER_WIDTH;
            int16_t *depth_row = depth_buf + screen_row * RENDER_WIDTH;

            depth_row[screen_col] = sprite_z_bias;
            row8[screen_col] = texel;

            /* Color from .pal brightness palette (15 levels × 64 bytes or single 64-byte block).
             * Amiga .pal is big-endian 12-bit words. Try little-endian if colors look wrong. */
            if (pal && pal_size >= 64) {
                uint32_t level_off = (pal_level_off + 64 <= pal_size) ? pal_level_off : 0;
                uint32_t ci = level_off + (uint32_t)texel * 2;
                if (ci + 1 < pal_size) {
                    uint16_t cw = (uint16_t)((pal[ci] << 8) | pal[ci + 1]);
                    row32[screen_col] = amiga12_to_argb(cw);
                } else {
                    int shade = (gray * (int)texel) / 31;
                    row32[screen_col] = 0xFF000000u
                        | ((uint32_t)shade << 16) | ((uint32_t)shade << 8) | (uint32_t)shade;
                }
            } else {
                /* No palette: use texel for shading so sprite shape is visible */
                int shade = (gray * (int)texel) / 31;
                row32[screen_col] = 0xFF000000u
                    | ((uint32_t)shade << 16) | ((uint32_t)shade << 8) | (uint32_t)shade;
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Draw gun overlay
 *
 * Translated from AB3DI.s DrawInGun (lines 2426-2535).
 * Amiga: gun graphic from Objects+9, GUNYOFFS=20, 3 chunks × 32 = 96 wide,
 * 78-GUNYOFFS = 58 lines tall. We draw a placeholder pistol until real
 * gun graphics are loaded; match approximate size and position.
 * ----------------------------------------------------------------------- */
void renderer_draw_gun(GameState *state)
{
    uint8_t *buf = g_renderer.buffer;
    uint32_t *rgb = g_renderer.rgb_buffer;
    if (!buf || !rgb) return;

    PlayerState *plr = (state->mode == MODE_SLAVE) ? &state->plr2 : &state->plr1;
    if (plr->gun_selected < 0) return;

    /* Amiga: 96 columns, 58 lines. Scale with RENDER_SCALE so gun matches view resolution. */
    const int gun_w_src = GUN_COLS;
    const int gun_h_src = GUN_LINES;
    const int gun_w_draw = gun_w_src * RENDER_SCALE;
    const int gun_h_draw = gun_h_src * RENDER_SCALE;
    int gy = RENDER_HEIGHT - gun_h_draw;
    if (gy < 0) gy = 0;
    int gx = (RENDER_WIDTH - gun_w_draw) / 2;

    /* Draw from loaded gun data (newgunsinhand.wad + .ptr + .pal) if present */
    const uint8_t *gun_wad = g_renderer.gun_wad;
    const uint8_t *gun_ptr = g_renderer.gun_ptr;
    const uint8_t *gun_pal = g_renderer.gun_pal;
    size_t gun_wad_size = g_renderer.gun_wad_size;

    if (gun_wad && gun_ptr && gun_pal && gun_wad_size > 0) {
        int gun_type = plr->gun_selected;
        if (gun_type >= 8) gun_type = 0;
        const GunAnim *anim = &gun_anims[gun_type];
        int anim_frame = plr->gun_frame;
        if (anim_frame > anim->num_frames) anim_frame = 0;
        int graphic_frame = anim->frames[anim_frame];
        if (graphic_frame > 3) graphic_frame = 0;
        uint32_t frame_slot = (uint32_t)(gun_type * 4 + graphic_frame);
        if (frame_slot >= 32) frame_slot = 0;
        uint32_t ptr_off = gun_ptr_frame_offsets[frame_slot];

        if (ptr_off != 0 || (gun_type != 5 && gun_type != 6)) {
            const uint8_t *frame_ptr = gun_ptr + ptr_off;
            /* Draw scaled: each screen pixel (sx,sy) samples source at (sx-gx)/RENDER_SCALE, (sy-gy)/RENDER_SCALE */
            for (int sy = gy; sy < gy + gun_h_draw && sy < RENDER_HEIGHT; sy++) {
                if (sy < 0) continue;
                int src_row = (sy - gy) / RENDER_SCALE;
                if (src_row >= gun_h_src) continue;
                for (int sx = gx; sx < gx + gun_w_draw && sx < RENDER_WIDTH; sx++) {
                    if (sx < 0) continue;
                    int src_col = (sx - gx) / RENDER_SCALE;
                    if (src_col >= gun_w_src) continue;

                    const uint8_t *col_ptr = gun_ptr + ptr_off + (uint32_t)src_col * 4;
                    uint8_t mode = col_ptr[0];
                    uint32_t wad_off = ((uint32_t)col_ptr[1] << 16) | ((uint32_t)col_ptr[2] << 8) | (uint32_t)col_ptr[3];
                    if (wad_off == 0 || wad_off >= gun_wad_size) continue;

                    const uint8_t *src = gun_wad + wad_off;
                    uint32_t idx = 0;
                    if (mode == 0) {
                        if (wad_off + (size_t)(src_row + 1) * 2 > gun_wad_size) continue;
                        uint16_t w = (uint16_t)((src[src_row * 2u] << 8) | src[src_row * 2u + 1]);
                        idx = (uint32_t)(w & 31u);
                    } else if (mode == 1) {
                        if (wad_off + (size_t)(src_row + 1) * 2 > gun_wad_size) continue;
                        uint16_t w = (uint16_t)((src[src_row * 2u] << 8) | src[src_row * 2u + 1]);
                        idx = (uint32_t)((w >> 5) & 31u);
                    } else {
                        if (wad_off + (size_t)src_row * 2u + 1 >= gun_wad_size) continue;
                        uint8_t b = src[src_row * 2u];
                        idx = (uint32_t)((b >> 2) & 31u);
                    }
                    if (idx == 0) continue;

                    uint16_t c12 = (uint16_t)((gun_pal[idx * 2u] << 8) | gun_pal[idx * 2u + 1]);
                    uint32_t c = amiga12_to_argb(c12);
                    buf[sy * RENDER_STRIDE + sx] = 15;
                    rgb[sy * RENDER_WIDTH + sx] = c;
                }
            }
            return;
        }
    }

    /* Placeholder when gun data not loaded or slot unused (narrower than WAD: 48 logical width) */
    const int placeholder_gun_w = 48 * RENDER_SCALE;
    int gx_ph = (RENDER_WIDTH - placeholder_gun_w) / 2;
    uint32_t col_barrel = 0xFF808080u;
    uint32_t col_body  = 0xFF606060u;
    uint32_t col_grip  = 0xFF404040u;
    const int mid_logical = 24;  /* 48/2 in source space */

    for (int y = gy; y < gy + gun_h_draw && y < RENDER_HEIGHT; y++) {
        if (y < 0) continue;
        int local_y = y - gy;
        int local_y_logical = local_y / RENDER_SCALE;
        for (int x = gx_ph; x < gx_ph + placeholder_gun_w && x < RENDER_WIDTH; x++) {
            if (x < 0) continue;
            int local_x = x - gx_ph;
            int local_x_logical = local_x / RENDER_SCALE;

            int draw = 0;
            uint32_t c = 0;

            if (local_y_logical < gun_h_src * 2 / 5) {
                if (local_x_logical >= mid_logical - 2 && local_x_logical <= mid_logical + 2) {
                    draw = 1; c = col_barrel;
                }
            } else if (local_y_logical < gun_h_src * 3 / 5) {
                if (local_x_logical >= mid_logical - 6 && local_x_logical <= mid_logical + 6) {
                    draw = 1; c = col_body;
                }
            } else {
                int w = 4 + (local_y_logical - gun_h_src * 3 / 5) / 4;
                if (w > 10) w = 10;
                if (local_x_logical >= mid_logical - w && local_x_logical <= mid_logical + w) {
                    draw = 1; c = col_grip;
                }
            }

            if (draw) {
                buf[y * RENDER_STRIDE + x] = 15;
                rgb[y * RENDER_WIDTH + x] = c;
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Sprite FRAMES tables (from ObjDraw3.ChipRam.s).
 *
 * Each frame entry is {ptr_offset, down_strip}:
 *   ptr_offset = byte offset into .ptr table for the starting column
 *   down_strip = vertical row offset within each column's data
 *
 * The Amiga stores these as dc.w pairs; ptr_offset values are in bytes
 * (column_index * 4 since each PTR entry is 4 bytes).
 * ----------------------------------------------------------------------- */
typedef struct { uint16_t ptr_off; uint16_t down_strip; } SpriteFrame;

/* Type 0 - ALIEN2: 64 cols/frame, walking 0-15, exploding 16-31, dying 32-33 */
static const SpriteFrame frames_alien[] = {
    {0,0},{64*4,0},{64*4*2,0},{64*4*3,0},{64*4*4,0},{64*4*5,0},{64*4*6,0},{64*4*7,0},
    {64*4*8,0},{64*4*9,0},{64*4*10,0},{64*4*11,0},{64*4*12,0},{64*4*13,0},{64*4*14,0},{64*4*15,0},
    {4*(64*16),0},{4*(64*16+16),0},{4*(64*16+32),0},{4*(64*16+48),0},
    {4*(64*16),16},{4*(64*16+16),16},{4*(64*16+32),16},{4*(64*16+48),16},
    {4*(64*16),32},{4*(64*16+16),32},{4*(64*16+32),32},{4*(64*16+48),32},
    {4*(64*16),48},{4*(64*16+16),48},{4*(64*16+32),48},{4*(64*16+48),48},
    {64*4*17,0},{64*4*18,0}
};

/* Type 1 - PICKUPS: variable layout */
static const SpriteFrame frames_pickups[] = {
    {0,0},                /* 0: medikit */
    {0,32},               /* 1: big gun */
    {64*4,32},            /* 2: bullet */
    {32*4,0},             /* 3: ammo */
    {64*4,0},             /* 4: battery */
    {192*4,0},            /* 5: rockets */
    {128*4,0},{(128+16)*4,0},{(128+32)*4,0},{(128+48)*4,0},   /* 6-9: gunpop */
    {128*4,16},{(128+16)*4,16},{(128+32)*4,16},{(128+48)*4,16}, /* 10-13 */
    {128*4,32},{(128+16)*4,32},{(128+32)*4,32},{(64+16)*4,32}, /* 14-17 */
    {64*4,48},{(64+16)*4,48},                                   /* 18-19 */
    {(64+32)*4,0},        /* 20: rocket launcher */
    {64*4,32},{(64+16)*4,32},{(64+16)*4,48},{64*4,48},          /* 21-24: grenade */
    {128*4,32},           /* 25: shotgun */
    {256*4,0},            /* 26: grenade launcher */
    {64*3*4,32},          /* 27: shotgun shells*4 */
    {(64*3+32)*4,0},      /* 28: shotgun shells*20 */
    {(64*3+32)*4,32}      /* 29: grenade clip */
};

/* Type 2 - BIGBULLET */
static const SpriteFrame frames_bigbullet[] = {
    {0,0},{0,32},{32*4,0},{32*4,32},{64*4,0},{64*4,32},{96*4,0},{96*4,32},
    {128*4,0},{128*4,32},{32*5*4,0},{32*5*4,32},{32*6*4,0},{32*6*4,32},
    {32*7*4,0},{32*7*4,32},{32*8*4,0},{32*8*4,32},{32*9*4,0},{32*9*4,32}
};

/* Type 4 - FLYINGMONSTER: 64 cols/frame, 21 frames */
static const SpriteFrame frames_flying[] = {
    {0,0},{64*4,0},{64*4*2,0},{64*4*3,0},{64*4*4,0},{64*4*5,0},{64*4*6,0},{64*4*7,0},
    {64*4*8,0},{64*4*9,0},{64*4*10,0},{64*4*11,0},{64*4*12,0},{64*4*13,0},{64*4*14,0},{64*4*15,0},
    {64*4*16,0},{64*4*17,0},{64*4*18,0},{64*4*19,0},{64*4*20,0}
};

/* Type 5 - KEYS: 4 frames */
static const SpriteFrame frames_keys[] = {
    {0,0},{0,32},{32*4,0},{32*4,32}
};

/* Type 6 - ROCKETS: 12 frames */
static const SpriteFrame frames_rockets[] = {
    {0,0},{32*4,0},{0,32},{32*4,32},
    {64*4,0},{(64+32)*4,0},{64*4,32},{(64+32)*4,32},
    {128*4,0},{(128+32)*4,0},{128*4,32},{(128+32)*4,32}
};

/* Type 7 - BARREL: 1 frame */
static const SpriteFrame frames_barrel[] = { {0,0} };

/* Type 8 - EXPLOSION: 9 frames (uses explosion.wad/ptr, not bigbullet) */
static const SpriteFrame frames_explosion[] = {
    {0,0},{64*4,0},{64*4*2,0},{64*4*3,0},{64*4*4,0},{64*4*5,0},{64*4*6,0},{64*4*7,0},{64*4*8,0}
};

/* Type 9 - GUNS (in-hand): handled by gun renderer, but provide frames */
static const SpriteFrame frames_guns[] = {
    {96*4*20,0},{96*4*21,0},{96*4*22,0},{96*4*23,0},
    {96*4*4,0},{96*4*5,0},{96*4*6,0},{96*4*7,0},
    {96*4*16,0},{96*4*17,0},{96*4*18,0},{96*4*19,0},
    {96*4*12,0},{96*4*13,0},{96*4*14,0},{96*4*15,0},
    {96*4*24,0},{96*4*25,0},{96*4*26,0},{96*4*27,0},
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    {96*4*0,0},{96*4*1,0},{96*4*2,0},{96*4*3,0}
};

/* Type 10 - MARINE: 19 frames, 64 cols each */
static const SpriteFrame frames_marine[] = {
    {0,0},{64*4,0},{(64*2)*4,0},{(64*3)*4,0},{(64*4)*4,0},{(64*5)*4,0},
    {(64*6)*4,0},{(64*7)*4,0},{(64*8)*4,0},{(64*9)*4,0},{(64*10)*4,0},
    {(64*11)*4,0},{(64*12)*4,0},{(64*13)*4,0},{(64*14)*4,0},{(64*15)*4,0},
    {(64*16)*4,0},{(64*17)*4,0},{(64*18)*4,0}
};

/* Type 11 - BIGALIEN: 4 frames, 128 cols each */
static const SpriteFrame frames_bigalien[] = {
    {0,0},{128*4,0},{128*4*2,0},{128*4*3,0}
};

/* Type 12 - LAMPS: 1 frame */
static const SpriteFrame frames_lamps[] = { {0,0} };

/* Type 13 - WORM: 21 frames, 90 cols each */
static const SpriteFrame frames_worm[] = {
    {0,0},{90*4,0},{90*4*2,0},{90*4*3,0},{90*4*4,0},{90*4*5,0},{90*4*6,0},{90*4*7,0},
    {90*4*8,0},{90*4*9,0},{90*4*10,0},{90*4*11,0},{90*4*12,0},{90*4*13,0},{90*4*14,0},{90*4*15,0},
    {90*4*16,0},{90*4*17,0},{90*4*18,0},{90*4*19,0},{90*4*20,0}
};

/* Type 14 - BIGCLAWS: 18 frames, 128 cols each */
static const SpriteFrame frames_bigclaws[] = {
    {0,0},{128*4,0},{128*4*2,0},{128*4*3,0},{128*4*4,0},{128*4*5,0},{128*4*6,0},{128*4*7,0},
    {128*4*8,0},{128*4*9,0},{128*4*10,0},{128*4*11,0},{128*4*12,0},{128*4*13,0},{128*4*14,0},{128*4*15,0},
    {128*4*16,0},{128*4*17,0}
};

/* Type 15 - TREE: 22 frames, 64 cols each (with repeats) */
static const SpriteFrame frames_tree[] = {
    {0,0},{64*4,0},{64*2*4,0},{64*3*4,0},
    {0,0},{64*4,0},{64*2*4,0},{64*3*4,0},
    {0,0},{64*4,0},{64*2*4,0},{64*3*4,0},
    {0,0},{64*4,0},{64*2*4,0},{64*3*4,0},
    {0,0},{0,0},
    {32*8*4,0},{32*9*4,0},{32*10*4,0},{32*11*4,0}
};

/* Master lookup: frames table + count per sprite type */
static const struct {
    const SpriteFrame *frames;
    int count;
} sprite_frames_table[MAX_SPRITE_TYPES] = {
    { frames_alien,     sizeof(frames_alien)/sizeof(SpriteFrame) },       /* 0 */
    { frames_pickups,   sizeof(frames_pickups)/sizeof(SpriteFrame) },     /* 1 */
    { frames_bigbullet, sizeof(frames_bigbullet)/sizeof(SpriteFrame) },   /* 2 */
    { NULL, 0 },                                                          /* 3 ugly monster */
    { frames_flying,    sizeof(frames_flying)/sizeof(SpriteFrame) },      /* 4 */
    { frames_keys,      sizeof(frames_keys)/sizeof(SpriteFrame) },        /* 5 */
    { frames_rockets,   sizeof(frames_rockets)/sizeof(SpriteFrame) },     /* 6 */
    { frames_barrel,    sizeof(frames_barrel)/sizeof(SpriteFrame) },      /* 7 */
    { frames_explosion, sizeof(frames_explosion)/sizeof(SpriteFrame) },   /* 8 */
    { frames_guns,      sizeof(frames_guns)/sizeof(SpriteFrame) },        /* 9 */
    { frames_marine,    sizeof(frames_marine)/sizeof(SpriteFrame) },      /* 10 */
    { frames_bigalien,  sizeof(frames_bigalien)/sizeof(SpriteFrame) },    /* 11 */
    { frames_lamps,     sizeof(frames_lamps)/sizeof(SpriteFrame) },       /* 12 */
    { frames_worm,      sizeof(frames_worm)/sizeof(SpriteFrame) },        /* 13 */
    { frames_bigclaws,  sizeof(frames_bigclaws)/sizeof(SpriteFrame) },    /* 14 */
    { frames_tree,      sizeof(frames_tree)/sizeof(SpriteFrame) },        /* 15 */
    { frames_marine,    sizeof(frames_marine)/sizeof(SpriteFrame) },      /* 16 tough (same as 10) */
    { frames_marine,    sizeof(frames_marine)/sizeof(SpriteFrame) },      /* 17 flame (same as 10) */
    { NULL, 0 }, { NULL, 0 }
};

/* -----------------------------------------------------------------------
 * Draw objects in the current zone
 *
 * Translated from ObjDraw3.ChipRam.s ObjDraw (lines 38-137).
 *
 * Iterates all objects in ObjectData, finds those in the current zone,
 * sorts by depth, and draws them back-to-front.
 * ----------------------------------------------------------------------- */
static void draw_zone_objects(GameState *state, int16_t zone_id,
                              int32_t top_of_room, int32_t bot_of_room)
{
    RendererState *r = &g_renderer;
    LevelState *level = &state->level;
    if (!level->object_data || !level->object_points) return;

    int32_t y_off = r->yoff;

    /* Build depth-sorted list of objects in this zone */
    typedef struct { int idx; int32_t z; } ObjEntry;
    ObjEntry objs[80];
    int obj_count = 0;

    int num_pts = level->num_object_points;
    if (num_pts > MAX_OBJ_POINTS) num_pts = MAX_OBJ_POINTS;

    /* Iterate by object index; each object has point number at offset 0 (ObjDraw: move.w (a0)+,d0).
     * Use that to look up ObjRotated[pt_num] so keys and other pickups use correct position/z. */
    for (int obj_idx = 0; obj_idx < 80 && obj_count < 80; obj_idx++) {
        const uint8_t *obj = level->object_data + obj_idx * OBJECT_SIZE;
        int16_t pt_num = rd16(obj);
        if (pt_num < 0) break; /* End of object list */

        if ((unsigned)pt_num >= (unsigned)num_pts) continue; /* invalid point number */

        /* Which zone draws this object */
        int16_t graphic_room = rd16(obj + 26);
        int16_t obj_zone = rd16(obj + 12);
        int in_this_zone = (graphic_room >= 0 && graphic_room == (int16_t)zone_id)
                           || (obj_zone >= 0 && obj_zone == (int16_t)zone_id);
        if (!in_this_zone) continue;

        ObjRotatedPoint *orp = &r->obj_rotated[pt_num];
        if (orp->z <= 0) continue; /* Behind camera */

        objs[obj_count].idx = obj_idx;
        objs[obj_count].z = orp->z;
        obj_count++;
    }

    /* Sort by Z (farthest first - painter's algorithm) */
    for (int i = 0; i < obj_count - 1; i++) {
        for (int j = i + 1; j < obj_count; j++) {
            if (objs[j].z > objs[i].z) {
                ObjEntry tmp = objs[i];
                objs[i] = objs[j];
                objs[j] = tmp;
            }
        }
    }

    /* Draw each object (back-to-front, painter's algorithm)
     * Translated from ObjDraw3.ChipRam.s BitMapObj (lines 527-626).
     *
     * For each object, the data layout (from Defs.i):
     *   Offset 0:  object type (word)
     *   Offset 2:  brightness (objVectBright, word)
     *   Offset 4:  Y position (word, in ObjectPoints offset 2)
     *   Offset 6:  world width (byte), offset 7: world height (byte)
     *   Offset 8:  vector number (objVectNumber, word)
     *   Offset 10: frame number (objVectFrameNumber, word)
     *   Offset 14: source columns (byte), offset 15: source rows (byte)
     *   Offset 26: GraphicRoom (word)
     */
    for (int oi = 0; oi < obj_count; oi++) {
        int obj_idx = objs[oi].idx;
        const uint8_t *obj = level->object_data + obj_idx * OBJECT_SIZE;
        /* ObjDraw3: cmp.b #$ff,6(a0); bne BitMapObj → 3D/polygon objects have obj[6]==OBJ_3D_SPRITE. Skip for now. */
        if ((uint8_t)obj[6] == (uint8_t)OBJ_3D_SPRITE) continue;

        int16_t pt_num = rd16(obj);
        if ((unsigned)pt_num >= (unsigned)num_pts) continue;
        ObjRotatedPoint *orp = &r->obj_rotated[pt_num];

        /* Use actual view Z for size so sprites scale at all distances. Guard only vs div-by-zero. */
        int32_t z_for_size = orp->z;
        if (z_for_size < 1) z_for_size = 1;

        /* Project Y boundaries from room top/bottom (same PROJ_Y_SCALE as walls). */
        int32_t clip_top_y = (int)((int64_t)((top_of_room - y_off) >> 8) * PROJ_Y_SCALE * RENDER_SCALE / orp->z) + (RENDER_HEIGHT / 2);
        int32_t clip_bot_y = (int)((int64_t)((bot_of_room - y_off) >> 8) * PROJ_Y_SCALE * RENDER_SCALE / orp->z) + (RENDER_HEIGHT / 2);
        if (clip_top_y >= clip_bot_y) continue;

        /* Project to screen X (PROJ_X_SCALE/2 = horizontal focal length). */
        int32_t obj_vx_fine = orp->x_fine;
        int scr_x = (int)(obj_vx_fine * RENDER_SCALE / (int32_t)orp->z) + (RENDER_WIDTH / 2);

        /* Get brightness + distance attenuation
         * ASM: asr.w #7,d6 ; add.w (a0)+,d6 (distance>>7 + obj brightness) */
        /* Raw brightness d6 = (z >> 7) + objVectBright.
         * Passed to renderer_draw_sprite which uses objscalecols to map to palette level. */
        int16_t obj_bright = rd16(obj + 2);  /* objVectBright */
        int bright = (orp->z >> 7) + obj_bright;
        if (bright < 0) bright = 0;

        int8_t obj_number = (int8_t)obj[16];
        if (obj_number == OBJ_NBR_DEAD) continue;

        /* Resolve sprite graphic (vect_num). Fallback by obj_number when vect missing/invalid
         * or when level left vect 0 for a non-alien (so all enemies don't draw as alien). */
        int16_t vect_num = rd16(obj + 8);
        int16_t frame_num = rd16(obj + 10);
        int use_fallback = (vect_num < 0 || vect_num >= MAX_SPRITE_TYPES ||
                            !r->sprite_wad[vect_num] || !r->sprite_ptr[vect_num]);
        if (!use_fallback && vect_num == 0 && obj_number != OBJ_NBR_ALIEN &&
            obj_number >= OBJ_NBR_ROBOT && obj_number <= OBJ_NBR_FLAME_MARINE)
            use_fallback = 1;
        if (use_fallback) {
            switch ((ObjNumber)obj_number) {
                case OBJ_NBR_ALIEN:           vect_num = 0;  break;
                case OBJ_NBR_MEDIKIT:         vect_num = 1;  break;
                case OBJ_NBR_BULLET:          vect_num = 2;  break;
                case OBJ_NBR_BIG_GUN:         vect_num = 9;  break;
                case OBJ_NBR_KEY:             vect_num = 5;  break;
                case OBJ_NBR_PLR1:
                case OBJ_NBR_PLR2:
                case OBJ_NBR_MARINE:          vect_num = 10; break;
                case OBJ_NBR_ROBOT:           vect_num = 10; break;
                case OBJ_NBR_BIG_NASTY:       vect_num = 11; break;
                case OBJ_NBR_FLYING_NASTY:    vect_num = 4;  break;
                case OBJ_NBR_AMMO:            vect_num = 6;  break;
                case OBJ_NBR_BARREL:          vect_num = 7;  break;
                case OBJ_NBR_WORM:            vect_num = 13; break;
                case OBJ_NBR_HUGE_RED_THING:  vect_num = 14; break;
                case OBJ_NBR_SMALL_RED_THING: vect_num = 15; break;
                case OBJ_NBR_TREE:            vect_num = 15; break;
                case OBJ_NBR_EYEBALL:         vect_num = 0;  break;
                case OBJ_NBR_TOUGH_MARINE:    vect_num = 16; break;
                case OBJ_NBR_FLAME_MARINE:    vect_num = 17; break;
                default:                      vect_num = 0;  break;
            }
            frame_num = 0;
        }
        if (vect_num < 0 || vect_num >= MAX_SPRITE_TYPES) continue;

        /* World size from table by vect_num. Ammo pickup often uses vect 1 (pickups) from level
         * data, so use a smaller size when we know it's ammo to avoid huge ammo sprites. */
        int v = vect_num;
        int world_w = sprite_world_size_by_vect[v].w;
        int world_h = sprite_world_size_by_vect[v].h;
        if (v == 1 && obj_number == OBJ_NBR_AMMO) {
            world_w = 16;
            world_h = 16;
        }

        /* Screen size: (world * SPRITE_SIZE_SCALE / z) * SPRITE_SIZE_MULTIPLIER. Do not clamp
         * to RENDER_WIDTH/HEIGHT: that stops scaling when very close. Let size grow; draw loop clips to screen. */
        int sprite_w = (int)((int32_t)world_w * SPRITE_SIZE_SCALE / z_for_size) * SPRITE_SIZE_MULTIPLIER;
        int sprite_h = (int)((int32_t)world_h * SPRITE_SIZE_SCALE / z_for_size) * SPRITE_SIZE_MULTIPLIER;
        if (sprite_w < 1) sprite_w = 1;
        if (sprite_h < 1) sprite_h = 1;

        /* Source dimensions: columns and rows from object data offsets 14, 15. */
        int src_cols = (int)obj[14];
        int src_rows = (int)obj[15];
        if (src_cols < 1) src_cols = 32;
        if (src_rows < 1) src_rows = 32;

        /* Sprite Y: place feet on zone floor. Use per-vect feet anchor (rows from bottom of graphic). */
        int32_t floor_rel = (int32_t)(bot_of_room - y_off);
        int floor_screen_y = (int)((int64_t)(floor_rel >> 8) * PROJ_Y_SCALE * (int32_t)RENDER_SCALE / orp->z) + (RENDER_HEIGHT / 2);
        int half_h = sprite_h / 2;
        int feet_rows = sprite_feet_rows_from_bottom_by_vect[vect_num];
        int feet_screen = (feet_rows > 0 && src_rows > 0) ? (feet_rows * sprite_h) / src_rows : 0;
        int scr_y = floor_screen_y - half_h + feet_screen;

        /* Look up frame info from FRAMES table */
        uint32_t ptr_off = 0;
        uint16_t down_strip = 0;
        const SpriteFrame *ft = sprite_frames_table[vect_num].frames;
        int ft_count = sprite_frames_table[vect_num].count;
        if (ft && frame_num >= 0 && frame_num < ft_count) {
            ptr_off = ft[frame_num].ptr_off;
            down_strip = ft[frame_num].down_strip;
        }

        /* Use dedicated .pal if loaded; no fallback to WAD header because
         * sprite .pal format (15 levels × 32 × 2 bytes = 960) differs from
         * the wall LUT format in the WAD header (17 blocks × 32 × 2 = 2048). */
        const uint8_t *obj_pal = r->sprite_pal_data[vect_num];
        size_t obj_pal_size = r->sprite_pal_size[vect_num];

        renderer_draw_sprite((int16_t)scr_x, (int16_t)scr_y,
                             (int16_t)sprite_w, (int16_t)sprite_h,
                             (int16_t)(orp->z > 32767 ? 32767 : orp->z),
                             r->sprite_wad[vect_num], r->sprite_wad_size[vect_num],
                             r->sprite_ptr[vect_num], r->sprite_ptr_size[vect_num],
                             obj_pal, obj_pal_size,
                             ptr_off, down_strip,
                             src_cols, src_rows,
                             (int16_t)bright);
    }
}

/* -----------------------------------------------------------------------
 * Draw a single zone
 *
 * Translated from AB3DI.s DoThisRoom (lines 3814-3925) and polyloop.
 *
 * The zone graphics data (from LEVELGRAPHICS) is a stream of entries:
 *   - First word: zone number (consumed before polyloop)
 *   - Then type words followed by type-specific data
 *
 * Type dispatch (from polyloop, lines 3828-3852):
 *   0  = wall        -> itsawalldraw (26 bytes of data after type)
 *   1  = floor       -> itsafloordraw (variable: ypos + sides + points + extra)
 *   2  = roof        -> itsafloordraw (same format)
 *   3  = clip setter -> no data (clipping done separately via ListOfGraphRooms)
 *   4  = object      -> ObjDraw (1 word: draw mode)
 *   5  = arc         -> CurveDraw (variable)
 *   6  = light beam  -> LightDraw (variable)
 *   7  = water       -> itsafloordraw (same as floor)
 *   8  = chunky floor-> itsafloordraw
 *   9  = bumpy floor -> itsafloordraw
 *  12  = backdrop    -> putinbackdrop (no extra data)
 *  13  = see-wall    -> itsawalldraw (same 26 bytes)
 *  <0  = end of list
 *
 * Wall entry (26 bytes after type word):
 *   +0: point1 (word)       - index into Points/Rotated array
 *   +2: point2 (word)       - index into Points/Rotated array
 *   +4: strip_start (word)  - leftend texture column
 *   +6: strip_end (word)    - rightend texture column
 *   +8: texture_tile (word) - tile offset (*16)
 *  +10: totalyoff (word)    - vertical texture offset
 *  +12: texture_id (word)   - index into walltiles[] array
 *  +14: VALAND (byte)       - vertical texture AND mask
 *  +15: VALSHIFT (byte)     - vertical texture shift
 *  +16: HORAND (word)       - horizontal texture AND mask
 *  +18: topofwall (long)    - wall top height (world coords)
 *  +22: botofwall (long)    - wall bottom height (world coords)
 *  +26: wallbrightoff (word)- brightness offset
 *  Total: 28 bytes after type word
 *
 * Floor/Roof entry (variable bytes after type word):
 *   +0: ypos (word)        - floor/ceiling height (>>6 for world)
 *   +2: num_sides-1 (word) - polygon sides minus 1
 *   +4: point indices (2 bytes * (sides))
 *   +4+2*sides: 4 bytes padding + 6 bytes extra data = 10 bytes
 *  Total: 2+2+2*sides+10 bytes after type word
 *
 * Object entry (2 bytes after type word):
 *   +0: draw_mode (word) - 0=before water, 1=after water, 2=full room
 * ----------------------------------------------------------------------- */
#define MAX_DOOR_ENTRIES 64

static int zone_has_door(const uint8_t *door_data, int16_t zone_id)
{
    if (!door_data) return 0;
    for (int i = 0; i < MAX_DOOR_ENTRIES; i++) {
        int16_t d_zone = rd16(door_data);
        if (d_zone < 0) return 0;  /* End of list */
        if (d_zone == zone_id) return 1;
        door_data += 16;
    }
    return 0;  /* Safety: avoid reading past buffer if format is wrong */
}

void renderer_draw_zone(GameState *state, int16_t zone_id, int use_upper)
{
    RendererState *r = &g_renderer;
    LevelState *level = &state->level;

    if (!level->data || !level->zone_adds || !level->zone_graph_adds) return;

    /* Get zone data */
    int32_t zone_off = rd32(level->zone_adds + zone_id * 4);
    const uint8_t *zone_data = level->data + zone_off;

    /* Zone heights */
    int32_t zone_floor = rd32(zone_data + 2);   /* ToZoneFloor */
    int32_t zone_roof  = rd32(zone_data + 6);   /* ToZoneRoof */

    /* Get zone graphics data (the polygon list for this zone).
     * zone_graph_adds: 8 bytes per zone = lower gfx offset (long) + upper gfx offset (long). */
    const uint8_t *zgraph = level->zone_graph_adds + zone_id * 8;
    int32_t gfx_off = use_upper ? rd32(zgraph + 4) : rd32(zgraph);
    if (gfx_off == 0 || !level->graphics) return;

    const uint8_t *gfx_data = level->graphics + gfx_off;
    int32_t zone_water = rd32(zone_data + 18);  /* ToZoneWater */
    (void)zone_water; /* Used by water-type floors when entry_type==7 */

    int32_t y_off = r->yoff;
    int half_h = RENDER_HEIGHT / 2;

    int zone_has_door_flag = zone_has_door(level->door_data, zone_id);

    /* Read zone number from graphics data (consumed before polyloop) */
    const uint8_t *ptr = gfx_data;
    /* int16_t gfx_zone = rd16(ptr); */
    ptr += 2;

    /* Brightness for this zone (from ZoneBrightTable if available).
     * Table layout: byte per zone, upper floor at [zone + num_zones].
     * Default 0 so dark corridors (zone_bright 0 in table) get as dark as the Amiga. */
    int16_t zone_bright = 0;
    if (level->zone_bright_table && zone_id >= 0 && zone_id < level->num_zones) {
        int bright_idx = use_upper && level->num_zones > 0
            ? zone_id + level->num_zones : zone_id;
        zone_bright = (int16_t)level->zone_bright_table[bright_idx];
    }

    /* Deferred walls: collect during stream parsing, draw AFTER floors/ceilings.
     * This ensures floors draw on the black framebuffer first, then walls
     * overwrite on top (matching Amiga behaviour). Stream order is preserved. */
    DeferredWall deferred[MAX_DEFERRED_WALLS];
    int num_deferred = 0;
    /* Water: append to global s_deferred_water (drawn after all zones, depth-sorted). */
    int objects_drawn_this_zone = 0;

    int max_iter = 500; /* Safety limit */

    while (max_iter-- > 0) {
        int16_t entry_type = rd16(ptr);
        ptr += 2; /* Consume type word (matches ASM (a0)+) */

        if (entry_type < 0) break; /* End of list */

        switch (entry_type) {
        case 0:  /* Wall */
        case 13: /* See-through wall */
        {
            /* Wall entry: 28 bytes of data
             * Translated from WallRoutine3.ChipMem.s itsawalldraw (line 1761)
             *
             * Walls are deferred and drawn after all floor/ceiling entries
             * so that floors can draw on the black framebuffer first. */
            int16_t p1       = rd16(ptr + 0);   /* point1 index */
            int16_t p2       = rd16(ptr + 2);   /* point2 index */
            int16_t leftend  = rd16(ptr + 4);   /* strip start */
            int16_t rightend = rd16(ptr + 6);   /* strip end */
            /* ASM line 1770-1772: fromtile = (ptr+8) << 4 */
            int16_t fromtile = rd16(ptr + 8) << 4; /* horiz texture offset */
            int16_t totalyoff = rd16(ptr + 10); /* vertical texture offset */
            uint8_t  valand    = ptr[14];             /* vert AND mask */
            uint8_t  valshift  = ptr[15];             /* vert shift */
            int16_t  horand    = rd16(ptr + 16);      /* horiz AND mask */
            int32_t topwall  = rd32(ptr + 18);  /* wall top height */
            int32_t botwall  = rd32(ptr + 22);  /* wall bottom height */
            int16_t wallbrightoff = rd16(ptr + 26);  /* ASM: move.w (a0)+,wallbrightoff */

            /* Subtract camera Y (ASM: sub.l d6,topofwall / sub.l d6,botofwall) */
            topwall -= y_off;
            botwall -= y_off;

            int16_t tex_id = rd16(ptr + 12);
            /* Switch walls: use p1 as point index; bit 1 of first word = on/off for texture only */
            if (p1 >= 0 && p1 < MAX_POINTS && p2 >= 0 && p2 < MAX_POINTS &&
                num_deferred < MAX_DEFERRED_WALLS)
            {
                int16_t rx1 = (int16_t)(r->rotated[p1].x >> 7);
                int16_t rz1 = (int16_t)r->rotated[p1].z;
                int16_t rx2 = (int16_t)(r->rotated[p2].x >> 7);
                int16_t rz2 = (int16_t)r->rotated[p2].z;
                const uint8_t *wall_tex = (tex_id >= 0 && tex_id < MAX_WALL_TILES) ? r->walltiles[tex_id] : NULL;

                /* Use dimensions from loaded file when available; else use level data. */
                uint8_t use_valand = valand, use_valshift = valshift;
                if (tex_id >= 0 && tex_id < MAX_WALL_TILES && r->wall_valshift[tex_id] != 0) {
                    use_valand = r->wall_valand[tex_id];
                    use_valshift = r->wall_valshift[tex_id];
                }

                /* Amiga: leftwallbright = pointBrightsPtr[pt] + 300 + wallbrightoff.
                 * We use zone_bright + wallbrightoff (no per-vertex brightness yet). */
                int16_t wall_bright = (int16_t)(zone_bright + wallbrightoff);

                int16_t wall_top = (int16_t)(topwall >> 8);
                int16_t wall_bot = (int16_t)(botwall >> 8);
                /* Original level height for texture step (before any door override). */
                int16_t wall_height_for_tex = (int16_t)((botwall - topwall) >> 8);
                if (wall_height_for_tex < 1) wall_height_for_tex = 1;
                if (zone_has_door_flag) {
                    int32_t zone_roof_rel = zone_roof - y_off;
                    int32_t zone_floor_rel = zone_floor - y_off;
                    int32_t top_abs = topwall + y_off, bot_abs = botwall + y_off;
                    const int32_t zone_match_margin = 512;
                    if (top_abs >= zone_roof - zone_match_margin &&
                        bot_abs <= zone_floor + zone_match_margin) {
                        wall_top = (int16_t)(zone_roof_rel >> 8);
                        wall_bot = (int16_t)(zone_floor_rel >> 8);
                    }
                }

                /* Switch walls (tex_id 11): same texture has on/off states.
                 * Amiga stores state in wall first word bit 1 (Anims.s: or.w #2,d3 for "on").
                 * Top half of texture (rows 0–15) = off, bottom half (rows 16–31) = on. */
                int16_t eff_totalyoff = totalyoff;
                if (tex_id == SWITCHES_WALL_TEX_ID && (p1 & 2))
                    eff_totalyoff = (int16_t)(totalyoff + 16);

                DeferredWall *dw = &deferred[num_deferred++];
                dw->x1 = rx1; dw->z1 = rz1; dw->x2 = rx2; dw->z2 = rz2;
                dw->top        = wall_top;
                dw->bot        = wall_bot;
                dw->wall_height_for_tex = wall_height_for_tex;
                dw->texture    = wall_tex;
                dw->tex_start  = leftend;
                dw->tex_end    = rightend;
                dw->brightness = wall_bright;
                dw->valand     = use_valand;
                dw->valshift   = use_valshift;
                dw->horand     = horand;
                dw->tex_id     = tex_id;
                dw->totalyoff  = eff_totalyoff;
                dw->fromtile   = fromtile;
            }
            ptr += 28;
            break;
        }

        case 1:  /* Floor */
        case 2:  /* Roof */
        case 7:  /* Water */
        case 8:  /* Chunky floor */
        case 9:  /* Bumpy floor */
        case 10: /* Bumpy floor variant */
        case 11: /* Bumpy floor variant */
        {
            /* Floor/Roof/Water polygon entry (variable size)
             * Translated from AB3DI.s itsafloordraw (line 5066).
             *
             * Format after type word:
             *   ypos (word): floor height
             *   num_sides-1 (word): number of polygon sides minus 1
             *   point_indices (word * (sides)): vertex indices
             *   extra_data (10 bytes): skip over
             */
            int16_t ypos = rd16(ptr);
            ptr += 2;
            int16_t num_sides_m1 = rd16(ptr);
            ptr += 2;

            /* Skip point indices (2 bytes each) */
            int sides = num_sides_m1 + 1;
            if (sides < 0) sides = 0;
            if (sides > 100) sides = 100; /* safety */

            /* We need these point indices for proper polygon rendering.
             * For now, collect them for the fill algorithm. */
            int16_t pt_indices[100];
            for (int s = 0; s < sides; s++) {
                pt_indices[s] = rd16(ptr);
                ptr += 2;
            }

            /* Extra data after point indices (ASM: pastsides, line 5891):
             *   +0: padding (2 bytes, consumed by sideloop peek + addq #2)
             *   +2: scaleval (word) - texture scale shift
             *   +4: whichtile (word) - byte offset into floortile sheet
             *   +6: brightness offset (word) - added to ZoneBright
             * Total: 8 bytes.  Note: dontdrawreturn uses lea 4+6(a0),a0
             * which skips past the last point index (2) + these 8 = 10. */
            ptr += 2; /* padding */
            /* int16_t scaleval = rd16(ptr); */ ptr += 2;
            int16_t whichtile = rd16(ptr); ptr += 2;
            int16_t floor_bright_off = rd16(ptr); ptr += 2;

            /* Determine floor height in world coords */
            int32_t floor_h_world = (int32_t)ypos << 6; /* ASM: asl.l #6,d7 */
            int32_t rel_h = floor_h_world - y_off; /* Relative to camera */

            /* Floor Y offset (from flooryoff) */
            int16_t floor_y_dist = (int16_t)(ypos - r->flooryoff);

            if (floor_y_dist == 0) {
                /* At eye level - skip */
                continue;
            }

            /* ---- Polygon scanline rasterization ----
             * Translated from AB3DI.s sideloop (line 5208).
             *
             * Build left/right edge tables from polygon edges,
             * then fill between the edges for each row.
             *
             * Screen Y at each vertex: sy = rel_h / z + center
             * Screen X from on_screen[] (already projected).
             */
            int center = RENDER_HEIGHT / 2;  /* Match wall projection center */
            int16_t left_edge[RENDER_HEIGHT];
            int16_t right_edge_tab[RENDER_HEIGHT];
            for (int i = 0; i < RENDER_HEIGHT; i++) {
                left_edge[i] = (int16_t)RENDER_WIDTH;
                right_edge_tab[i] = -1;
            }
            int poly_top = RENDER_HEIGHT;
            int poly_bot = -1;

            /* Clamp Y range for floor vs ceiling */
            int y_min_clamp, y_max_clamp;
            if (floor_y_dist > 0) {
                y_min_clamp = half_h;       /* floor: center to bottom */
                y_max_clamp = RENDER_HEIGHT - 1;
            } else {
                y_min_clamp = 0;            /* ceiling: top to center */
                y_max_clamp = half_h - 1;
            }

            /* Full screen: use full extension; portal view: use 1px only to avoid drawing outside. */
            int full_screen_zone = (r->left_clip == 0 && r->right_clip == RENDER_WIDTH);
            int edge_extra_portal = full_screen_zone ? 0 : PORTAL_EDGE_EXTRA;  /* portal: 1px to close gaps only */

            /* Walk each polygon edge and rasterize into edge tables (floor and ceiling/roof).
             * Near-plane clip edges so vertices behind the camera get proper
             * screen X values (otherwise on_screen[].screen_x is garbage and
             * the edge table doesn't reach the screen sides). */
            const int16_t FLOOR_NEAR = 4;
            for (int s = 0; s < sides; s++) {
                int i1 = pt_indices[s];
                int i2 = pt_indices[(s + 1) % sides];
                if (i1 < 0 || i1 >= MAX_POINTS || i2 < 0 || i2 >= MAX_POINTS) continue;

                int32_t z1 = r->rotated[i1].z;
                int32_t z2 = r->rotated[i2].z;
                int32_t rx1 = r->rotated[i1].x;
                int32_t rx2 = r->rotated[i2].x;

                /* Near-plane clip: interpolate X at FLOOR_NEAR when behind camera.
                 * Do this even when both vertices are behind (e.g. ceiling's back edge)
                 * so we still rasterize that edge and the polygon extends to the screen. */
                int32_t ez1 = z1, ez2 = z2;
                int32_t ex1 = rx1, ex2 = rx2;
                if (ez1 < FLOOR_NEAR) {
                    int32_t dz = ez2 - ez1;
                    if (dz != 0) {
                        int32_t t = ((int32_t)(FLOOR_NEAR - ez1) << 16) / dz;
                        ex1 = rx1 + (int32_t)((int64_t)(rx2 - rx1) * t / 65536);
                    }
                    ez1 = FLOOR_NEAR;
                }
                if (ez2 < FLOOR_NEAR) {
                    int32_t dz = ez1 - ez2;
                    if (dz != 0) {
                        int32_t t = ((int32_t)(FLOOR_NEAR - ez2) << 16) / dz;
                        ex2 = rx2 + (int32_t)((int64_t)(rx1 - rx2) * t / 65536);
                    }
                    ez2 = FLOOR_NEAR;
                }
                /* Project X: use on_screen only when safely in front (z >= FLOOR_NEAR).
                 * For z < FLOOR_NEAR (behind or very close) use clipped projection so rotation
                 * doesn't cause jumps when vertices cross the camera plane or get very close. */
                int sx1 = (z1 >= FLOOR_NEAR) ? r->on_screen[i1].screen_x
                        : (int)((int64_t)ex1 * RENDER_SCALE / ez1) + RENDER_WIDTH / 2;
                int sx2 = (z2 >= FLOOR_NEAR) ? r->on_screen[i2].screen_x
                        : (int)((int64_t)ex2 * RENDER_SCALE / ez2) + RENDER_WIDTH / 2;

                /* Project Y: same rule so X and Y stay consistent (no jump when z crosses FLOOR_NEAR). */
                int32_t rel_h_8 = rel_h >> 8;
                int sy1_raw = (int)((int64_t)rel_h_8 * PROJ_Y_SCALE * RENDER_SCALE / (int32_t)ez1) + center;
                int sy2_raw = (int)((int64_t)rel_h_8 * PROJ_Y_SCALE * RENDER_SCALE / (int32_t)ez2) + center;

                /* Clamp for horizontal edge check */
                int sy1 = sy1_raw;
                int sy2 = sy2_raw;
                if (sy1 < y_min_clamp) sy1 = y_min_clamp;
                if (sy1 > y_max_clamp) sy1 = y_max_clamp;
                if (sy2 < y_min_clamp) sy2 = y_min_clamp;
                if (sy2 > y_max_clamp) sy2 = y_max_clamp;

                /* DDA edge walk - use raw Y values to properly interpolate X
                 * across visible rows even when endpoints project off-screen */
                int dy_raw = sy2_raw - sy1_raw;
                if (dy_raw == 0) {
                    /* Truly horizontal edge in world space */
                    int row = sy1;
                    if (row >= y_min_clamp && row <= y_max_clamp) {
                        int lo = sx1 < sx2 ? sx1 : sx2;
                        int hi = sx1 > sx2 ? sx1 : sx2;
                        int he_extra = full_screen_zone ? ((floor_y_dist < 0) ? CEILING_EDGE_EXTRA : FLOOR_EDGE_EXTRA) : edge_extra_portal;
                        lo -= he_extra;
                        hi += he_extra;
                        if (lo < r->left_clip) lo = r->left_clip;
                        if (hi >= r->right_clip) hi = r->right_clip - 1;
                        if (lo < left_edge[row]) left_edge[row] = (int16_t)lo;
                        if (hi > right_edge_tab[row]) right_edge_tab[row] = (int16_t)hi;
                        if (row < poly_top) poly_top = row;
                        if (row > poly_bot) poly_bot = row;
                    }
                    continue;
                }

                /* Walk visible rows, interpolating X using raw Y values */
                int row_start = (sy1_raw < sy2_raw) ? sy1_raw : sy2_raw;
                int row_end = (sy1_raw > sy2_raw) ? sy1_raw : sy2_raw;
                if (row_start < y_min_clamp) row_start = y_min_clamp;
                if (row_end > y_max_clamp) row_end = y_max_clamp;

                /* Use fixed-point for better precision */
                int64_t x_fp = (int64_t)sx1 << 16;
                int64_t dx_fp = ((int64_t)(sx2 - sx1) << 16) / dy_raw;
                
                /* Adjust starting position if row_start is clamped */
                if (sy1_raw < sy2_raw) {
                    x_fp += dx_fp * (row_start - sy1_raw);
                } else {
                    x_fp = (int64_t)sx2 << 16;
                    dx_fp = ((int64_t)(sx1 - sx2) << 16) / (-dy_raw);
                    x_fp += dx_fp * (row_start - sy2_raw);
                }
                
                int edge_extra = full_screen_zone ? ((floor_y_dist < 0) ? CEILING_EDGE_EXTRA : FLOOR_EDGE_EXTRA) : edge_extra_portal;
                for (int row = row_start; row <= row_end; row++) {
                    if (row < 0 || row >= RENDER_HEIGHT) { x_fp += dx_fp; continue; }
                    int x = (int)(x_fp >> 16);
                    /* Extend edges only when full screen (avoid drawing outside portal). */
                    int left_x = x - edge_extra;
                    int right_x = x + edge_extra;
                    if (left_x < r->left_clip) left_x = r->left_clip;
                    if (right_x >= r->right_clip) right_x = r->right_clip - 1;
                    if (left_x < left_edge[row]) left_edge[row] = (int16_t)left_x;
                    if (right_x > right_edge_tab[row]) right_edge_tab[row] = (int16_t)right_x;
                    if (row < poly_top) poly_top = row;
                    if (row > poly_bot) poly_bot = row;
                    x_fp += dx_fp;
                }
            }

            /* Clamp polygon bounds so row is always in [0, RENDER_HEIGHT-1] */
            if (poly_top < y_min_clamp) poly_top = y_min_clamp;
            if (poly_top >= RENDER_HEIGHT) poly_top = RENDER_HEIGHT - 1;
            if (poly_bot > y_max_clamp) poly_bot = y_max_clamp;
            if (poly_bot >= RENDER_HEIGHT) poly_bot = RENDER_HEIGHT - 1;
            if (poly_bot < 0) poly_bot = -1;

            /* Resolve floor texture: floortile + whichtile offset.
             * ASM: move.l floortile,a0 / adda.w whichtile,a0 */
            const uint8_t *floor_tex = NULL;
            if (r->floor_tile && whichtile >= 0) {
                floor_tex = r->floor_tile + (uint16_t)whichtile;
            }

            /* Brightness: zone_bright + floor entry's brightness offset
             * ASM: move.w (a0)+,d6 / add.w ZoneBright,d6 */
            int16_t bright = zone_bright + floor_bright_off;

            /* Fill between edges for each row (floor and ceiling/roof). Clamp to zone clip. */
            for (int row = poly_top; row <= poly_bot; row++) {
                if (row < 0 || row >= RENDER_HEIGHT) continue;
                int16_t le = left_edge[row];
                int16_t re = right_edge_tab[row];
                if (le >= RENDER_WIDTH || re < 0) continue;
                if (le < r->left_clip) le = (int16_t)r->left_clip;
                if (re >= r->right_clip) re = (int16_t)(r->right_clip - 1);
                if (le > re) continue;
                if (entry_type == 7) {
                    /* Water: defer to global list (drawn after all zones, depth-sorted). */
                    if (s_num_deferred_water < MAX_DEFERRED_WATER_SPANS) {
                        int row_dist = row - half_h;
                        int abs_rd = (row_dist >= 0) ? row_dist : -row_dist;
                        if (abs_rd <= 3) abs_rd = 4;
                        int32_t dist = (int32_t)((int64_t)(rel_h >> 8) * PROJ_Y_SCALE * RENDER_SCALE / abs_rd);
                        if (dist < 1) dist = 1;
                        if (dist > 32767) dist = 32767;
                        int32_t span_depth = (floor_y_dist < 0) ? dist - 1 : dist;
                        if (span_depth < 0) span_depth = 0;
                        if (span_depth > 32767) span_depth = 32767;
                        DeferredWaterSpan *ws = &s_deferred_water[s_num_deferred_water++];
                        ws->y = (int16_t)row;
                        ws->x_left = le;
                        ws->x_right = re;
                        ws->floor_height = rel_h;
                        ws->depth = (int16_t)span_depth;
                        ws->texture = floor_tex;
                        ws->brightness = bright;
                    }
                } else {
                    renderer_draw_floor_span((int16_t)row, le, re,
                                             rel_h, floor_tex, bright, 0);
                }
            }
            break;
        }

        case 3: /* Clip setter */
            /* No additional data consumed in polyloop
             * (clipping is done from ListOfGraphRooms, not here) */
            break;

        case 4: /* Object (sprite) */
        {
            /* ObjDraw reads 1 word: draw mode (0=before water, 1=after, 2=full)
             * Then iterates ALL objects in ObjectData for this zone.
             * We defer drawing to the end of the zone so sprites are always
             * on top of floor and walls (avoid "partially behind floor"). */
            ptr += 2;
            objects_drawn_this_zone = 1;  /* will draw after deferred walls */
            break;
        }

        case 5: /* Arc (curved wall) */
        {
            /* Arc entry: 28 bytes of data after type word.
             * Translated from WallRoutine3.ChipMem.s CurveDraw (lines 498-538):
             *   center_pt(2) + edge_pt(2) + bitmap_start(2) + bitmap_end(2) +
             *   angle(2) + subdivide_idx(2) + walltiles_offset(4) +
             *   basebright(2) + brightmult(2) + topofwall(4) + botofwall(4) = 28
             */
            int16_t center_pt = rd16(ptr + 0);
            int16_t edge_pt   = rd16(ptr + 2);
            int16_t bmp_start = rd16(ptr + 4);
            int16_t bmp_end   = rd16(ptr + 6);
            /* int16_t arc_angle = rd16(ptr + 8); */
            int16_t subdiv_idx = rd16(ptr + 10);
            int32_t tex_offset = rd32(ptr + 12);
            int16_t base_bright = rd16(ptr + 16);
            /* int16_t bright_mult = rd16(ptr + 18); */
            int32_t topwall = rd32(ptr + 20);
            int32_t botwall = rd32(ptr + 24);
            
            topwall -= y_off;
            botwall -= y_off;
            
            /* Subdivide lookup: index -> (shift, count) */
            static const int subdiv_counts[] = { 4, 8, 16, 32, 64 };
            int num_segments = 4;
            if (subdiv_idx >= 0 && subdiv_idx < 5) {
                num_segments = subdiv_counts[subdiv_idx];
            }
            
            /* Get center and edge in rotated space */
            if (center_pt >= 0 && center_pt < MAX_POINTS &&
                edge_pt >= 0 && edge_pt < MAX_POINTS)
            {
                int32_t cx = r->rotated[center_pt].x;
                int32_t cz = r->rotated[center_pt].z;
                int32_t ex = r->rotated[edge_pt].x;
                int32_t ez = r->rotated[edge_pt].z;
                
                /* Compute radius vector from center to edge */
                int32_t dx = ex - cx;
                int32_t dz = ez - cz;
                
                /* Get texture - tex_offset is byte offset into walltiles array */
                int16_t tex_id = (int16_t)(tex_offset / 4096); /* Approximate: each texture is 4K */
                const uint8_t *arc_tex = (tex_id >= 0 && tex_id < MAX_WALL_TILES) ? r->walltiles[tex_id] : NULL;
                
                /* Draw arc as series of wall segments */
                int32_t prev_x = (ex >> 7);
                int32_t prev_z = ez;
                int32_t prev_t = bmp_start;
                
                for (int seg = 1; seg <= num_segments; seg++) {
                    /* Compute angle for this segment (0 to 2*PI over num_segments) */
                    /* Use integer sin/cos approximation */
                    int angle = (seg * 1024) / num_segments; /* 0-1024 represents 0-360 degrees */
                    
                    /* Simple sin/cos using lookup or approximation */
                    /* sin/cos tables would be ideal but for now use rough approximation */
                    int32_t s, c;
                    /* Map angle 0-1024 to sin/cos (-256 to 256 scale) */
                    int a = angle & 1023;
                    if (a < 256) {
                        s = a;
                        c = 256 - (a * a / 512);
                    } else if (a < 512) {
                        s = 512 - a;
                        c = -(a - 256);
                    } else if (a < 768) {
                        s = -(a - 512);
                        c = -(768 - a);
                    } else {
                        s = a - 1024;
                        c = 256 - ((1024 - a) * (1024 - a) / 512);
                    }
                    
                    /* Rotate radius vector by angle */
                    int32_t rx = (dx * c - dz * s) / 256;
                    int32_t rz = (dx * s + dz * c) / 256;
                    
                    int32_t new_x = (cx + rx) >> 7;
                    int32_t new_z = cz + rz;
                    int32_t new_t = bmp_start + ((bmp_end - bmp_start) * seg / num_segments);
                    
                    /* Draw wall segment if we have room */
                    if (num_deferred < MAX_DEFERRED_WALLS && new_z > 0 && prev_z > 0) {
                        DeferredWall *dw = &deferred[num_deferred++];
                        dw->x1 = (int16_t)prev_x;
                        dw->z1 = (int16_t)prev_z;
                        dw->x2 = (int16_t)new_x;
                        dw->z2 = (int16_t)new_z;
                        dw->top = (int16_t)(topwall >> 8);
                        dw->bot = (int16_t)(botwall >> 8);
                        dw->texture = arc_tex;
                        dw->tex_start = (int16_t)prev_t;
                        dw->tex_end = (int16_t)new_t;
                        dw->brightness = base_bright;
                        dw->valand = 63;    /* Default */
                        dw->valshift = 6;   /* Default */
                        dw->horand = 255;   /* Default */
                        dw->tex_id = tex_id;
                        dw->totalyoff = 0;
                        dw->fromtile = 0;
                        dw->wall_height_for_tex = (int16_t)((botwall - topwall) >> 8);
                        if (dw->wall_height_for_tex < 1) dw->wall_height_for_tex = 1;
                    }
                    
                    prev_x = new_x;
                    prev_z = new_z;
                    prev_t = new_t;
                }
            }
            
            ptr += 28;
            break;
        }

        case 6: /* Light beam */
        {
            /* Light beams: 4 bytes of data after type word.
             * Translated from AB3DI.s LightDraw (lines 4364-4365):
             *   point1(2) + point2(2) = 4 bytes */
            ptr += 4;
            break;
        }

        case 12: /* Backdrop */
        {
            /* putinbackdrop - no additional data in the graphics stream */
            /* Would fill the background with sky texture */
            break;
        }

        default:
            /* Unknown type - skip nothing (type word already consumed) */
            break;
        }
    }

    /* Sort deferred walls by depth (painter's algorithm: far to near).
     * Use maximum Z of endpoints as sort key - walls further away draw first,
     * closer walls overwrite them. Simple insertion sort is fine for small N. */
    for (int i = 1; i < num_deferred; i++) {
        DeferredWall tmp = deferred[i];
        int16_t tmp_z = (tmp.z1 > tmp.z2) ? tmp.z1 : tmp.z2;
        int j = i - 1;
        while (j >= 0) {
            int16_t j_z = (deferred[j].z1 > deferred[j].z2) ? deferred[j].z1 : deferred[j].z2;
            if (j_z >= tmp_z) break;
            deferred[j + 1] = deferred[j];
            j--;
        }
        deferred[j + 1] = tmp;
    }

    /* Draw sorted walls (far to near). */
    for (int i = 0; i < num_deferred; i++) {
        DeferredWall *dw = &deferred[i];
        if (dw->tex_id >= 0 && dw->tex_id < MAX_WALL_TILES) {
            r->cur_wall_pal = r->wall_palettes[dw->tex_id];
        } else {
            r->cur_wall_pal = NULL;
        }
        renderer_draw_wall(dw->x1, dw->z1, dw->x2, dw->z2,
                          dw->top, dw->bot,
                          dw->texture, dw->tex_start, dw->tex_end,
                          dw->brightness, dw->valand, dw->valshift, dw->horand,
                          dw->totalyoff, dw->fromtile, dw->tex_id,
                          dw->wall_height_for_tex);
    }

    /* Draw zone objects after all floor and walls so sprites are on top.
     * (objects_drawn_this_zone means type-4 was in stream; we still draw once per zone.) */
    draw_zone_objects(state, zone_id, zone_roof, zone_floor);
}

/* -----------------------------------------------------------------------
 * DrawDisplay - Main rendering entry point
 *
 * Translated from AB3DI.s DrawDisplay (lines 3395-3693).
 *
 * This is called once per frame to render the entire 3D scene.
 * ----------------------------------------------------------------------- */
void renderer_draw_display(GameState *state)
{
    RendererState *r = &g_renderer;
    if (!r->buffer) return;

    /* 1. Clear framebuffer */
    renderer_clear(0);

    /* Water: advance phase every 2nd frame, full cycle 0-255 for slow animation */
    static int water_tick = 0;
    if ((++water_tick & 1) == 0) {
        g_water_phase = (g_water_phase + 1) & 255;
    }

    /* 2. Setup view transform (from AB3DI.s DrawDisplay lines 3399-3438) */
    PlayerState *plr = (state->mode == MODE_SLAVE) ? &state->plr2 : &state->plr1;

    int16_t ang = (int16_t)(plr->angpos & 0x3FFF); /* 14-bit angle */
    r->sinval = sin_lookup(ang);
    r->cosval = cos_lookup(ang);

    /* Extract integer part of 16.16 fixed-point position for rendering.
     * On Amiga: .w operations on big-endian 32-bit values read the high word.
     * Raise camera Y for drawing only so floor appears further away (match Amiga). */
    r->xoff = (int16_t)(plr->xoff >> 16);
    r->zoff = (int16_t)(plr->zoff >> 16);
    r->yoff = plr->yoff - VIEW_HEIGHT_LIFT;

    /* wallyoff = (yoff >> 8) + 224, masked to 0-255 */
    int32_t y_shifted = r->yoff >> 8;
    r->wallyoff = (int16_t)((y_shifted + 256 - 32) & 255);
    r->flooryoff = (int16_t)(y_shifted << 2);

    /* xoff34 = xoff * 3/4, zoff34 = zoff * 3/4 */
    r->xoff34 = (int16_t)((r->xoff * 3) >> 2);
    r->zoff34 = (int16_t)((r->zoff * 3) >> 2);

    /* xwobble from head bob */
    r->xwobble = 0; /* Would be set from plr->bob_frame */

    /* 3. Initialize column clipping and depth buffer */
    for (int i = 0; i < RENDER_WIDTH; i++) {
        r->clip.top[i] = 0;
        r->clip.bot[i] = RENDER_HEIGHT - 1;
    }
    /* Clear depth buffer to far (large Z = far away) */
    for (int i = 0; i < RENDER_WIDTH * RENDER_HEIGHT; i++) {
        r->depth_buffer[i] = 32767;
    }

    /* 4. Rotate geometry */
    renderer_rotate_level_pts(state);
    renderer_rotate_object_pts(state);

    /* 5. Iterate zones back-to-front (from OrderZones output)
     *
     * order_zones produces zone_order_zones[0]=farthest .. [count-1]=nearest.
     * Draw in array order so far zones are drawn first, near zones last (painter's).
     *
     * Translated from AB3DI.s subroomloop (lines 3456-3643).
     *
     * For each zone:
     *   a) Find it in ListOfGraphRooms to get clip data offset
     *   b) Apply clipping from LEVELCLIPS using NEWsetlclip/NEWsetrclip
     *   c) Skip if fully clipped
     *   d) Render the zone
     */
    s_num_deferred_water = 0;
    for (int i = 0; i < state->zone_order_count; i++) {
        int16_t zone_id = state->zone_order_zones[i];
        if (zone_id < 0) continue;

        /* Reset clip to full screen for each zone */
        r->left_clip = 0;
        r->right_clip = RENDER_WIDTH;
        r->top_clip = 0;
        r->bot_clip = RENDER_HEIGHT - 1;
        r->wall_top_clip = -1;
        r->wall_bot_clip = -1;

        /* Apply zone clipping from ListOfGraphRooms + LEVELCLIPS.
         * ListOfGraphRooms entries are 8 bytes:
         *   +0: zone_id (word)
         *   +2: clip_offset (word) - offset into LEVELCLIPS (* 2)
         *   +4: reserved (4 bytes)
         * LEVELCLIPS is an array of word-sized point indices.
         * First section: left clips (terminated by -1)
         * Second section: right clips (terminated by -1) */
        if (state->level.list_of_graph_rooms && state->level.clips) {
            const uint8_t *lgr = state->level.list_of_graph_rooms;
            /* Find this zone's entry in ListOfGraphRooms */
            int found = 0;
            while (rd16(lgr) >= 0) {
                if (rd16(lgr) == zone_id) {
                    found = 1;
                    break;
                }
                lgr += 8;
            }

            if (found) {
                int16_t clip_off = rd16(lgr + 2);
                if (clip_off >= 0) {
                    const uint8_t *clip_ptr = state->level.clips + clip_off * 2;

                    /* Left clips: tighten leftclip.
                     * Each entry is a point index.
                     * If point is in front, use its OnScreen X as left clip. */
                    while (rd16(clip_ptr) >= 0) {
                        int16_t pt = rd16(clip_ptr);
                        clip_ptr += 2;
                        if (pt >= 0 && pt < MAX_POINTS) {
                            if (r->rotated[pt].z > 0) {
                                int16_t sx = r->on_screen[pt].screen_x;
                                if (sx > r->left_clip) {
                                    r->left_clip = sx;
                                }
                            }
                        }
                    }
                    clip_ptr += 2; /* Skip -1 terminator */

                    /* Right clips: tighten rightclip */
                    while (rd16(clip_ptr) >= 0) {
                        int16_t pt = rd16(clip_ptr);
                        clip_ptr += 2;
                        if (pt >= 0 && pt < MAX_POINTS) {
                            if (r->rotated[pt].z > 0) {
                                int16_t sx = r->on_screen[pt].screen_x;
                                if (sx < r->right_clip) {
                                    r->right_clip = sx;
                                }
                            }
                        }
                    }
                }
            }

            /* Skip if fully clipped */
            if (r->left_clip >= RENDER_WIDTH) continue;
            if (r->right_clip <= 0) continue;
            if (r->left_clip >= r->right_clip) continue;
        }

        /* Multi-floor zone: draw lower and upper room with vertical clip split */
        if (state->level.zone_adds && state->level.zone_graph_adds) {
            int32_t zone_off = rd32(state->level.zone_adds + zone_id * 4);
            const uint8_t *zgraph = state->level.zone_graph_adds + zone_id * 8;
            int32_t upper_gfx = rd32(zgraph + 4);
            if (upper_gfx != 0 && zone_off >= 0 && state->level.data) {
                const uint8_t *zd = state->level.data + zone_off;
                int32_t zone_roof = rd32(zd + 6);  /* ToZoneRoof = split height */
                int32_t rel = zone_roof - r->yoff;
                /* Split screen Y where lower ceiling / upper floor projects (ref z ~400). */
                int split_y = (int)((int64_t)(rel >> 8) * PROJ_Y_SCALE * RENDER_SCALE / 400) + (RENDER_HEIGHT / 2);
                if (split_y < 1) split_y = 1;
                if (split_y >= RENDER_HEIGHT) split_y = RENDER_HEIGHT - 1;
                /* Reserve a band above the split for the lower room so the wall can extend up to
                 * meet the ceiling. When very close to the wall the ceiling projects high on screen,
                 * so use a large margin so the join holds even at close range. */
                const int split_margin = (RENDER_HEIGHT * 3) / 5;  /* large band so wall meets ceiling when very close */
                int lower_top = split_y - split_margin;
                if (lower_top < 0) lower_top = 0;
                int upper_bot = split_y - split_margin - 1;
                if (upper_bot < 0) upper_bot = -1;
                r->top_clip = (int16_t)lower_top;
                r->bot_clip = RENDER_HEIGHT - 1;
                r->wall_top_clip = 0;   /* lower room: walls can extend to top when very close to wall */
                r->wall_bot_clip = -1;
                renderer_draw_zone(state, zone_id, 0);  /* lower room */
                r->top_clip = 0;
                r->bot_clip = (int16_t)(upper_bot >= 0 ? upper_bot : split_y - 1);
                r->wall_top_clip = -1;
                r->wall_bot_clip = (int16_t)split_y;  /* upper room: walls extend down to meet floor at split */
                renderer_draw_zone(state, zone_id, 1);  /* upper room */
                continue;
            }
        }
        renderer_draw_zone(state, zone_id, 0);
    }

    /* Draw all deferred water after every zone's opaque geometry (transparency ordering).
     * Sort back-to-front (larger depth = further = draw first) so overlapping water blends correctly. */
    for (int i = 1; i < s_num_deferred_water; i++) {
        DeferredWaterSpan tmp = s_deferred_water[i];
        int16_t tmp_d = tmp.depth;
        int j = i - 1;
        while (j >= 0 && s_deferred_water[j].depth < tmp_d) {
            s_deferred_water[j + 1] = s_deferred_water[j];
            j--;
        }
        s_deferred_water[j + 1] = tmp;
    }
    for (int i = 0; i < s_num_deferred_water; i++) {
        DeferredWaterSpan *ws = &s_deferred_water[i];
        renderer_draw_floor_span(ws->y, ws->x_left, ws->x_right,
                                 ws->floor_height, ws->texture, ws->brightness, 1);
    }

    /* Debug: count visible non-zero pixels + histogram */
    {
        static int dbg_frame = 0;
        if (dbg_frame < 3) {
            int nonzero = 0;
            int histo[16] = {0}; /* buckets for value ranges 0-15, 16-31, etc */
            for (int y = 0; y < RENDER_HEIGHT; y++) {
                for (int x = 0; x < RENDER_WIDTH; x++) {
                    uint8_t v = r->buffer[y * RENDER_STRIDE + x];
                    if (v) { nonzero++; histo[v >> 4]++; }
                }
            }
            printf("[RENDER] Frame %d: %d zones, %d/%d visible non-zero px\n",
                   dbg_frame, state->zone_order_count, nonzero, RENDER_WIDTH*RENDER_HEIGHT);
            printf("[RENDER] Histo: ");
            for (int i = 0; i < 16; i++) {
                if (histo[i]) printf("[%d-%d]=%d ", i*16, i*16+15, histo[i]);
            }
            printf("\n");
            fflush(stdout);
        }
        dbg_frame++;
    }

    /* 6. Draw gun overlay */
    renderer_draw_gun(state);

    /* 7. Swap buffers (the just-drawn buffer becomes the display buffer) */
    renderer_swap();
}
