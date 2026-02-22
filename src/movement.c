/*
 * Alien Breed 3D I - PC Port
 * movement.c - Object movement, collision, and physics (full implementation)
 *
 * Translated from: ObjectMove.s, Fall.s
 */

#include "movement.h"
#include "game_data.h"
#include "math_tables.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* -----------------------------------------------------------------------
 * Constants from the original code
 * ----------------------------------------------------------------------- */
#define GRAVITY_ACCEL       256     /* gravity per frame (Fall.s: add.l #256,d2) */
#define GRAVITY_DECEL       512     /* ground decel (Fall.s: sub.l #512,d2) */
#define WATER_MAX_VELOCITY  512     /* max fall speed in water */

/* Floor line structure offsets (from Defs.i / raw data analysis):
 * 0:  int16  x1         - start X
 * 2:  int16  z1         - start Z
 * 4:  int16  xlen       - X length (direction * scale)
 * 6:  int16  zlen       - Z length (direction * scale)
 * 8:  int16  connected_zone  - zone on other side (-1 = solid wall)
 * 10: int16  line_length     - magnitude of direction vector
 * 12: int16  normal_or_angle - precomputed value
 * 14: int8   awayfromwall_shift
 * 15: int8   reserved
 * Total: 16 bytes per floor line
 */
#define FLINE_SIZE      16
#define FLINE_X         0
#define FLINE_Z         2
#define FLINE_XLEN      4
#define FLINE_ZLEN      6
#define FLINE_CONNECT   8
#define FLINE_LENGTH    10
#define FLINE_NORMAL    12
#define FLINE_AWAY      14

/* Zone data offsets */
#define ZONE_FLOOR_HEIGHT       2
#define ZONE_ROOF_HEIGHT        6
#define ZONE_UPPER_FLOOR        10
#define ZONE_UPPER_ROOF         14
#define ZONE_EXIT_LIST          32
#define ZONE_WALL_LIST          28

/* -----------------------------------------------------------------------
 * Helper: read big-endian int16 from level data
 * ----------------------------------------------------------------------- */
static int16_t read_be16(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}

static int32_t read_be32(const uint8_t *p)
{
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* -----------------------------------------------------------------------
 * True iff path (oldx,oldz)->(newx,newz) intersects the wall SEGMENT
 * (lx,lz) to (lx+wx, lz+wz) where all coordinates are in the same
 * (shifted) coordinate space. Returns false for the infinite line extension.
 * ----------------------------------------------------------------------- */
static int path_hits_segment(int32_t oldx, int32_t oldz, int32_t newx, int32_t newz,
                              int32_t lx, int32_t lz, int32_t wx, int32_t wz)
{
    int64_t dx = (int64_t)(newx - oldx);
    int64_t dz = (int64_t)(newz - oldz);
    int64_t old_cross = (int64_t)(oldx - lx) * wz - (int64_t)(oldz - lz) * wx;
    int64_t denom = dx * (int64_t)wz - dz * (int64_t)wx;
    if (denom == 0) return 0;
    /* t = -old_cross / denom = param along path (0 = old, 1 = new) */
    int64_t t_num = -old_cross;
    if (denom > 0) {
        if (t_num < 0 || t_num > denom) return 0;
    } else {
        if (t_num > 0 || t_num < denom) return 0;
    }
    /* Param s on wall: s=0 at (lx,lz), s=1 at (lx+wx,lz+wz).
     * s = ((hit - start) . wall_dir) / |wall_dir|^2 */
    int64_t wall_len_sq = (int64_t)wx * wx + (int64_t)wz * wz;
    if (wall_len_sq == 0) return 0;
    int64_t s_num = (int64_t)(oldx - lx) * wx + (int64_t)(oldz - lz) * wz;
    s_num += (t_num * (dx * (int64_t)wx + dz * (int64_t)wz)) / denom;
    if (s_num < 0 || s_num > wall_len_sq) return 0;
    return 1;
}

/* -----------------------------------------------------------------------
 * Helper: Newton-Raphson square root (from ObjectMove.s CalcDist)
 *
 * The original uses 3 iterations of Newton-Raphson:
 *   guess = (dx+dz)/2  (initial)
 *   guess = (guess + (dx*dx + dz*dz)/guess) / 2  (iterate 3x)
 * ----------------------------------------------------------------------- */
static int32_t newton_sqrt(int32_t dx, int32_t dz)
{
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;

    if (dx == 0 && dz == 0) return 0;

    /* Initial guess */
    int32_t guess = (dx + dz) / 2;
    if (guess == 0) guess = 1;

    /* Sum of squares */
    int64_t sum_sq = (int64_t)dx * dx + (int64_t)dz * dz;

    /* 3 iterations */
    for (int i = 0; i < 3; i++) {
        if (guess == 0) break;
        guess = (int32_t)((guess + sum_sq / guess) / 2);
    }

    return guess;
}

/* -----------------------------------------------------------------------
 * move_context_init
 * ----------------------------------------------------------------------- */
void move_context_init(MoveContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->extlen = 40;
    ctx->step_up_val = 40 * 256;
    ctx->step_down_val = 0x1000000;
    ctx->coll_id = -1;
}

/* -----------------------------------------------------------------------
 * player_fall - Gravity physics
 *
 * Translated directly from Fall.s Plr1Fall.
 * ----------------------------------------------------------------------- */
void player_fall(int32_t *yoff, int32_t *yvel, int32_t tyoff,
                 int32_t water_level, bool in_water)
{
    int32_t d0 = tyoff - *yoff;
    int32_t d1 = *yoff;
    int32_t d2 = *yvel;

    if (d0 > 0) {
        /* Above ground - falling */
        d1 += d2;
        d2 += GRAVITY_ACCEL;

        if (in_water && d1 >= water_level) {
            if (d2 > WATER_MAX_VELOCITY) {
                d2 = WATER_MAX_VELOCITY;
            }
        }
    } else {
        /* At or below ground */
        d2 -= GRAVITY_DECEL;
        if (d2 < 0) {
            d2 = 0;
        }

        d1 += d2;
        d0 = tyoff - d1;

        if (d0 > 0) {
            d2 = 0;
            d1 += d0;
        }
    }

    *yvel = d2;
    *yoff = d1;
}

/* -----------------------------------------------------------------------
 * move_object - Move object through level geometry
 *
 * Translated from ObjectMove.s MoveObject (line ~21-998).
 *
 * Full algorithm:
 * 1. Get current room's wall list and exit list
 * 2. Check movement against each wall line (cross product test)
 * 3. For exit lines: check if passable, transition rooms
 * 4. For wall lines: collision response (slide along wall)
 * 5. Handle step-up/step-down at room transitions
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Wall slide + bounds check
 *
 * Translated from ObjectMove.s:
 *   calcalong  (lines ~300-317) - slide computation
 *   othercheck (lines ~357-448) - segment bounds validation
 *   hitthewall (lines ~450-457) - accept and write result
 *
 * The Amiga ONLY writes the wall-slid position if the slide point falls
 * within the wall segment (±4 unit tolerance on the dominant axis).
 * Without this bounds check, crossings at the virtual extension of wall
 * lines cause false wall hits that eat up player movement ("treacle").
 *
 * Returns true if wall hit accepted, false if slide was outside segment.
 * When block_transition is true (stairs: blocking step-back), always slide/clamp
 * so we make progress along the boundary and can reach the next step.
 * ----------------------------------------------------------------------- */
static bool do_wall_slide(MoveContext *ctx, const uint8_t *fline,
                           int16_t lxlen, int16_t lzlen,
                           int32_t xdiff, int32_t zdiff,
                           int64_t new_cross, int ps, bool block_transition)
{
    int32_t lx = (int32_t)(int16_t)read_be16(fline + FLINE_X) << ps;
    int32_t lz = (int32_t)(int16_t)read_be16(fline + FLINE_Z) << ps;

    if (ctx->wallbounce) {
        ctx->newx = ctx->oldx;
        ctx->newz = ctx->oldz;
        return true;
    }

    /* Compute slide position: project movement onto wall direction.
     * Result: old position + component of movement parallel to wall. */
    int32_t slide_x, slide_z;
    int32_t wall_len_sq = (int32_t)lxlen * lxlen + (int32_t)lzlen * lzlen;
    if (wall_len_sq > 0) {
        int64_t dot = (int64_t)xdiff * lxlen + (int64_t)zdiff * lzlen;
        slide_x = ctx->oldx + (int32_t)((dot * lxlen) / wall_len_sq);
        slide_z = ctx->oldz + (int32_t)((dot * lzlen) / wall_len_sq);
    } else {
        slide_x = ctx->oldx;
        slide_z = ctx->oldz;
    }

    /* Push away from wall (only for AI/objects, player has awayfromwall=0) */
    if (ctx->awayfromwall > 0) {
        int8_t away = *(int8_t*)(fline + FLINE_AWAY);
        int32_t push_x, push_z;
        if (ps > away) {
            push_x = (int32_t)lzlen << (ps - away);
            push_z = (int32_t)lxlen << (ps - away);
        } else {
            push_x = (int32_t)lzlen >> (away - ps);
            push_z = (int32_t)lxlen >> (away - ps);
        }
        if (new_cross > 0) {
            slide_x -= push_x;
            slide_z += push_z;
        } else {
            slide_x += push_x;
            slide_z -= push_z;
        }
    }

    /* ---- Segment bounds check (Amiga othercheck, lines ~387-448) ----
     * Verify the slide position is within the wall line segment and on the
     * same side of the line as old (else we'd put the player through the wall). */
    {
        int32_t rel_x = (int32_t)((slide_x - lx) >> ps);
        int32_t rel_z = (int32_t)((slide_z - lz) >> ps);
        int16_t ax = lxlen >= 0 ? lxlen : -lxlen;
        int16_t az = lzlen >= 0 ? lzlen : -lzlen;
        int32_t wall_len_sq2 = (int32_t)lxlen * lxlen + (int32_t)lzlen * lzlen;
        if (wall_len_sq2 <= 0) return false;

        /* Slide must be on same side of line as old position (don't accept "through" wall). */
        int64_t old_cross = (int64_t)(ctx->oldx - lx) * lzlen - (int64_t)(ctx->oldz - lz) * lxlen;
        int64_t slide_cross = (int64_t)(slide_x - lx) * lzlen - (int64_t)(slide_z - lz) * lxlen;
        int64_t t_num;
        if ((slide_cross ^ old_cross) < 0) {
            /* Slide is on wrong side - reject so caller reverts to old position */
            return false;
        }

        /* Perpendicular distance from slide to wall line - reject if far off. */
        int64_t perp = (int64_t)rel_x * (int64_t)lzlen - (int64_t)rel_z * (int64_t)lxlen;
        if (perp < 0) perp = -perp;
        if (ax + az > 0 && perp > (int64_t)(16 * (ax + az))) return false;

        /* Project slide onto segment: t = (rel . wall) / |wall|^2. Accept only if in [0,1]. */
        t_num = (int64_t)rel_x * lxlen + (int64_t)rel_z * lzlen;
        if (t_num >= 0 && t_num <= (int64_t)wall_len_sq2) {
            /* Clearly within segment - accept the slide */
            ctx->newx = slide_x;
            ctx->newz = slide_z;
            return true;
        }

        /* Outside segment: for long walls clamp to corner; for short segments (e.g. stair
         * risers) clamping to the endpoint would teleport the player to the side, so revert.
         * When block_transition we must slide so we can reach the next step - clamp to segment. */
        if (wall_len_sq2 <= (int64_t)(128 * 128) && !block_transition) {
            return false;  /* short segment - revert, caller will not move */
        }
        /* Clamp to nearest point on segment (stop at corner). */
        if (t_num < 0) t_num = 0;
        if (t_num > (int64_t)wall_len_sq2) t_num = (int64_t)wall_len_sq2;
        int64_t scale = (int64_t)(1 << ps);
        ctx->newx = lx + (int32_t)((t_num * (int64_t)lxlen * scale) / (int64_t)wall_len_sq2);
        ctx->newz = lz + (int32_t)((t_num * (int64_t)lzlen * scale) / (int64_t)wall_len_sq2);
        return true;
    }
}

/* -----------------------------------------------------------------------
 * check_wall_line - Phase 1: Check one floor line for wall collision.
 *
 * For wall lines (connect < 0): always do wall collision.
 * For exit lines (connect >= 0): check if passable → skip. Blocked → wall.
 * Zone transitions are handled separately in find_room (Phase 3).
 *
 * Returns:  0 = no crossing or passable exit (continue)
 *           2 = wall hit, slid (restart scan)
 *           3 = wall hit, reverted (stop)
 * ----------------------------------------------------------------------- */
static int check_wall_line(MoveContext *ctx, LevelState *level,
                           const uint8_t *fline, const uint8_t *zone_data,
                           int32_t *xdiff, int32_t *zdiff)
{
    int ps = ctx->pos_shift;

    int16_t connect = (int16_t)read_be16(fline + FLINE_CONNECT);
    int32_t lx = (int32_t)(int16_t)read_be16(fline + FLINE_X) << ps;
    int32_t lz = (int32_t)(int16_t)read_be16(fline + FLINE_Z) << ps;
    int16_t lxlen = (int16_t)read_be16(fline + FLINE_XLEN);
    int16_t lzlen = (int16_t)read_be16(fline + FLINE_ZLEN);

    /* Cross product test: are old and new on opposite sides of this line? */
    int64_t new_cross = (int64_t)(ctx->newx - lx) * lzlen -
                        (int64_t)(ctx->newz - lz) * lxlen;
    int64_t old_cross = (int64_t)(ctx->oldx - lx) * lzlen -
                        (int64_t)(ctx->oldz - lz) * lxlen;

    if ((new_cross ^ old_cross) >= 0) return 0;  /* did not cross */

    /* Verify crossing is within the wall segment (not infinite extension) */
    int32_t wx = (int32_t)lxlen << ps;
    int32_t wz = (int32_t)lzlen << ps;
    if (!path_hits_segment(ctx->oldx, ctx->oldz, ctx->newx, ctx->newz,
                           lx, lz, wx, wz)) {
        return 0;
    }

    /* ---- Exit line: check if passable ---- */
    bool slide_no_revert = false;

    if (zone_data && level->zone_adds && connect >= 0 && connect < level->num_zones) {
        int32_t target_zone_off = read_be32(level->zone_adds + connect * 4);
        const uint8_t *target_zone = level->data + target_zone_off;

        int32_t target_floor = read_be32(target_zone + ZONE_FLOOR_HEIGHT);
        int32_t target_roof  = read_be32(target_zone + ZONE_ROOF_HEIGHT);
        int32_t our_floor = read_be32(zone_data +
            (ctx->stood_in_top ? ZONE_UPPER_FLOOR : ZONE_FLOOR_HEIGHT));

        int32_t floor_diff = target_floor - our_floor;
        if (floor_diff < 0) floor_diff = -floor_diff;

        if (floor_diff <= ctx->step_up_val) {
            int32_t clearance = target_floor - target_roof;
            if (clearance >= ctx->thing_height || ctx->thing_height == 0) {
                uint8_t *target_room = (uint8_t*)(level->data + target_zone_off);
                if (ctx->no_transition_back && target_room == ctx->no_transition_back) {
                    slide_no_revert = true;
                } else {
                    /* Exit is passable - skip. Zone transition in find_room. */
                    return 0;
                }
            }
        }
        /* Exit is blocked (height/clearance failed) - treat as wall */
    }

    /* ---- Wall collision: slide or revert ---- */
    if (do_wall_slide(ctx, fline, lxlen, lzlen, *xdiff, *zdiff,
                      new_cross, ps, slide_no_revert)) {
        ctx->hitwall = 1;
        *xdiff = ctx->newx - ctx->oldx;
        *zdiff = ctx->newz - ctx->oldz;
        return 2;  /* wall hit, slid */
    } else {
        ctx->newx = ctx->oldx;
        ctx->newz = ctx->oldz;
        ctx->hitwall = 1;
        return 3;  /* wall hit, reverted */
    }
}

/* -----------------------------------------------------------------------
 * find_room - Phase 3: Determine which room the player's final position
 *             is in after wall collision resolution.
 *
 * Translated from ObjectMove.s "FIND ROOM WE'RE STANDING IN" (line ~787-998).
 *
 * Walks the current zone's exit list (entries before -1 only). For each
 * exit line, checks if the player's new position has crossed to the other
 * side. If so, transitions to the connected zone and restarts.
 * ----------------------------------------------------------------------- */
static void find_room(MoveContext *ctx, LevelState *level,
                      const uint8_t **zone_data_ptr)
{
    const uint8_t *zone_data = *zone_data_ptr;
    if (!zone_data || !level->zone_adds) return;

    int ps = ctx->pos_shift;
    int restart_count = 0;

find_room_restart:
    if (restart_count++ > 16) return;  /* safety limit */

    {
        int16_t list_off = read_be16(zone_data + ZONE_EXIT_LIST);
        const uint8_t *list_ptr = zone_data + list_off;

        for (int i = 0; i < 64; i++) {
            int16_t entry = read_be16(list_ptr + i * 2);
            if (entry < 0) break;  /* -1 ends the exit portion */

            const uint8_t *fline = level->floor_lines + entry * FLINE_SIZE;
            int16_t connect = (int16_t)read_be16(fline + FLINE_CONNECT);
            if (connect < 0) continue;  /* wall line, skip */
            if (connect >= level->num_zones) continue;

            int32_t lx = (int32_t)(int16_t)read_be16(fline + FLINE_X) << ps;
            int32_t lz = (int32_t)(int16_t)read_be16(fline + FLINE_Z) << ps;
            int16_t lxlen = (int16_t)read_be16(fline + FLINE_XLEN);
            int16_t lzlen = (int16_t)read_be16(fline + FLINE_ZLEN);

            /* Check if newpos is on the "other side" of this exit line
             * (Amiga: cross product < 0 means on the left/exit side) */
            int64_t new_cross = (int64_t)(ctx->newx - lx) * lzlen -
                                (int64_t)(ctx->newz - lz) * lxlen;
            if (new_cross >= 0) continue;  /* still on this side */

            /* Verify path actually crossed the segment (not just infinite line).
             * Uses cross products with segment endpoints to check bounds.
             * (Amiga lines ~912-942: checkifcrossed) */
            int32_t wx = (int32_t)lxlen << ps;
            int32_t wz = (int32_t)lzlen << ps;
            if (!path_hits_segment(ctx->oldx, ctx->oldz, ctx->newx, ctx->newz,
                                   lx, lz, wx, wz)) {
                continue;
            }

            /* Check height compatibility */
            int32_t target_zone_off = read_be32(level->zone_adds + connect * 4);
            const uint8_t *target_zone = level->data + target_zone_off;
            int32_t target_floor = read_be32(target_zone + ZONE_FLOOR_HEIGHT);
            int32_t target_roof  = read_be32(target_zone + ZONE_ROOF_HEIGHT);

            int32_t our_floor = read_be32(zone_data +
                (ctx->stood_in_top ? ZONE_UPPER_FLOOR : ZONE_FLOOR_HEIGHT));
            int32_t floor_diff = target_floor - our_floor;
            if (floor_diff < 0) floor_diff = -floor_diff;
            if (floor_diff > ctx->step_up_val) continue;

            int32_t clearance = target_floor - target_roof;
            if (clearance < ctx->thing_height && ctx->thing_height != 0) continue;

            /* Block transition back if one-frame lockout is active */
            uint8_t *target_room = (uint8_t*)(level->data + target_zone_off);
            if (ctx->no_transition_back && target_room == ctx->no_transition_back) {
                continue;
            }

            /* ---- Zone transition ---- */
            ctx->objroom = target_room;
            ctx->stood_in_top = (ctx->newy < target_roof) ? 1 : 0;
            zone_data = target_zone;
            *zone_data_ptr = target_zone;
            ctx->oldx = ctx->newx;
            ctx->oldz = ctx->newz;
            goto find_room_restart;  /* restart with new zone's exit list */
        }
    }
}

/* -----------------------------------------------------------------------
 * move_object - Zone-based collision against current room's walls and exits
 *
 * Translated from ObjectMove.s MoveObject (line ~21-998).
 *
 * The Amiga only checks floor lines listed in the current zone's combined
 * exit+wall list (not all lines in the level). This avoids phantom
 * collisions from lines belonging to other rooms.
 *
 * Data format at zone_data + ToExitList (offset 32):
 *   [exit_line_indices..., -1, wall_line_indices..., -2]
 *
 * Note: ToWallList (offset 28) is defined but never used by the Amiga
 * MoveObject - the wall indices are embedded after the -1 separator in
 * the exit list.
 *
 * Algorithm (matching Amiga "checkwalls" first pass):
 * 1. Walk the combined list entry by entry
 *    - >= 0: floor line index → check crossing, handle exit/wall
 *    - -1: separator (skip, continue to wall portion)
 *    - -2: end of list
 * 2. On zone transition, restart with new zone's list
 * 3. On wall hit, slide and restart to catch secondary collisions
 * ----------------------------------------------------------------------- */
void move_object(MoveContext *ctx, LevelState *level)
{
    if (!level->data || !level->floor_lines) {
        ctx->hitwall = 0;
        return;
    }

    ctx->hitwall = 0;

    int32_t xdiff = ctx->newx - ctx->oldx;
    int32_t zdiff = ctx->newz - ctx->oldz;
    if (xdiff == 0 && zdiff == 0) return;

    const uint8_t *zone_data = ctx->objroom;

    int total_iterations = 0;
    const int max_total = 200;

    if (zone_data) {
        /* ==== Zone-based collision (Amiga gobackanddoitallagain loop) ====
         *
         * Phase 1: Walk current zone's combined exit+wall list.
         *          Passable exits are SKIPPED (player can pass through).
         *          Walls and blocked exits cause wall-slide or revert.
         *
         * Phase 3: After wall collision, determine which room the player's
         *          final position is in. Zone transitions happen here.
         *          On transition, restart ALL phases with the new zone
         *          (Amiga: bra gobackanddoitallagain).
         */

    gobackanddoitallagain:
        if (total_iterations >= max_total) return;

        /* ---- Phase 1: Wall collision ---- */
    restart_walls:
        if (total_iterations >= max_total) goto phase3;

        {
            int16_t list_off = read_be16(zone_data + ZONE_EXIT_LIST);
            const uint8_t *list_ptr = zone_data + list_off;

            for (int i = 0; i < 128; i++, total_iterations++) {
                if (total_iterations >= max_total) goto phase3;

                int16_t entry = read_be16(list_ptr + i * 2);
                if (entry == -2) break;   /* end of combined list */
                if (entry < 0) continue;  /* -1 separator: skip to wall portion */

                const uint8_t *fline = level->floor_lines + entry * FLINE_SIZE;
                int result = check_wall_line(ctx, level, fline, zone_data,
                                             &xdiff, &zdiff);

                if (result == 2) goto restart_walls;  /* wall slide - rescan */
                if (result == 3) goto phase3;          /* wall revert - done */
            }
        }

    phase3:
        /* ---- Phase 3: Find room (zone transitions) ---- */
        {
            const uint8_t *prev_zone = zone_data;
            find_room(ctx, level, &zone_data);

            if (zone_data != prev_zone) {
                /* Zone changed - restart with new zone's walls
                 * (Amiga: bra gobackanddoitallagain) */
                goto gobackanddoitallagain;
            }
        }

    } else {
        /* ==== Brute-force fallback (objects/AI without zone data) ====
         * Check all floor lines. Less accurate but works without zone info. */
        int32_t num_lines = (int32_t)level->num_floor_lines;
        if (num_lines <= 0) return;

    restart_brute:
        if (total_iterations >= max_total) return;

        for (int32_t i = 0; i < num_lines; i++, total_iterations++) {
            if (total_iterations >= max_total) return;

            const uint8_t *fline = level->floor_lines + i * FLINE_SIZE;
            int result = check_wall_line(ctx, level, fline, zone_data,
                                         &xdiff, &zdiff);

            if (result == 2) goto restart_brute;
            if (result == 3) return;
        }
    }
}

/* -----------------------------------------------------------------------
 * collision_check - Object-to-object collision
 *
 * Translated from ObjectMove.s Collision (line ~1839-1943).
 * ----------------------------------------------------------------------- */
void collision_check(MoveContext *ctx, LevelState *level)
{
    ctx->hitwall = 0;

    if (!level->object_data) return;

    int obj_index = 0;
    while (1) {
        GameObject *obj = (GameObject *)(level->object_data + obj_index * OBJECT_SIZE);

        /* End of list */
        if (OBJ_CID(obj) < 0) break;

        /* Skip self */
        if (OBJ_CID(obj) == ctx->coll_id && ctx->coll_id >= 0) {
            obj_index++;
            continue;
        }

        /* Skip dead/inactive objects */
        if (OBJ_ZONE(obj) < 0) {
            obj_index++;
            continue;
        }

        /* Check collide flags bitmask */
        int obj_type = obj->obj.number;
        if (obj_type < 0 || obj_type > 20) {
            obj_index++;
            continue;
        }

        uint32_t type_bit = 1u << obj_type;
        if (!(ctx->collide_flags & type_bit)) {
            obj_index++;
            continue;
        }

        /* Check same floor layer */
        if (ctx->stood_in_top != obj->obj.in_top) {
            obj_index++;
            continue;
        }

        /* Get object position */
        int16_t ox = 0, oz = 0;
        if (level->object_points) {
            const uint8_t *p = level->object_points + obj_index * 8;
            ox = obj_w(p);
            oz = obj_w(p + 4);
        }

        /* Get collision box */
        const CollisionBox *box = &col_box_table[obj_type];

        /* Manhattan distance check first (fast reject) */
        int32_t dx = ctx->newx - ox;
        int32_t dz = ctx->newz - oz;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;

        /* Y-axis height check (ObjectMove.s lines ~1887-1893):
         * Uses the mover's full vertical extent [newy, newy+thingheight]
         * and the object's vertical extent [obj_y - half_h, obj_y + half_h].
         * Collision only if these ranges overlap. */
        {
            int16_t obj_y = (int16_t)((obj->raw[4] << 8) | obj->raw[5]);
            int16_t mover_bot = (int16_t)(ctx->newy >> 7);
            int16_t mover_top = (int16_t)((ctx->newy + ctx->thing_height) >> 7);

            int16_t obj_bot = obj_y - box->half_height;
            int16_t obj_top = obj_y + box->half_height;

            if (mover_top < obj_bot || mover_bot > obj_top) {
                obj_index++;
                continue;
            }
        }

        /* Manhattan / box distance check (ObjectMove.s lines ~1895-1918).
         * The Amiga checks max(|dx|,|dz|) against target box width. */
        if (dx < box->width && dz < box->width) {

            /* "Moving closer" check (ObjectMove.s lines ~1920-1935):
             * Only trigger collision if the mover is getting closer to
             * (or maintaining distance from) the object.  This is critical:
             * without it the player gets permanently trapped inside any
             * collision box because the position reverts every frame. */
            int32_t new_dx = (int32_t)ox - ctx->newx;
            int32_t new_dz = (int32_t)oz - ctx->newz;
            int32_t new_dist_sq = new_dx * new_dx + new_dz * new_dz;

            int32_t old_dx = (int32_t)ox - ctx->oldx;
            int32_t old_dz = (int32_t)oz - ctx->oldz;
            int32_t old_dist_sq = old_dx * old_dx + old_dz * old_dz;

            if (new_dist_sq > old_dist_sq) {
                /* Moving away from this object - allow the movement */
                obj_index++;
                continue;
            }

            ctx->hitwall = 1;
            return;
        }

        obj_index++;
    }
}

/* -----------------------------------------------------------------------
 * head_towards - Move toward a target point
 *
 * Translated from ObjectMove.s HeadTowards (line ~1044-1135).
 * Uses Newton-Raphson sqrt for distance calculation.
 * ----------------------------------------------------------------------- */
void head_towards(MoveContext *ctx, int32_t target_x, int32_t target_z,
                  int16_t speed)
{
    int32_t dx = target_x - ctx->oldx;
    int32_t dz = target_z - ctx->oldz;
    int32_t dist = newton_sqrt(dx, dz);

    if (dist == 0) {
        ctx->newx = ctx->oldx;
        ctx->newz = ctx->oldz;
        return;
    }

    /* Check if within range */
    if (dist <= speed) {
        ctx->newx = target_x;
        ctx->newz = target_z;
        return;
    }

    /* Scale movement vector to speed */
    ctx->newx = ctx->oldx + (dx * speed) / dist;
    ctx->newz = ctx->oldz + (dz * speed) / dist;
}

/* -----------------------------------------------------------------------
 * head_towards_angle - Steer facing toward target and move forward
 *
 * Translated from ObjectMove.s HeadTowardsAng (line ~1199-1366).
 *
 * 1. Calculate distance to target using Newton-Raphson
 * 2. Calculate desired angle using binary search on SineTable
 * 3. Steer facing toward desired angle (limited by turn_speed)
 * 4. Move forward in facing direction at speed
 * 5. Apply player push forces (shove system)
 * ----------------------------------------------------------------------- */
void head_towards_angle(MoveContext *ctx, int16_t *facing,
                        int32_t target_x, int32_t target_z,
                        int16_t speed, int16_t turn_speed)
{
    int32_t dx = target_x - ctx->oldx;
    int32_t dz = target_z - ctx->oldz;

    if (dx == 0 && dz == 0) return;

    /* Calculate target angle using atan2 approximation
     * Original uses binary search on SineTable, but atan2 is equivalent */
    double angle_rad = atan2((double)-dx, (double)-dz);
    int16_t target_angle = (int16_t)(angle_rad * (4096.0 / (2.0 * 3.14159265)));
    target_angle = (target_angle * 2) & ANGLE_MASK; /* Convert to byte index */

    /* Calculate angle difference */
    int16_t current = *facing;
    int16_t diff = target_angle - current;

    /* Normalize to [-4096, 4096] */
    while (diff > 4096) diff -= 8192;
    while (diff < -4096) diff += 8192;

    /* Apply turn speed limit */
    if (diff > turn_speed) diff = turn_speed;
    if (diff < -turn_speed) diff = -turn_speed;

    current += diff;
    current &= ANGLE_MASK;
    *facing = current;

    /* Move forward at speed in the facing direction */
    int16_t sin_val = sin_lookup(current);
    int16_t cos_val = cos_lookup(current);

    ctx->newx = ctx->oldx - ((int32_t)sin_val * speed) / 16384;
    ctx->newz = ctx->oldz - ((int32_t)cos_val * speed) / 16384;
}

/* -----------------------------------------------------------------------
 * check_teleport - Zone teleporter check
 *
 * Translated from ObjectMove.s CheckTeleport (line ~2002-2043).
 *
 * Algorithm:
 * 1. Check if zone has ToTelZone >= 0
 * 2. If yes, get destination coordinates (ToTelX, ToTelZ)
 * 3. Adjust Y for floor height difference between zones
 * 4. Check collision at destination
 * 5. If no collision, teleport; update objroom
 * ----------------------------------------------------------------------- */
bool check_teleport(MoveContext *ctx, LevelState *level, int16_t zone_id)
{
    if (!level->data || !level->zone_adds) return false;

    /* Get zone data */
    int32_t zone_off = read_be32(level->zone_adds + zone_id * 4);
    const uint8_t *zone_data = level->data + zone_off;

    /* Check ToTelZone (offset 38 in zone data) */
    int16_t tel_zone = read_be16(zone_data + 38); /* ZONE_OFF_TEL_ZONE */
    if (tel_zone < 0) return false;

    /* Get destination coordinates */
    int16_t tel_x = read_be16(zone_data + 40); /* ZONE_OFF_TEL_X */
    int16_t tel_z = read_be16(zone_data + 42); /* ZONE_OFF_TEL_Z */

    /* Get floor height difference */
    int32_t src_floor = read_be32(zone_data + ZONE_FLOOR_HEIGHT);

    int32_t dest_zone_off = read_be32(level->zone_adds + tel_zone * 4);
    const uint8_t *dest_zone = level->data + dest_zone_off;
    int32_t dest_floor = read_be32(dest_zone + ZONE_FLOOR_HEIGHT);

    int32_t floor_diff = dest_floor - src_floor;

    /* Set new position */
    ctx->newx = tel_x;
    ctx->newz = tel_z;
    ctx->newy += floor_diff;

    /* Check collision at destination */
    uint32_t old_flags = ctx->collide_flags;
    ctx->collide_flags = 0x7FFFF; /* All types */
    collision_check(ctx, level);
    ctx->collide_flags = old_flags;

    if (ctx->hitwall) {
        /* Can't teleport - destination blocked */
        ctx->newy -= floor_diff;
        ctx->hitwall = 0;
        return false;
    }

    /* Teleport successful - update room */
    ctx->objroom = (uint8_t*)(level->data + dest_zone_off);
    return true;
}

/* -----------------------------------------------------------------------
 * find_close_room - Locate the room nearest to a target point
 *
 * Translated from ObjectMove.s FindCloseRoom (line ~2045-2080).
 * Used when an enemy needs to know which room to spawn/die in.
 * ----------------------------------------------------------------------- */
void find_close_room(MoveContext *ctx, LevelState *level, int16_t distance)
{
    /* This function uses HeadTowards to move toward the target,
     * then MoveObject to find which room that point is in.
     * The 'distance' parameter limits how far to search.
     */
    if (!level->data || !level->zone_adds) return;

    /* Just use MoveObject to resolve the room - simplified version */
    ctx->step_up_val = 0x1000000;
    ctx->step_down_val = 0x1000000;
    ctx->thing_height = 0;
    ctx->extlen = 0;
    ctx->exitfirst = 1;

    move_object(ctx, level);

    (void)distance;
}
