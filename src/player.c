/*
 * Alien Breed 3D I - PC Port
 * player.c - Player control and movement physics
 *
 * Translated from: Plr1Control.s, Plr2Control.s, PlayerShoot.s, LevelData2.s,
 *                  AB3DI.s (PLR1_Control, PLR2_Control)
 */

#include "player.h"
#include "game_data.h"
#include "movement.h"
#include "objects.h"
#include "math_tables.h"
#include "stub_input.h"
#include "stub_audio.h"
#include "stub_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Constants from the original ASM
 * ----------------------------------------------------------------------- */
#define WALK_SPEED          2
#define RUN_SPEED           3
#define MAX_TURN_WALK       35
#define MAX_TURN_RUN        60
#define TURN_STEP           10
#define MOUSE_CLAMP         50
#define HEIGHT_STEP         1024
#define BOBBLE_MASK         8190
#define CLUMP_MASK          4095
#define CLUMP_THRESHOLD     (-4096)  /* and.w #-4096,d1 checks bit 12+ */
/* Amiga step-up: same scale as zone floor heights. game_data uses 40*256 for marines;
 * movement.c default is 40*256. Step-UP blocked when ledge is higher than this.
 * Step-DOWN always passable (unlimited). */
#define STEP_UP_NORMAL      (40 * 256)   /* 10240 - match Amiga ObjectMove step-up */
#define STEP_UP_DUCKED      (20 * 256)   /* 5120 - stricter when ducked */
#define STEP_DOWN_DEFAULT   0x3fffffff  /* effectively unlimited */

/* Gun selection key -> gun index mapping (from GUNVALS in Plr1Control.s) */
static const int8_t gun_key_map[6] = { 0, 7, 1, 2, 3, 4 };

/* -----------------------------------------------------------------------
 * Friction helper
 *
 * The original code computes a per-tick friction term from velocity,
 * then applies it as:
 *   velocity = velocity - friction_term
 *
 * The friction term uses this sequence:
 *   neg.l d6; ble .nobug; asr #N,d6; add #1,d6; bra .done
 *   .nobug: asr #N,d6
 *
 * Control mode shifts:
 *   Mouse:    #1
 *   Keyboard: #3
 *   Mouse+KBD (port tuning): #2
 * ----------------------------------------------------------------------- */
static int32_t apply_friction(int32_t val, int shift)
{
    int32_t neg = -val;
    if (neg > 0) {
        return -((neg >> shift) + 1);
    } else {
        return -(neg >> shift);
    }
}

/* -----------------------------------------------------------------------
 * PLR1_alwayskeys - Keys checked regardless of control mode
 *
 * Translated from Plr1Control.s PLR1_alwayskeys (~line 187-315)
 * ----------------------------------------------------------------------- */
static void player_always_keys(PlayerState *plr, const uint8_t *key_map,
                                const KeyBindings *keys, uint8_t *old_space)
{
    /* Operate/Space - tap detection */
    uint8_t space_state = key_map[keys->operate];
    if (space_state && !(*old_space)) {
        plr->spctap = -1;  /* st = set to $FF */
    }
    *old_space = space_state;

    /* Duck toggle */
    if (key_map[keys->duck]) {
        /* The original clears the key after reading to make it a toggle */
        plr->s_targheight = PLAYER_HEIGHT;
        plr->ducked = !plr->ducked;
        if (plr->ducked) {
            plr->s_targheight = PLAYER_CROUCHED;
        }
    }

    /* NOTE: Auto-duck is handled in player_full_control where GameState is available */

    /* Height interpolation toward target */
    int32_t h = plr->s_height;
    int32_t target = plr->s_targheight;
    if (h < target) {
        h += HEIGHT_STEP;
        if (h > target) h = target;
    } else if (h > target) {
        h -= HEIGHT_STEP;
        if (h < target) h = target;
    }
    plr->s_height = h;

    /* Weapon selection (keys 1-5) */
    /* Keys 1-5 are at Amiga raw codes $01-$05, which map to key_map[1..5] */
    for (int i = 0; i < 5; i++) {
        if (key_map[i + 1]) {
            int gun_idx = gun_key_map[i];
            /* Check if weapon is available (visible flag in gun_data); skip placeholder */
            //if (plr->gun_data[gun_idx].visible) {
                plr->gun_selected = (int16_t)gun_idx;
            //}
        }
    }
}

/* -----------------------------------------------------------------------
 * Keyboard control
 *
 * Translated from Plr1Control.s PLR1_keyboard_control (~line 337-547)
 * ----------------------------------------------------------------------- */
static void player_keyboard_control(PlayerState *plr, const uint8_t *key_map,
                                     const KeyBindings *keys, int16_t temp_frames)
{
    int16_t max_turn = MAX_TURN_WALK;
    int16_t move_speed = WALK_SPEED;

    /* Running */
    if (key_map[keys->run]) {
        max_turn = MAX_TURN_RUN;
        move_speed = RUN_SPEED;
    }

    /* Ducked halves speed */
    if (plr->ducked) {
        move_speed >>= 1;
    }

    /* Scale by temp_frames: on the Amiga, speed = temp_frames * walk_speed.
     * This gives larger per-tick deltas so the collision system works. */
    move_speed *= temp_frames;

    /* ---- Turning ---- */
    int16_t angspd = plr->s_angspd;

    /* Friction on turn speed: d3 = d3*3/4 */
    angspd = (angspd * 3) / 4;

    /* Force sidestep mode swaps turn keys with strafe keys */
    uint8_t left_key = keys->turn_left;
    uint8_t right_key = keys->turn_right;
    uint8_t sl_key = keys->sidestep_left;
    uint8_t sr_key = keys->sidestep_right;

    if (key_map[keys->force_sidestep]) {
        sl_key = keys->turn_left;
        sr_key = keys->turn_right;
        left_key = 0xFF;  /* disable turning */
        right_key = 0xFF;
    }

    if (left_key < 128 && key_map[left_key]) {
        angspd -= TURN_STEP;
    }
    if (right_key < 128 && key_map[right_key]) {
        angspd += TURN_STEP;
    }

    /* Clamp turn speed */
    if (angspd > max_turn) angspd = max_turn;
    if (angspd < -max_turn) angspd = -max_turn;

    /* Apply turn to angle (doubled in original: add.w d3,d0; add.w d3,d0) */
    int16_t angpos = plr->s_angpos;
    angpos += angspd * 2;
    plr->s_angspd = angspd;

    /* ---- Strafe ---- */
    int16_t strafe = 0;
    if (sl_key < 128 && key_map[sl_key]) {
        strafe = (move_speed * 2 + 1) / 2;   /* add+add+asr = *1.5 rounded */
    }
    if (sr_key < 128 && key_map[sr_key]) {
        strafe = -((move_speed * 2 + 1) / 2);
    }

    /* ---- Angle and sin/cos lookup ---- */
    angpos &= 8191;
    plr->s_angpos = angpos;
    plr->s_sinval = sin_lookup(angpos);
    plr->s_cosval = cos_lookup(angpos);

    /* ---- Velocity friction (keyboard: ~1/8 removed per tick) ---- */
    int32_t xspd = plr->s_xspdval - apply_friction(plr->s_xspdval, 3);
    int32_t zspd = plr->s_zspdval - apply_friction(plr->s_zspdval, 3);

    /* ---- Forward/backward ---- */
    int16_t fwd = 0;
    if (key_map[keys->forward]) {
        fwd = -move_speed;
    }
    if (key_map[keys->backward]) {
        fwd = move_speed;
    }

    /* ---- Bobble ---- */
    int16_t bob_delta = fwd << 6;  /* asl.w #6,d2 */
    plr->bobble = (plr->bobble + bob_delta) & BOBBLE_MASK;

    /* ---- Clump (footstep sound trigger) ---- */
    /* clumptime accumulates movement; when bit 12+ overflows, trigger sound */
    /* TODO: actual clump timing once audio works */

    /* ---- Calculate movement vector ---- */
    /* Forward: x -= sin*fwd, z -= cos*fwd */
    int32_t sin_val = plr->s_sinval;
    int32_t cos_val = plr->s_cosval;

    xspd -= sin_val * fwd;
    zspd -= cos_val * fwd;

    /* Strafe: x -= cos*strafe, z += sin*strafe */
    xspd -= cos_val * strafe;
    zspd += sin_val * strafe;

    /* ---- Apply velocity to position ---- */
    plr->s_xspdval = xspd;
    plr->s_zspdval = zspd;
    plr->s_xoff += xspd;
    plr->s_zoff += zspd;

    /* ---- Fire handling ---- */
    if (plr->fire) {
        /* Fire was pressed last frame */
        if (key_map[keys->fire]) {
            plr->fire = -1;  /* still held */
        } else {
            plr->fire = 0;   /* released */
        }
    } else {
        /* Fire was not pressed last frame */
        if (key_map[keys->fire]) {
            plr->clicked = -1;  /* new click */
            plr->fire = -1;
        }
    }
}

/* -----------------------------------------------------------------------
 * Mouse control
 *
 * Translated from Plr1Control.s PLR1_mouse_control (~line 13-138)
 * ----------------------------------------------------------------------- */
static void player_mouse_control(PlayerState *plr, const uint8_t *key_map,
                                  const KeyBindings *keys, int16_t temp_frames)
{
    MouseState mouse;
    input_read_mouse(&mouse);

    /* Angle from mouse X - mouse delta self-scales (accumulates between ticks) */
    int16_t angpos = plr->s_angpos;
    angpos += mouse.dx;

    /* Forward/backward from mouse Y - also self-scaling */
    int16_t fwd_delta = mouse.dy;

    /* Clamp */
    if (fwd_delta > MOUSE_CLAMP) fwd_delta = MOUSE_CLAMP;
    if (fwd_delta < -MOUSE_CLAMP) fwd_delta = -MOUSE_CLAMP;

    /* Halve when ducked */
    if (plr->ducked) {
        fwd_delta >>= 1;
    }

    int16_t move_mag = fwd_delta << 4;

    /* Angle and sin/cos */
    angpos &= 8190;
    plr->s_angpos = angpos;
    plr->s_sinval = sin_lookup(angpos);
    plr->s_cosval = cos_lookup(angpos);

    /* Velocity friction (mouse: ~1/2 removed per tick) */
    int32_t xspd = plr->s_xspdval - apply_friction(plr->s_xspdval, 1);
    int32_t zspd = plr->s_zspdval - apply_friction(plr->s_zspdval, 1);

    /* Strafe from keyboard (lrs variable in original) - scale by temp_frames */
    int16_t lrs = 0;
    if (key_map[keys->sidestep_left])  lrs += 2 * temp_frames;
    if (key_map[keys->sidestep_right]) lrs -= 2 * temp_frames;

    /* Calculate movement */
    int32_t sin_val = plr->s_sinval;
    int32_t cos_val = plr->s_cosval;

    /* Strafe component */
    int32_t sx = -cos_val * lrs;
    int32_t sz = sin_val * lrs;

    /* Forward component */
    int32_t fx = sin_val * fwd_delta;
    int32_t fz = cos_val * fwd_delta;

    xspd = xspd - fx - sx;
    zspd = zspd - fz + sz;

    /* Apply */
    plr->s_xspdval = xspd;
    plr->s_zspdval = zspd;
    plr->s_xoff += xspd;
    plr->s_zoff += zspd;

    /* Bobble */
    plr->bobble = (plr->bobble + move_mag) & BOBBLE_MASK;

    /* Fire from mouse button */
    if (plr->fire) {
        if (mouse.left_button) {
            plr->fire = -1;
        } else {
            plr->fire = 0;
        }
    } else {
        if (mouse.left_button) {
            plr->clicked = -1;
            plr->fire = -1;
        }
    }

    (void)keys; /* some keys used above directly */
}

/* -----------------------------------------------------------------------
 * Mouse + Keyboard control (modern FPS style)
 *
 * Mouse X controls turning (horizontal look).
 * WASD / arrow keys control movement and strafing.
 * Mouse left button or fire key fires weapon.
 * Shift to run.
 * ----------------------------------------------------------------------- */
#define MOUSE_SENSITIVITY 3  /* angle units per SDL pixel; tune to taste */

static void player_mouse_kbd_control(PlayerState *plr, const uint8_t *key_map,
                                      const KeyBindings *keys, int16_t temp_frames)
{
    MouseState mouse;
    input_read_mouse(&mouse);

    int16_t move_speed = WALK_SPEED;

    /* Running (shift) */
    if (key_map[keys->run]) {
        move_speed = RUN_SPEED;
    }

    /* Ducked halves speed */
    if (plr->ducked) {
        move_speed >>= 1;
    }

    /* Scale by temp_frames: on the Amiga, speed = temp_frames * walk_speed.
     * This gives larger per-tick deltas so the collision system works. */
    move_speed *= temp_frames;

    /* ---- Turning from mouse X ---- */
    /* Mouse delta self-scales (accumulates between game ticks) */
    int16_t angpos = plr->s_angpos;
    angpos += (int16_t)(mouse.dx * MOUSE_SENSITIVITY);
    angpos &= ANGLE_MASK;
    plr->s_angpos = angpos;
    plr->s_sinval = sin_lookup(angpos);
    plr->s_cosval = cos_lookup(angpos);

    /* ---- Velocity friction (~1/8 removed per tick).
     * Plr1WasdControl.s uses asr.l #3 for mouse+kbd, same as keyboard mode. */
    int32_t xspd = plr->s_xspdval - apply_friction(plr->s_xspdval, 3);
    int32_t zspd = plr->s_zspdval - apply_friction(plr->s_zspdval, 3);

    /* ---- Forward / backward (W/S or arrow up/down) ---- */
    int16_t fwd = 0;
    if (key_map[keys->forward]) {
        fwd = -move_speed;
    }
    if (key_map[keys->backward]) {
        fwd = move_speed;
    }

    /* ---- Strafe (A/D or . / keys) ---- */
    int16_t strafe = 0;
    if (key_map[keys->sidestep_left]) {
        strafe = (move_speed * 2 + 1) / 2;   /* match original 1.5x strafe */
    }
    if (key_map[keys->sidestep_right]) {
        strafe = -((move_speed * 2 + 1) / 2);
    }

    /* Also allow arrow left/right for turning when mouse isn't moved */
    if (key_map[keys->turn_left]) {
        int16_t ang = plr->s_angpos;
        ang -= TURN_STEP * 2 * temp_frames;
        ang &= ANGLE_MASK;
        plr->s_angpos = ang;
        plr->s_sinval = sin_lookup(ang);
        plr->s_cosval = cos_lookup(ang);
    }
    if (key_map[keys->turn_right]) {
        int16_t ang = plr->s_angpos;
        ang += TURN_STEP * 2 * temp_frames;
        ang &= ANGLE_MASK;
        plr->s_angpos = ang;
        plr->s_sinval = sin_lookup(ang);
        plr->s_cosval = cos_lookup(ang);
    }

    /* ---- Bobble ---- */
    int16_t bob_delta = fwd << 6;
    plr->bobble = (plr->bobble + bob_delta) & BOBBLE_MASK;

    /* ---- Movement vector ---- */
    int32_t sin_val = plr->s_sinval;
    int32_t cos_val = plr->s_cosval;

    /* Forward: x -= sin*fwd, z -= cos*fwd */
    xspd -= sin_val * fwd;
    zspd -= cos_val * fwd;

    /* Strafe: x -= cos*strafe, z += sin*strafe */
    xspd -= cos_val * strafe;
    zspd += sin_val * strafe;

    /* ---- Apply velocity ---- */
    plr->s_xspdval = xspd;
    plr->s_zspdval = zspd;
    plr->s_xoff += xspd;
    plr->s_zoff += zspd;

    /* ---- Fire from mouse button or keyboard fire key ---- */
    bool fire_pressed = mouse.left_button || key_map[keys->fire];
    if (plr->fire) {
        if (fire_pressed) {
            plr->fire = -1;  /* still held */
        } else {
            plr->fire = 0;   /* released */
        }
    } else {
        if (fire_pressed) {
            plr->clicked = -1;  /* new click */
            plr->fire = -1;
        }
    }
}

/* -----------------------------------------------------------------------
 * Top-level player 1/2 simulation control
 *
 * This runs the input -> movement -> fall pipeline.
 * The PLR1_Control in AB3DI.s then takes the simulation results
 * and applies collision/room-transition. That second stage is below.
 * ----------------------------------------------------------------------- */
static void player_sim_control(PlayerState *plr, const ControlMode *ctrl,
                                const uint8_t *key_map, const KeyBindings *keys,
                                int16_t temp_frames)
{
    static uint8_t old_space = 0;

    /* Always-active keys */
    player_always_keys(plr, key_map, keys, &old_space);

    /* Dispatch based on control mode */
    if (ctrl->mouse_kbd) {
        player_mouse_kbd_control(plr, key_map, keys, temp_frames);
    } else if (ctrl->mouse) {
        player_mouse_control(plr, key_map, keys, temp_frames);
    } else if (ctrl->keys) {
        player_keyboard_control(plr, key_map, keys, temp_frames);
    } else if (ctrl->joy) {
        player_keyboard_control(plr, key_map, keys, temp_frames);
    } else {
        /* Default: mouse + keyboard */
        player_mouse_kbd_control(plr, key_map, keys, temp_frames);
    }
}

/* -----------------------------------------------------------------------
 * PLR1_Control / PLR2_Control - Full control + collision + room update
 *
 * Translated from AB3DI.s PLR1_Control (~line 2972-3180)
 *
 * 1. Snapshot old/new positions
 * 2. Calculate sin/cos and bobble
 * 3. Check teleport
 * 4. Object-to-object collision
 * 5. MoveObject (room transitions, wall collision)
 * 6. Update room data, floor heights, graph rooms
 * ----------------------------------------------------------------------- */
static void player_full_control(PlayerState *plr, GameState *state, int plr_num)
{
    MoveContext ctx;
    move_context_init(&ctx);

    /* 1. Snapshot positions (16.16 fixed-point) */
    plr->oldxoff = plr->xoff;
    plr->oldzoff = plr->zoff;

    /* Copy simulation position to actual (both in 16.16 fixed-point).
     * On the Amiga (AB3DI.s PLR1_Control ~line 2984):
     *   move.l PLR1s_xoff,PLR1_xoff
     * PLR1s_xoff is updated by PLR1s_Control (the sim) before this point. */
    plr->xoff = plr->s_xoff;
    plr->zoff = plr->s_zoff;

    /* For collision_check (object-to-object): use integer coordinates.
     * The Amiga does this via .w operations on big-endian 32-bit values. */
    ctx.oldx = plr->oldxoff >> 16;
    ctx.oldz = plr->oldzoff >> 16;
    ctx.newx = plr->xoff >> 16;
    ctx.newz = plr->zoff >> 16;

    plr->height = plr->p_height;

    /* Movement delta (integer for collision_check) */
    ctx.xdiff = ctx.newx - ctx.oldx;
    ctx.zdiff = ctx.newz - ctx.oldz;

    /* 2. Angle and sin/cos */
    int16_t angpos = plr->p_angpos;
    plr->angpos = angpos;
    plr->sinval = sin_lookup(angpos);
    plr->cosval = cos_lookup(angpos);

    /* 3. Bobble -> Y offset adjustment */
    int16_t bob_sin = sin_lookup(plr->p_bobble);
    int16_t bob_val = bob_sin;
    if (bob_val > 0) bob_val = -bob_val;
    bob_val += 16384;
    bob_val >>= 4;
    if (!plr->ducked) {
        bob_val *= 2;
    }

    int32_t yoff = plr->p_yoff;
    yoff += bob_val;
    plr->yoff = yoff;
    ctx.newy = yoff;
    ctx.oldy = yoff;

    ctx.thing_height = plr->height - bob_val;

    /* Step values */
    ctx.step_up_val = plr->ducked ? STEP_UP_DUCKED : STEP_UP_NORMAL;
    ctx.step_down_val = STEP_DOWN_DEFAULT;

    /* 4a. Set objroom from current zone (required for move_object wall checks).
     *     Prefer zone so that after transitioning we immediately use the new
     *     zone's walls; keep roompt in sync so zone is updated from it. */
    ctx.objroom = NULL;
    if (state->level.data && state->level.zone_adds && plr->zone >= 0 &&
        plr->zone < state->level.num_zones) {
        int32_t zone_off = (int32_t)(
            (state->level.zone_adds[plr->zone * 4    ] << 24) |
            (state->level.zone_adds[plr->zone * 4 + 1] << 16) |
            (state->level.zone_adds[plr->zone * 4 + 2] <<  8) |
             state->level.zone_adds[plr->zone * 4 + 3]);
        ctx.objroom = state->level.data + zone_off;
    } else if (state->level.data && plr->roompt >= 0) {
        ctx.objroom = state->level.data + plr->roompt;
    }

    /* 4b. Teleport check (uses integer ctx) */
    if (plr->zone >= 0) {
        if (check_teleport(&ctx, &state->level, plr->zone)) {
            /* Teleport destination is in integer coords; sync to 16.16 */
            plr->xoff = (int32_t)ctx.newx << 16;
            plr->zoff = (int32_t)ctx.newz << 16;
            plr->s_xoff = plr->xoff;
            plr->s_zoff = plr->zoff;
        }
    }

    /* 5. Object collision check.
     * coll_id must be set to the player's OWN object CID so that
     * collision_check skips the player object.  Without this, the player
     * collides with themselves every frame and can never move. */
    {
        uint8_t *plr_obj_ptr = (plr_num == 1) ?
            state->level.plr1_obj : state->level.plr2_obj;
        if (plr_obj_ptr) {
            GameObject *plr_obj = (GameObject*)plr_obj_ptr;
            ctx.coll_id = OBJ_CID(plr_obj);
        } else {
            ctx.coll_id = -1;
        }
    }
    /* CollideFlags from AB3DI.s PLR1_Control / PLR2_Control:
     *   P1: %1011111110111000001 = 0x5FDC1  (excludes PLR1 bit 5)
     *   P2: %1011111010111100001 = 0x5F5E1  (excludes PLR2 bit 11) */
    ctx.collide_flags = (plr_num == 1) ? 0x5FDC1 : 0x5F5E1;
    ctx.wall_flags = (plr_num == 1) ? 0x100 : 0x800;
    ctx.stood_in_top = plr->stood_in_top;

    collision_check(&ctx, &state->level);

    /* On object collision, revert to old position.
     * The Amiga (AB3DI.s ~line 3119-3123):
     *   move.w oldx,PLR1_xoff        ; Writes integer part only (high word)
     *   move.w oldz,PLR1_zoff        ; Low word (sim fraction) is PRESERVED
     *   move.l PLR1_xoff,PLR1s_xoff  ; Full 32-bit sync
     *   bra.b  .cantmove             ; SKIP MoveObject!
     *
     * Note: the Amiga uses move.w which only overwrites the high word
     * (integer part) while preserving the low word (fractional part from
     * the sim).  This means sub-pixel velocity accumulates even when
     * blocked by an object.
     *
     * Also note: when Collision hits, the Amiga SKIPS MoveObject entirely
     * (branches to .cantmove).  In our code, we still call move_object
     * but xdiff=zdiff=0 causes it to return immediately, which is equivalent. */
    if (ctx.hitwall) {
        /* Revert integer part only, preserve sim fraction (Amiga move.w semantics) */
        plr->xoff = (plr->oldxoff & (int32_t)0xFFFF0000) | (plr->xoff & 0x0000FFFF);
        plr->zoff = (plr->oldzoff & (int32_t)0xFFFF0000) | (plr->zoff & 0x0000FFFF);
        plr->s_xoff = plr->xoff;
        plr->s_zoff = plr->zoff;
    }

    /* 6. MoveObject - room transitions and wall sliding.
     *
     * CRITICAL: The Amiga MoveObject uses move.w (16-bit word) reads on
     * the big-endian 32-bit position values, extracting only the INTEGER
     * part (high word).  All cross products, wall slides, and distance
     * calculations happen in integer space.
     *
     * Using full 16.16 fixed-point (pos_shift=16) causes sub-pixel wall
     * crossings that the original game would never detect, resulting in
     * the player getting stuck against walls.  We match the Amiga by
     * passing integer coordinates (pos_shift=0). */
    ctx.pos_shift = 0;
    ctx.oldx = plr->oldxoff >> 16;   /* Integer part (Amiga: move.w oldx) */
    ctx.oldz = plr->oldzoff >> 16;
    ctx.newx = plr->xoff >> 16;      /* Integer part (Amiga: move.w newx) */
    ctx.newz = plr->zoff >> 16;
    ctx.xdiff = ctx.newx - ctx.oldx;
    ctx.zdiff = ctx.newz - ctx.oldz;
    ctx.extlen = 40;
    ctx.awayfromwall = 0;  /* AB3DI.s PLR1_Control: move.b #0,awayfromwall */
    ctx.exitfirst = 0;
    ctx.wallbounce = 0;

    /* Run wall/room collision in small integer sub-steps to avoid tunneling
     * through thin walls/corners when per-tick velocity gets high. */
    {
        int32_t start_x = ctx.oldx;
        int32_t start_z = ctx.oldz;
        int32_t target_x = ctx.newx;
        int32_t target_z = ctx.newz;
        int32_t dx = target_x - start_x;
        int32_t dz = target_z - start_z;
        int32_t adx = (dx < 0) ? -dx : dx;
        int32_t adz = (dz < 0) ? -dz : dz;
        int32_t maxd = (adx > adz) ? adx : adz;

        /* 4 world units per sub-step so we don't tunnel through walls. */
        int steps = (int)(maxd / 4) + 1;
        if (steps > 32) steps = 32;

        /* Stairs: for one frame after a zone change, don't allow stepping back into the previous zone. */
        ctx.no_transition_back = (plr->no_transition_back_roompt >= 0 && state->level.data) ?
            (state->level.data + plr->no_transition_back_roompt) : NULL;
        plr->no_transition_back_roompt = -1;  /* use once then clear */

        int32_t cur_x = start_x;
        int32_t cur_z = start_z;
        uint8_t *cur_room = ctx.objroom;
        int8_t cur_top = ctx.stood_in_top;
        int8_t any_hit = 0;

        for (int i = 1; i <= steps; i++) {
            uint8_t *room_before = cur_room;
            MoveContext step = ctx;
            step.objroom = cur_room;
            step.stood_in_top = cur_top;
            step.oldx = cur_x;
            step.oldz = cur_z;
            step.newx = start_x + (dx * i) / steps;
            step.newz = start_z + (dz * i) / steps;
            step.xdiff = step.newx - step.oldx;
            step.zdiff = step.newz - step.oldz;

            move_object(&step, &state->level);

            if (step.hitwall) any_hit = 1;
            cur_x = step.newx;
            cur_z = step.newz;
            cur_room = step.objroom;
            cur_top = step.stood_in_top;
            if (step.hitwall) any_hit = 1;

            /* One zone transition per frame so stairs climb step-by-step. After
             * transitioning, validate position in the new zone so we don't end up
             * inside a wall (wall clipping) or trigger phantom walls next frame. */
            if (cur_room != room_before) {
                /* Run move_object again in the new zone so we check the new zone's
                 * walls and slide/revert if we stepped into a wall. */
                MoveContext step2 = ctx;
                step2.objroom = cur_room;
                step2.stood_in_top = cur_top;
                step2.oldx = cur_x;
                step2.oldz = cur_z;
                step2.newx = start_x + (dx * i) / steps;
                step2.newz = start_z + (dz * i) / steps;
                step2.xdiff = step2.newx - step2.oldx;
                step2.zdiff = step2.newz - step2.oldz;
                move_object(&step2, &state->level);
                cur_x = step2.newx;
                cur_z = step2.newz;
                cur_room = step2.objroom;
                cur_top = step2.stood_in_top;
                if (step2.hitwall) any_hit = 1;
                plr->no_transition_back_roompt = (int32_t)(room_before - state->level.data);
                break;
            }
        }

        ctx.newx = cur_x;
        ctx.newz = cur_z;
        ctx.objroom = cur_room;
        ctx.stood_in_top = cur_top;
        ctx.hitwall = any_hit;
        if (any_hit) {
            int16_t zone_id = -1;
            int32_t roompt = -1;
            if (ctx.objroom && state->level.data) {
                roompt = (int32_t)(ctx.objroom - state->level.data);
                zone_id = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
                if (zone_id < 0 || zone_id >= state->level.num_zones)
                    zone_id = -1;
            }
            printf("[PLAYER] wall collision plr%d at (%d, %d) zone=%d roompt=%ld\n",
                   plr_num, (int)ctx.newx, (int)ctx.newz, (int)zone_id, (long)roompt);
        }
    }

    plr->stood_in_top = ctx.stood_in_top;

    /* Remember zone before we apply move_object result (detect zone transition) */
    int16_t prev_zone = plr->zone;

    /* Update roompt and zone from move_object result.
     * ctx.objroom may have changed if the player crossed into a new zone.
     * We use zone to set objroom next frame, so zone must stay in sync. */
    if (ctx.objroom && state->level.data) {
        plr->roompt = (int32_t)(ctx.objroom - state->level.data);

        /* Update plr->zone from the room data so next frame we use the right zone's walls.
         * Zone data offset 0 contains the zone number (word).
         * Translated from AB3DI.s line ~3100: Roompt -> zone lookup */
        int16_t new_zone = (int16_t)(
            (ctx.objroom[0] << 8) | ctx.objroom[1]);
        if (new_zone >= 0 && new_zone < state->level.num_zones) {
            plr->zone = new_zone;
        } else if (state->level.zone_adds) {
            /* Fallback: some levels have wrong/missing zone id in room data.
             * Derive zone from roompt so wall collision uses the correct room. */
            int32_t roompt = plr->roompt;
            for (int16_t z = 0; z < state->level.num_zones; z++) {
                int32_t zoff = (int32_t)(
                    (state->level.zone_adds[z * 4    ] << 24) |
                    (state->level.zone_adds[z * 4 + 1] << 16) |
                    (state->level.zone_adds[z * 4 + 2] <<  8) |
                     state->level.zone_adds[z * 4 + 3]);
                if (zoff == roompt) {
                    plr->zone = z;
                    break;
                }
            }
        }
    }

    /* Write MoveObject's integer result back to the 16.16 position.
     * The Amiga (AB3DI.s ~line 3137-3140):
     *   move.w newx,PLR1_xoff       ; Write integer to HIGH WORD only
     *   move.w newz,PLR1_zoff       ; Low word (sim fraction) preserved
     *   move.l PLR1_xoff,PLR1s_xoff ; Full 32-bit sync
     *
     * This preserves the fractional part from the simulation while
     * updating only the integer part from MoveObject's wall-slid result. */
    plr->xoff = ((int32_t)ctx.newx << 16) | (plr->xoff & 0x0000FFFF);
    plr->zoff = ((int32_t)ctx.newz << 16) | (plr->zoff & 0x0000FFFF);
    plr->s_xoff = plr->xoff;
    plr->s_zoff = plr->zoff;

    /* 6b. Auto-duck if room is too low (AB3DI.s line ~1355-1374) */
    if (state->level.zone_adds && state->level.data && plr->zone >= 0) {
        const uint8_t *za2 = state->level.zone_adds;
        int z2 = plr->zone;
        int32_t zoff2 = (int32_t)((za2[z2*4]<<24)|(za2[z2*4+1]<<16)|
                        (za2[z2*4+2]<<8)|za2[z2*4+3]);
        const uint8_t *zd2 = state->level.data + zoff2;
        int zd_off2 = plr->stood_in_top ? 8 : 0;
        int32_t fh = (int32_t)((zd2[2+zd_off2]<<24)|(zd2[3+zd_off2]<<16)|
                     (zd2[4+zd_off2]<<8)|zd2[5+zd_off2]);
        int32_t rh = (int32_t)((zd2[6+zd_off2]<<24)|(zd2[7+zd_off2]<<16)|
                     (zd2[8+zd_off2]<<8)|zd2[9+zd_off2]);
        int32_t room_ht = fh - rh;
        if (room_ht < plr->s_targheight + 3 * 1024) {
            plr->s_targheight = PLAYER_CROUCHED;
        }
    }

    /* 7. Update target Y from room floor height
     * AND set up PointsToRotatePtr / ListOfGraphRooms for this zone.
     *
     * Translated from AB3DI.s .cantmove (~line 3145-3178):
     *   move.l PLR1_Roompt,a0
     *   move.l ToZoneFloor(a0),d0       ; floor height
     *   ...
     *   adda.w #ToZonePts,a0            ; a0 = zone_data + 34
     *   move.w (a0)+,d1                 ; d1 = relative offset to points list
     *   ext.l  d1
     *   add.l  PLR1_Roompt,d1
     *   move.l d1,PLR1_PointsToRotatePtr
     *   tst.w  (a0)+                    ; backdrop flag at offset 36
     *   ...
     *   adda.w #10,a0                   ; a0 = zone_data + 48
     *   move.l a0,PLR1_ListOfGraphRooms
     */
    if (state->level.zone_adds && state->level.data && plr->zone >= 0) {
        const uint8_t *za = state->level.zone_adds;
        int z = plr->zone;
        int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|
                       (za[z*4+2]<<8)|za[z*4+3]);
        const uint8_t *zd = state->level.data + zoff;
        int zd_off = plr->stood_in_top ? 8 : 0;
        int32_t floor_h = (int32_t)((zd[2+zd_off]<<24)|(zd[3+zd_off]<<16)|
                          (zd[4+zd_off]<<8)|zd[5+zd_off]);
        plr->s_tyoff = floor_h - plr->s_height;

        /* When we just transitioned zones (e.g. stepped onto stairs), snap Y to new floor
         * immediately so we stand on the step instead of smoothing through it. */
        if (plr->zone != prev_zone && plr->s_tyoff < plr->s_yoff) {
            plr->s_yoff = plr->s_tyoff;
            plr->s_yvel = 0;
        }

        /* PointsToRotatePtr = roompt + (int16_t)(zone_data[34..35])
         * ToZonePts (offset 34) contains a 16-bit relative offset. */
        int16_t pts_rel = (int16_t)((zd[34] << 8) | zd[35]);
        plr->points_to_rotate_ptr = zoff + (int32_t)pts_rel;

        /* ListOfGraphRooms = zone_data + 48 (ToListOfGraph) */
        plr->list_of_graph_rooms = zoff + 48;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Default key bindings (matching original ControlLoop.s CONTROLBUFFER) */
static const KeyBindings default_keys = {
    .turn_left      = 0x4F,  /* Cursor Left */
    .turn_right     = 0x4E,  /* Cursor Right */
    .forward        = 0x4C,  /* Cursor Up */
    .backward       = 0x4D,  /* Cursor Down */
    .fire           = 0x65,  /* Right Alt */
    .operate        = 0x40,  /* Space */
    .run            = 0x61,  /* Right Shift */
    .force_sidestep = 0x67,  /* Right Amiga */
    .sidestep_left  = 0x39,  /* . > */
    .sidestep_right = 0x3A,  /* / ? */
    .duck           = 0x22,  /* D */
    .look_behind    = 0x28,  /* L */
};

void player1_snapshot(GameState *state)
{
    PlayerState *p = &state->plr1;
    /* Convert 16.16 fixed-point to integer for object/AI code.
     * On the Amiga, .w reads the high word of the 32-bit position. */
    p->p_xoff        = p->s_xoff >> 16;
    p->p_zoff        = p->s_zoff >> 16;
    p->p_yoff        = p->s_yoff;
    p->p_height      = p->s_height;
    p->p_angpos      = p->s_angpos;
    p->p_bobble      = p->bobble;
    p->p_clicked     = p->clicked;
    p->clicked       = 0;
    p->p_fire        = p->fire;
    p->p_spctap      = p->spctap;
    p->spctap        = 0;
    p->p_ducked      = p->ducked;
    p->p_gunselected = (int8_t)p->gun_selected;
}

void player2_snapshot(GameState *state)
{
    PlayerState *p = &state->plr2;
    /* Convert 16.16 fixed-point to integer for object/AI code. */
    p->p_xoff        = p->s_xoff >> 16;
    p->p_zoff        = p->s_zoff >> 16;
    p->p_yoff        = p->s_yoff;
    p->p_height      = p->s_height;
    p->p_angpos      = p->s_angpos;
    p->p_bobble      = p->bobble;
    p->p_clicked     = p->clicked;
    p->clicked       = 0;
    p->p_fire        = p->fire;
    p->p_spctap      = p->spctap;
    p->spctap        = 0;
    p->p_ducked      = p->ducked;
    p->p_gunselected = (int8_t)p->gun_selected;
}

void player1_control(GameState *state)
{
    /* Phase 1: Simulation (input -> velocity -> position) */
    player_sim_control(&state->plr1, &state->plr1_control,
                       state->key_map, &default_keys, state->temp_frames);

    /* Phase 2: Falling (with water level from zone data) */
    {
        int32_t water_level = 0;
        bool in_water = false;
        if (state->level.zone_adds && state->level.data && state->plr1.zone >= 0) {
            const uint8_t *za = state->level.zone_adds;
            int z = state->plr1.zone;
            int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|
                           (za[z*4+2]<<8)|za[z*4+3]);
            const uint8_t *zd = state->level.data + zoff;
            water_level = (int32_t)((zd[18]<<24)|(zd[19]<<16)|(zd[20]<<8)|zd[21]);
            in_water = (water_level != 0);
        }
        player_fall(&state->plr1.s_yoff, &state->plr1.s_yvel,
                    state->plr1.s_tyoff, water_level, in_water);
    }

    /* Phase 3: Snapshot current sim state for full control + shooting.
     * In the original engine this copy happens after control/fall and
     * before PLR1_Control consumes p1_* values. */
    player1_snapshot(state);

    /* Phase 4: Collision + room update */
    player_full_control(&state->plr1, state, 1);
}

void player2_control(GameState *state)
{
    player_sim_control(&state->plr2, &state->plr2_control,
                       state->key_map, &default_keys, state->temp_frames);

    {
        int32_t water_level = 0;
        bool in_water = false;
        if (state->level.zone_adds && state->level.data && state->plr2.zone >= 0) {
            const uint8_t *za = state->level.zone_adds;
            int z = state->plr2.zone;
            int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|
                           (za[z*4+2]<<8)|za[z*4+3]);
            const uint8_t *zd = state->level.data + zoff;
            water_level = (int32_t)((zd[18]<<24)|(zd[19]<<16)|(zd[20]<<8)|zd[21]);
            in_water = (water_level != 0);
        }
        player_fall(&state->plr2.s_yoff, &state->plr2.s_yvel,
                    state->plr2.s_tyoff, water_level, in_water);
    }

    /* Match original ordering: snapshot after sim/fall, before full control. */
    player2_snapshot(state);

    player_full_control(&state->plr2, state, 2);
}

/* Debug save file: debug_save.bin under data directory. Binary format: magic "AB3D", then per player:
 * xoff(int32), zoff(int32), zone(int16), angpos(int16), yoff(int32). Native byte order. */
#define DEBUG_SAVE_MAGIC "AB3D"
#define DEBUG_SAVE_SUBPATH "debug_save.bin"

void player_debug_save_position(GameState *state)
{
    char path[512];
    io_make_data_path(path, sizeof(path), DEBUG_SAVE_SUBPATH);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("[PLAYER] debug save: could not open %s for write\n", path);
        return;
    }
    if (fwrite(DEBUG_SAVE_MAGIC, 1, 4, f) != 4) goto fail;
    if (fwrite(&state->plr1.xoff, sizeof(state->plr1.xoff), 1, f) != 1) goto fail;
    if (fwrite(&state->plr1.zoff, sizeof(state->plr1.zoff), 1, f) != 1) goto fail;
    if (fwrite(&state->plr1.zone, sizeof(state->plr1.zone), 1, f) != 1) goto fail;
    if (fwrite(&state->plr1.angpos, sizeof(state->plr1.angpos), 1, f) != 1) goto fail;
    if (fwrite(&state->plr1.yoff, sizeof(state->plr1.yoff), 1, f) != 1) goto fail;
    if (fwrite(&state->plr2.xoff, sizeof(state->plr2.xoff), 1, f) != 1) goto fail;
    if (fwrite(&state->plr2.zoff, sizeof(state->plr2.zoff), 1, f) != 1) goto fail;
    if (fwrite(&state->plr2.zone, sizeof(state->plr2.zone), 1, f) != 1) goto fail;
    if (fwrite(&state->plr2.angpos, sizeof(state->plr2.angpos), 1, f) != 1) goto fail;
    if (fwrite(&state->plr2.yoff, sizeof(state->plr2.yoff), 1, f) != 1) goto fail;
    fclose(f);
    printf("[PLAYER] debug save: position/orientation written to %s\n", path);
    return;
fail:
    fclose(f);
    printf("[PLAYER] debug save: write failed\n");
}

#ifndef NDEBUG
/* In debug builds: load debug_save.bin and override player start position/orientation. */
static void player_debug_load_position_if_present(GameState *state)
{
    char path[512];
    io_make_data_path(path, sizeof(path), DEBUG_SAVE_SUBPATH);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, DEBUG_SAVE_MAGIC, 4) != 0) {
        fclose(f);
        return;
    }
    if (fread(&state->plr1.xoff, sizeof(state->plr1.xoff), 1, f) != 1) goto load_fail;
    if (fread(&state->plr1.zoff, sizeof(state->plr1.zoff), 1, f) != 1) goto load_fail;
    if (fread(&state->plr1.zone, sizeof(state->plr1.zone), 1, f) != 1) goto load_fail;
    if (fread(&state->plr1.angpos, sizeof(state->plr1.angpos), 1, f) != 1) goto load_fail;
    if (fread(&state->plr1.yoff, sizeof(state->plr1.yoff), 1, f) != 1) goto load_fail;
    if (fread(&state->plr2.xoff, sizeof(state->plr2.xoff), 1, f) != 1) goto load_fail;
    if (fread(&state->plr2.zoff, sizeof(state->plr2.zoff), 1, f) != 1) goto load_fail;
    if (fread(&state->plr2.zone, sizeof(state->plr2.zone), 1, f) != 1) goto load_fail;
    if (fread(&state->plr2.angpos, sizeof(state->plr2.angpos), 1, f) != 1) goto load_fail;
    if (fread(&state->plr2.yoff, sizeof(state->plr2.yoff), 1, f) != 1) goto load_fail;
    fclose(f);
    /* Sync sim position, angle and Y from saved */
    state->plr1.s_xoff = state->plr1.xoff;
    state->plr1.s_zoff = state->plr1.zoff;
    state->plr1.s_yoff = state->plr1.yoff;
    state->plr1.s_angpos = state->plr1.angpos;
    state->plr2.s_xoff = state->plr2.xoff;
    state->plr2.s_zoff = state->plr2.zoff;
    state->plr2.s_yoff = state->plr2.yoff;
    state->plr2.s_angpos = state->plr2.angpos;
    /* Re-apply zone-derived state (roompt, points_to_rotate_ptr, list_of_graph_rooms) */
    if (state->level.zone_adds && state->plr1.zone >= 0 && state->plr1.zone < state->level.num_zones) {
        const uint8_t *za = state->level.zone_adds;
        int z = state->plr1.zone;
        int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|(za[z*4+2]<<8)|za[z*4+3]);
        const uint8_t *zd = state->level.data + zoff;
        state->plr1.s_tyoff = (int32_t)((zd[2]<<24)|(zd[3]<<16)|(zd[4]<<8)|zd[5]) - PLAYER_HEIGHT;
        state->plr1.roompt = zoff;
        int16_t pts1 = (int16_t)((zd[34] << 8) | zd[35]);
        state->plr1.points_to_rotate_ptr = zoff + (int32_t)pts1;
        state->plr1.list_of_graph_rooms = zoff + 48;
    }
    if (state->level.zone_adds && state->plr2.zone >= 0 && state->plr2.zone < state->level.num_zones) {
        const uint8_t *za = state->level.zone_adds;
        int z = state->plr2.zone;
        int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|(za[z*4+2]<<8)|za[z*4+3]);
        const uint8_t *zd = state->level.data + zoff;
        state->plr2.s_tyoff = (int32_t)((zd[2]<<24)|(zd[3]<<16)|(zd[4]<<8)|zd[5]) - PLAYER_HEIGHT;
        state->plr2.roompt = zoff;
        int16_t pts2 = (int16_t)((zd[34] << 8) | zd[35]);
        state->plr2.points_to_rotate_ptr = zoff + (int32_t)pts2;
        state->plr2.list_of_graph_rooms = zoff + 48;
    }
    printf("[PLAYER] debug load: position/orientation overridden from %s\n", path);
    return;
load_fail:
    fclose(f);
}
#endif

void player_init_from_level(GameState *state)
{
    /* Default values */
    state->plr1.xoff = 0;
    state->plr1.zoff = 0;
    state->plr1.yoff = -PLAYER_HEIGHT;
    state->plr1.s_xoff = 0;
    state->plr1.s_zoff = 0;
    state->plr1.s_yoff = -PLAYER_HEIGHT;
    state->plr1.s_height = PLAYER_HEIGHT;
    state->plr1.s_targheight = PLAYER_HEIGHT;

    state->plr2.xoff = 0;
    state->plr2.zoff = 0;
    state->plr2.yoff = -PLAYER_HEIGHT;
    state->plr2.s_xoff = 0;
    state->plr2.s_zoff = 0;
    state->plr2.s_yoff = -PLAYER_HEIGHT;
    state->plr2.s_height = PLAYER_HEIGHT;
    state->plr2.s_targheight = PLAYER_HEIGHT;

    if (state->level.data) {
        /* Parse start positions from level data header (Amiga format).
         * Real Amiga header layout (from AB3DI.s):
         *   Byte  0: PLR1 start X (word)
         *   Byte  2: PLR1 start Z (word)
         *   Byte  4: PLR1 start zone (word)
         *   Byte  6: PLR2 start X (word)
         *   Byte  8: PLR2 start Z (word)
         *   Byte 10: PLR2 start zone (word)
         *   Byte 12: num control pts (word, unused)
         *   Byte 14: num points (word)
         * Note: Player angle is NOT in the header; initialized to 0.
         */
        const uint8_t *hdr = state->level.data;

        /* Positions are stored in 16.16 fixed-point internally,
         * matching the Amiga convention (move.w to high word of dc.l).
         * Upper 16 bits = world coordinate, lower 16 bits = sub-pixel. */
        state->plr1.xoff = (int32_t)(int16_t)((hdr[0] << 8) | hdr[1]) << 16;
        state->plr1.zoff = (int32_t)(int16_t)((hdr[2] << 8) | hdr[3]) << 16;
        state->plr1.zone = (int16_t)((hdr[4] << 8) | hdr[5]);
        state->plr1.angpos = 0;  /* Not stored in real Amiga header */
        state->plr1.s_xoff = state->plr1.xoff;
        state->plr1.s_zoff = state->plr1.zoff;

        state->plr2.xoff = (int32_t)(int16_t)((hdr[6] << 8) | hdr[7]) << 16;
        state->plr2.zoff = (int32_t)(int16_t)((hdr[8] << 8) | hdr[9]) << 16;
        state->plr2.zone = (int16_t)((hdr[10] << 8) | hdr[11]);
        state->plr2.angpos = 0;  /* Not stored in real Amiga header */
        state->plr2.s_xoff = state->plr2.xoff;
        state->plr2.s_zoff = state->plr2.zoff;

        /* Set initial Y from zone floor height + initialize roompt.
         * Translated from AB3DI.s: Initial PLR1_Roompt / PLR2_Roompt setup */
        if (state->level.zone_adds && state->plr1.zone >= 0) {
            const uint8_t *za = state->level.zone_adds;
            int z = state->plr1.zone;
            int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|
                           (za[z*4+2]<<8)|za[z*4+3]);
            const uint8_t *zd = state->level.data + zoff;
            int32_t floor_h = (int32_t)((zd[2]<<24)|(zd[3]<<16)|(zd[4]<<8)|zd[5]);
            state->plr1.s_tyoff = floor_h - PLAYER_HEIGHT;
            state->plr1.yoff = floor_h - PLAYER_HEIGHT;
            state->plr1.s_yoff = state->plr1.yoff;
            state->plr1.roompt = zoff;
            /* PointsToRotatePtr = roompt + (int16) zone_data[34..35] */
            int16_t pts1 = (int16_t)((zd[34] << 8) | zd[35]);
            state->plr1.points_to_rotate_ptr = zoff + (int32_t)pts1;
            /* ListOfGraphRooms = zone_data + 48 (ToListOfGraph) */
            state->plr1.list_of_graph_rooms = zoff + 48;
        }
        if (state->level.zone_adds && state->plr2.zone >= 0) {
            const uint8_t *za = state->level.zone_adds;
            int z = state->plr2.zone;
            int32_t zoff = (int32_t)((za[z*4]<<24)|(za[z*4+1]<<16)|
                           (za[z*4+2]<<8)|za[z*4+3]);
            const uint8_t *zd = state->level.data + zoff;
            int32_t floor_h = (int32_t)((zd[2]<<24)|(zd[3]<<16)|(zd[4]<<8)|zd[5]);
            state->plr2.s_tyoff = floor_h - PLAYER_HEIGHT;
            state->plr2.yoff = floor_h - PLAYER_HEIGHT;
            state->plr2.s_yoff = state->plr2.yoff;
            state->plr2.roompt = zoff;
            /* PointsToRotatePtr = roompt + (int16) zone_data[34..35] */
            int16_t pts2 = (int16_t)((zd[34] << 8) | zd[35]);
            state->plr2.points_to_rotate_ptr = zoff + (int32_t)pts2;
            /* ListOfGraphRooms = zone_data + 48 (ToListOfGraph) */
            state->plr2.list_of_graph_rooms = zoff + 48;
        }

#ifndef NDEBUG
        /* In debug build: override start position/orientation from saved file if present */
        player_debug_load_position_if_present(state);
#endif

        printf("[PLAYER] init_from_level: PLR1 at (%d,%d) zone %d yoff=%d tyoff=%d, "
               "PLR2 at (%d,%d) zone %d\n",
               (int)(state->plr1.xoff >> 16), (int)(state->plr1.zoff >> 16), state->plr1.zone,
               state->plr1.s_yoff, state->plr1.s_tyoff,
               (int)(state->plr2.xoff >> 16), (int)(state->plr2.zoff >> 16), state->plr2.zone);

        /* Zone dump removed - see player_init log for initial values */
    } else {
        printf("[PLAYER] init_from_level (no level data - players at origin)\n");
    }
}

/* -----------------------------------------------------------------------
 * Player shooting (full implementation)
 *
 * Translated from PlayerShoot.s Player1Shot (line ~23-340).
 *
 * Flow:
 * 1. Decrement fire rate timer
 * 2. Check if player clicked/held fire
 * 3. Check ammo
 * 4. For instant weapons: auto-aim, trace, apply damage
 * 5. For projectile weapons: create bullet in PlayerShotData
 * 6. Play sound, animate gun
 * ----------------------------------------------------------------------- */
static void player_shoot_internal(GameState *state, PlayerState *plr,
                                  int plr_num, const GunDataEntry *guns)
{
    int gun_idx = plr->gun_selected;
    if (gun_idx < 0 || gun_idx >= MAX_GUNS) return;

    const GunDataEntry *gun = &guns[gun_idx];

    /* 1. Decrement fire rate timer */
    plr->time_to_shoot -= state->temp_frames;
    if (plr->time_to_shoot > 0) return;

    /* 2. Check fire state */
    bool fire = false;
    if (gun->click_or_hold) {
        /* Hold-down weapon: fire while held */
        fire = (plr->p_fire != 0);
    } else {
        /* Click weapon: fire on new click */
        fire = (plr->p_clicked != 0);
        plr->p_clicked = 0;
    }

    if (!fire) return;

    /* 3. Check ammo */
    int16_t ammo = plr->gun_data[gun_idx].ammo;
    if (ammo < gun->ammo_per_shot) {
        /* Click sound (empty) */
        audio_play_sample(12, 300);
        plr->time_to_shoot = 10; /* prevent spam */
        return;
    }

    /* Consume ammo */
    plr->gun_data[gun_idx].ammo -= gun->ammo_per_shot;

    /* Set fire rate delay */
    plr->time_to_shoot = gun->fire_delay;

    /* Play gun sound */
    audio_play_sample(gun->gun_sample, 64);

    /* Set gun animation frame (PLR_GunFrame = max frame, counts down) */
    if (gun_idx < 8 && gun_anims[gun_idx].num_frames > 0) {
        plr->gun_frame = (int16_t)gun_anims[gun_idx].num_frames;
    }

    /* 4. Calculate firing direction */
    int16_t sin_val = plr->sinval;
    int16_t cos_val = plr->cosval;

    /* Forward direction matches Amiga: (sin, cos) not negated */
    int32_t dir_x = sin_val;
    int32_t dir_z = cos_val;

    /* 5. Auto-aim system */
    int16_t bulyspd = 0; /* bullet Y velocity for vertical auto-aim */

    int8_t *obs_in_line = (plr_num == 1) ? plr1_obs_in_line : plr2_obs_in_line;
    int16_t *obj_dists = (plr_num == 1) ? plr1_obj_dists : plr2_obj_dists;

    /* Hitscan target flags: enemies + barrel only.
     * Pickups (ammo/medikit/key/gun) and player objects are NOT valid targets —
     * the original Amiga magic numbers had ammo(9)/key(4) bits set by mistake,
     * which caused ammo pickups to silently absorb shots and report nothing. */
    uint32_t enemy_flags =
        (1u << OBJ_NBR_ALIEN)           |  /* 0  */
        (1u << OBJ_NBR_ROBOT)           |  /* 6  */
        (1u << OBJ_NBR_BIG_NASTY)       |  /* 7  */
        (1u << OBJ_NBR_FLYING_NASTY)    |  /* 8  */
        (1u << OBJ_NBR_BARREL)          |  /* 10 */
        (1u << OBJ_NBR_MARINE)          |  /* 12 */
        (1u << OBJ_NBR_WORM)            |  /* 13 */
        (1u << OBJ_NBR_HUGE_RED_THING)  |  /* 14 */
        (1u << OBJ_NBR_SMALL_RED_THING) |  /* 15 */
        (1u << OBJ_NBR_TREE)            |  /* 16 */
        (1u << OBJ_NBR_EYEBALL)         |  /* 17 */
        (1u << OBJ_NBR_TOUGH_MARINE)    |  /* 18 */
        (1u << OBJ_NBR_FLAME_MARINE)    |  /* 19 */
        (1u << OBJ_NBR_GAS_PIPE);          /* 20 */
    (void)plr_num; /* flags are now the same for both players */

    /* Find closest target in line of fire */
    int closest_idx = -1;
    int16_t closest_dist = 32767;

    if (state->level.object_data) {
        for (int i = 0; i < state->level.num_object_points && i < MAX_OBJECTS; i++) {
            GameObject *obj = (GameObject*)(state->level.object_data + i * OBJECT_SIZE);
            int obj_type = obj->obj.number;
            if (!obs_in_line[i]) continue;
            int16_t dist = obj_dists[i];
            if (dist <= 0) continue;

            /* Check if this object type is a valid target */
            if (OBJ_ZONE(obj) < 0) continue;

            if (obj_type < 0 || obj_type > 20) continue;
            if (!(enemy_flags & (1u << obj_type))) continue;

            /* Do not target dead enemies (death animation or no lives left) */
            if (obj_type == (int)OBJ_NBR_DEAD) continue;
            if (NASTY_LIVES(*obj) <= 0 && obj_type != (int)OBJ_NBR_BARREL) continue;

            if (dist < closest_dist) {
                closest_dist = dist;
                closest_idx = i;
            }
        }
    }
    /* Calculate vertical aim toward target (PlayerShoot.s lines 99-139) */
    if (closest_idx >= 0 && closest_dist > 0) {
        /* bulyspd = (target_y - player_y + 18*256) / (dist >> bullet_speed)
         * target_y comes from ObjectPoints offset 4 (stored in obj raw[4-5]) */
        GameObject *tgt = (GameObject*)(state->level.object_data +
                          closest_idx * OBJECT_SIZE);
        int16_t target_y = (int16_t)((tgt->raw[4] << 8) | tgt->raw[5]);
        int16_t player_y = (int16_t)(plr->p_yoff >> 7);
        int32_t target_ydiff = target_y - player_y + 18 * 256;

        int shift = gun->bullet_speed;
        if (shift < 0) shift = 0;
        if (shift > 15) shift = 15;
        int32_t dist_shifted = closest_dist >> shift;
        if (dist_shifted < 1) dist_shifted = 1;

        bulyspd = (int16_t)(target_ydiff / dist_shifted);
    }

    /* 6. Fire the weapon */
    if (gun->fire_bullet != 0) {
        /* Instant-hit weapon (pistol, shotgun) */
        int num_pellets = gun->bullet_count;
        if (num_pellets < 1) num_pellets = 1;

        /* Amiga PlayerShoot.s FIREBULLETS:
         *   rand_val = GetRand() & 0x7fff
         *   scaled_dist_sq = ((ox-plr_x)^2 + (oz-plr_z)^2) >> 6
         *   hit if rand_val*2 > scaled_dist_sq
         * Same rule applies to every pellet (pistol 1x, shotgun 7x). */
        int16_t tgt_ox = 0, tgt_oz = 0;
        if (closest_idx >= 0 && state->level.object_points) {
            GameObject *tgt = (GameObject*)(state->level.object_data + closest_idx * OBJECT_SIZE);
            int16_t cid = OBJ_CID(tgt);
            if (cid >= 0) {
                const uint8_t *pt = state->level.object_points + (int)cid * 8;
                tgt_ox = (int16_t)((pt[0] << 8) | pt[1]);
                tgt_oz = (int16_t)((pt[4] << 8) | pt[5]);
            }
        }
        int32_t plr_ox = plr->p_xoff; /* already integer world units */
        int32_t plr_oz = plr->p_zoff;
        int32_t adx = tgt_ox - (int16_t)plr_ox;
        int32_t adz = tgt_oz - (int16_t)plr_oz;
        int32_t scaled_dist_sq = ((adx * adx) + (adz * adz)) >> 6;

        for (int p = 0; p < num_pellets; p++) {
            /* Amiga hit test: random [0,32767]*2 vs scaled dist² */
            int32_t rand_val = (int32_t)(rand() & 0x7fff);

            if (closest_idx >= 0) {
                /* Miss if random doesn't beat distance (same for pistol and shotgun) */
                if (rand_val * 2 <= scaled_dist_sq)
                    continue; /* pellet missed */

                GameObject *target = (GameObject*)(state->level.object_data +
                                     closest_idx * OBJECT_SIZE);
                /* Do not apply damage to dead enemies */
                if (target->obj.number == (int8_t)OBJ_NBR_DEAD) continue;
                if (NASTY_LIVES(*target) <= 0 && target->obj.number != (int8_t)OBJ_NBR_BARREL) continue;

                /* Hit the target */
                NASTY_SET_DAMAGE(target, (int8_t)(NASTY_DAMAGE(*target) + gun->shot_power));

                /* Impact force on target (push in firing direction) */
                int16_t push_x = (int16_t)(dir_x >> 4);
                int16_t push_z = (int16_t)(dir_z >> 4);
                /* ImpactX/ImpactZ stored at type_data[20]/[24] for nastys */
                OBJ_SET_TD_W(target, 20, OBJ_TD_W(target, 20) + push_x);
                OBJ_SET_TD_W(target, 24, OBJ_TD_W(target, 24) + push_z);
            }
        }
    } else {
        /* Projectile weapon.
         * Amiga uses a shared shot pool; we put player bullets into nasty_shot_data
         * (slots 0-19) so they are updated and rendered by the same path as enemy shots. */
        if (!state->level.nasty_shot_data) return;

        /* Find free slot in nasty_shot_data */
        uint8_t *shots = state->level.nasty_shot_data;
        GameObject *bullet = NULL;
        for (int i = 0; i < 20; i++) {
            GameObject *candidate = (GameObject*)(shots + i * OBJECT_SIZE);
            if (OBJ_ZONE(candidate) < 0) {
                bullet = candidate;
                break;
            }
        }

        if (!bullet) return; /* No free slot */

        /* Save CID before memset (it's baked into the slot by level data) */
        int16_t saved_cid = OBJ_CID(bullet);

        /* Set up bullet */
        memset(bullet, 0, OBJECT_SIZE);

        /* Spawn bullet slightly in front of the player.
         * Amiga spawns at exact player pos; we add a small forward offset so
         * the rotated z > 0 (visible) on the first frame instead of culled. */
        int16_t spawn_x = (int16_t)(plr->p_xoff + ((sin_val * 32) >> 14));
        int16_t spawn_z = (int16_t)(plr->p_zoff + ((cos_val * 32) >> 14));

        /* Restore CID and write spawn position into that object point */
        obj_sw(bullet->raw, saved_cid);
        if (saved_cid >= 0 && state->level.object_points) {
            uint8_t *pt = state->level.object_points + (int)saved_cid * 8;
            obj_sw(pt,     spawn_x);
            obj_sw(pt + 4, spawn_z);
        }

        OBJ_SET_ZONE(bullet, plr->zone);
        bullet->obj.number = OBJ_NBR_BULLET;
        bullet->obj.in_top = plr->stood_in_top;

        /* Initialise sprite from first frame of the bullet animation table.
         * object_handle_bullet will advance SHOT_ANIM each tick. */
        if (gun_idx >= 0 && gun_idx < 8 && bullet_anim_tables[gun_idx]) {
            const BulletAnimFrame *f = &bullet_anim_tables[gun_idx][0];
            if (f->width != (int8_t)-1) {
                bullet->obj.width_or_3d  = f->width;
                bullet->obj.world_height = f->height;
                obj_sw(bullet->raw + 8,  f->vect_num);
                obj_sw(bullet->raw + 10, f->frame_num);
            }
        }
        bullet->raw[14] = bullet_fly_src_cols[gun_idx]; /* src cols (from BulletSizes) */
        bullet->raw[15] = bullet_fly_src_rows[gun_idx]; /* src rows */
        SHOT_ANIM(*bullet) = 0;

        int16_t bspd = gun->bullet_speed;
        SHOT_SET_XVEL(*bullet, (int16_t)((sin_val * bspd) >> 14));
        SHOT_SET_ZVEL(*bullet, (int16_t)((cos_val * bspd) >> 14));
        SHOT_SET_YVEL(*bullet, bulyspd + gun->bullet_y_offset);
        SHOT_POWER(*bullet) = gun->shot_power;
        SHOT_STATUS(*bullet) = 0;
        SHOT_SET_LIFE(*bullet, 0);
        SHOT_SET_GRAV(*bullet, gun->shot_gravity);
        SHOT_SET_FLAGS(*bullet, gun->shot_flags);
        SHOT_SET_ACCYPOS(*bullet, plr->p_yoff + 20 * 128);
        NASTY_SET_EFLAGS(*bullet, enemy_flags);
        SHOT_SIZE(*bullet) = (int8_t)gun_idx;
        bullet->obj.worry = 127;
    }
}

void player1_shoot(GameState *state)
{
    player_shoot_internal(state, &state->plr1, 1, default_plr1_guns);
}

void player2_shoot(GameState *state)
{
    player_shoot_internal(state, &state->plr2, 2, default_plr2_guns);
}
