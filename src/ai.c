/*
 * Alien Breed 3D I - PC Port
 * ai.c - AI decision system (full implementation)
 *
 * Translated from: AI.s, ObjectMove.s (GoInDirection)
 */

#include <stdio.h>
#include "ai.h"
#include "level.h"
#include "objects.h"
#include "game_data.h"
#include "movement.h"
#include "visibility.h"
#include "math_tables.h"
#include "stub_audio.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* -----------------------------------------------------------------------
 * Visible object entry for the AI's perception
 * ----------------------------------------------------------------------- */
typedef struct {
    int8_t   visible;       /* can this AI see the object? */
    int8_t   _pad;
    int32_t  dist_sq;       /* squared distance */
} VisibleEntry;

#define MAX_VISIBLE 100

/* -----------------------------------------------------------------------
 * GoInDirection - Move in a specific angle at speed
 *
 * Translated from ObjectMove.s GoInDirection (line ~1816-1831).
 * newx = oldx + sin(angle) * speed * 2 / 65536
 * newz = oldz + cos(angle) * speed * 2 / 65536
 * ----------------------------------------------------------------------- */
void go_in_direction(int32_t *newx, int32_t *newz,
                     int32_t oldx, int32_t oldz,
                     int16_t angle, int16_t speed)
{
    int16_t s = sin_lookup(angle);
    int16_t c = cos_lookup(angle);

    int32_t dx = (int32_t)s * speed;
    dx += dx; /* *2 */
    int32_t dz = (int32_t)c * speed;
    dz += dz; /* *2 */

    *newx = oldx + (int16_t)(dx >> 16);
    *newz = oldz + (int16_t)(dz >> 16);
}

/* -----------------------------------------------------------------------
 * ExplodeIntoBits - death explosion
 *
 * Translated from AB3DI.s ExplodeIntoBits.
 * Creates visual debris objects. The rendering of debris is platform-
 * specific but the logic to spawn them is game logic.
 * ----------------------------------------------------------------------- */
void explode_into_bits(GameObject *obj, GameState *state)
{
    /* Translated from Anims.s ExplodeIntoBits (line ~75-196).
     * Creates 7-9 debris fragments (gibs) in NastyShotData with random velocities.
     * No explosion sound here: death sound (e.g. splatpop) is played by enemy_check_damage. */
    if (!state || !state->level.nasty_shot_data) return;

    int num_bits = 7 + (rand() & 3); /* 7-9 pieces */

    for (int i = 0; i < num_bits; i++) {
        /* Find free slot in NastyShotData */
        uint8_t *shots = state->level.nasty_shot_data;
        GameObject *bit = NULL;
        int slot_j = -1;
        for (int j = 0; j < 20; j++) {
            GameObject *candidate = (GameObject*)(shots + j * OBJECT_SIZE);
            if (OBJ_ZONE(candidate) < 0) {
                bit = candidate;
                slot_j = j;
                break;
            }
        }
        if (!bit || slot_j < 0) break;

        /* Preserve the slot's pre-assigned CID (from level data) before clearing,
         * just like player bullets do - it is the index into object_points for this slot. */
        int16_t saved_cid = OBJ_CID(bit);
        memset(bit, 0, OBJECT_SIZE);
        OBJ_SET_CID(bit, saved_cid);
        bit->obj.number = OBJ_NBR_BULLET;
        OBJ_SET_ZONE(bit, OBJ_ZONE(obj));
        bit->obj.in_top = obj->obj.in_top;

        /* Y position from source object's render Y (Amiga: (obj[4]+6) << 7) */
        int32_t y_pos = (int32_t)obj_w(obj->raw + 4);
        SHOT_SET_ACCYPOS(*bit, (y_pos + 6) << 7);

        /* SHOT_SIZE 50-53: maps to Explode1-4Anim (vect=0, gib frames 16-31) */
        int8_t gib_type = (int8_t)(50 + (rand() & 3));
        SHOT_SIZE(*bit) = gib_type;
        SHOT_STATUS(*bit) = 0;
        SHOT_ANIM(*bit) = 0;
        SHOT_POWER(*bit) = 0;
        SHOT_SET_LIFE(*bit, -1);   /* infinite lifetime */
        SHOT_SET_GRAV(*bit, 40);   /* Amiga: shotgrav = 40 */
        SHOT_SET_FLAGS(*bit, 0);   /* Amiga: shotflags = 0 (no bounce) */

        /* Initialise sprite from first frame of gib anim table.
         * Gib frames in alien sheet are laid out as a 4×4 grid of 16×16 cells.
         * eff_cols = src_cols*2 and eff_rows = src_rows*2 in the renderer, so
         * src_cols=8 → eff_cols=16 and src_rows=8 → eff_rows=16, exactly one cell. */
        {
            const BulletAnimFrame *f = &bullet_anim_tables[gib_type][0];
            bit->obj.width_or_3d  = f->width;
            bit->obj.world_height = f->height;
            obj_sw(bit->raw + 8,  f->vect_num);
            obj_sw(bit->raw + 10, f->frame_num);
            bit->raw[14] = 8;
            bit->raw[15] = 8;
        }

        /* Random XZ velocity: Amiga ≈ ±0-3 int units from random angle + impact.
         * Use simple uniform random ±3 to approximate. */
        SHOT_SET_XVEL(*bit, (int16_t)((rand() % 7) - 3));
        SHOT_SET_ZVEL(*bit, (int16_t)((rand() % 7) - 3));
        /* Y velocity: upward, Amiga range -(256 + rand & 1023) = -256 to -1279 */
        SHOT_SET_YVEL(*bit, (int16_t)(-(256 + (rand() & 1023))));

        /* Copy XZ position from source object */
        if (state->level.object_points) {
            int src_idx = (int)OBJ_CID(obj);
            int dst_idx = (int)OBJ_CID(bit);
            if (src_idx >= 0 && dst_idx >= 0 &&
                src_idx < state->level.num_object_points && dst_idx < state->level.num_object_points) {
                uint8_t *sp = state->level.object_points + src_idx * 8;
                uint8_t *dp = state->level.object_points + dst_idx * 8;
                memcpy(dp, sp, 2); memcpy(dp + 4, sp + 4, 2);
            }
        }

        bit->obj.worry = 127;

        printf("[GIB] slot=%d saved_cid=%d zone=%d gib_type=%d num_pts=%d yvel=%d accypos=%d\n",
               slot_j, (int)saved_cid, (int)OBJ_ZONE(bit), (int)gib_type,
               state->level.num_object_points,
               (int)SHOT_YVEL(*bit), (int)SHOT_ACCYPOS(*bit));
    }
}

/* -----------------------------------------------------------------------
 * ViewpointToDraw - calculate sprite frame from viewer angle
 *
 * Translated from ViewpointToDraw in the enemy .s files.
 * Given the angle from object to viewer and the object's facing,
 * returns which of 8 rotational frames to use (0-7).
 * ----------------------------------------------------------------------- */
int16_t viewpoint_to_draw(int16_t viewer_x, int16_t viewer_z,
                          int16_t obj_x, int16_t obj_z,
                          int16_t obj_facing)
{
    /* Calculate angle from object to viewer */
    double dx = (double)(viewer_x - obj_x);
    double dz = (double)(viewer_z - obj_z);
    double angle = atan2(-dx, -dz);
    int16_t view_angle = (int16_t)(angle * (4096.0 / (2.0 * 3.14159265)));
    view_angle = (view_angle * 2) & ANGLE_MASK;

    /* Relative angle = view_angle - facing */
    int16_t rel = (view_angle - obj_facing) & ANGLE_MASK;

    /* Convert to 0-7 frame index (each frame = 45 degrees = 1024 byte-units) */
    return (rel + 512) / 1024; /* +512 for rounding */
}

/* Same as ViewpointToDraw but for 16 rotational frames (0-15).
 * Used by enemies whose sprite tables have 16 directions (e.g. alien, marine). */
int16_t viewpoint_to_draw_16(int16_t viewer_x, int16_t viewer_z,
                             int16_t obj_x, int16_t obj_z,
                             int16_t obj_facing)
{
    double dx = (double)(viewer_x - obj_x);
    double dz = (double)(viewer_z - obj_z);
    double angle = atan2(-dx, -dz);
    int16_t view_angle = (int16_t)(angle * (4096.0 / (2.0 * 3.14159265)));
    view_angle = (view_angle * 2) & ANGLE_MASK;
    int16_t rel = (view_angle - obj_facing) & ANGLE_MASK;
    /* 0-15: each frame = 256 units */
    int16_t frame = (int16_t)((rel * 16) >> 12);
    if (frame >= 16) frame = 15;
    return frame;
}

/* -----------------------------------------------------------------------
 * Helper: get object position from ObjectPoints
 * ----------------------------------------------------------------------- */
static void ai_get_obj_pos(const LevelState *level, int obj_index,
                           int16_t *x, int16_t *z)
{
    if (level->object_points) {
        const uint8_t *p = level->object_points + obj_index * 8;
        *x = obj_w(p);
        *z = obj_w(p + 4);
    } else {
        *x = 0;
        *z = 0;
    }
}

/* -----------------------------------------------------------------------
 * ai_control - Full AI decision tree
 *
 * Translated from AI.s AIControl (line ~73-449).
 *
 * Algorithm:
 * 1. BuildVisibleList: For each object in ObjectData, check if this AI
 *    can see it (via CanItBeSeen). Classify as friend or enemy.
 *    Track closest friend and closest enemy.
 * 2. If enemies visible -> Combatant path
 *    Based on Aggression:
 *    - High aggression: attack closest enemy (CA)
 *    - Medium + outnumbered: retreat (CNA)
 *    - Medium + not outnumbered: attack (CA)
 *    - Low aggression: retreat (CNA)
 *    Cooperation modifies:
 *    - High cooperation + friends visible: follow leader (CAC/CNAC)
 * 3. If no enemies visible -> NonCombatant path
 *    - If friends visible + cooperative: follow leader
 *    - Otherwise: idle/wander (handled by caller)
 * ----------------------------------------------------------------------- */
void ai_control(GameObject *obj, GameState *state, const AIParams *params)
{
    LevelState *level = &state->level;
    if (!level->object_data || !level->object_points || !level->zone_adds) return;

    /* Get this object's position and room */
    int self_idx = (int)(((uint8_t*)obj - level->object_data) / OBJECT_SIZE);
    int16_t self_x, self_z;
    ai_get_obj_pos(level, self_idx, &self_x, &self_z);

    int16_t self_zone = OBJ_ZONE(obj);
    if (self_zone < 0) return;

    const uint8_t *from_room = level_get_zone_data_ptr(level, self_zone);
    if (!from_room) return;

    /* ---- BuildVisibleList ---- */
    int num_friends = 1; /* count self */
    int num_enemies = 0;
    int32_t dist_to_friend = 0x7FFFFFFF;
    int32_t dist_to_enemy = 0x7FFFFFFF;
    int closest_friend_idx = -1;
    int closest_enemy_idx = -1;

    int obj_idx = 0;
    while (1) {
        GameObject *other = (GameObject*)(level->object_data + obj_idx * OBJECT_SIZE);
        if (OBJ_CID(other) < 0) break;

        /* Skip self */
        if (other == obj) {
            obj_idx++;
            continue;
        }

        /* Skip dead */
        if (other->obj.number < 0 || OBJ_ZONE(other) < 0) {
            obj_idx++;
            continue;
        }

        /* Check visibility via CanItBeSeen */
        int16_t other_zone = OBJ_ZONE(other);
        if (other_zone < 0) {
            obj_idx++;
            continue;
        }

        int16_t other_x, other_z;
        ai_get_obj_pos(level, obj_idx, &other_x, &other_z);

        const uint8_t *to_room = level_get_zone_data_ptr(level, other_zone);
        if (!to_room) {
            obj_idx++;
            continue;
        }

        /* Amiga: Viewery = 4(a0), Targety = 4(a2) — Y in >>7 scale for crossing height */
        int16_t viewer_y = (int16_t)obj_w(obj->raw + 4);
        int16_t target_y = (int16_t)obj_w(other->raw + 4);
        uint8_t vis = can_it_be_seen(level, from_room, to_room, other_zone,
                                     self_x, self_z, viewer_y,
                                     other_x, other_z, target_y,
                                     obj->obj.in_top, other->obj.in_top, 0);

        if (!vis) {
            obj_idx++;
            continue;
        }

        /* Classify as friend or enemy */
        int obj_type = other->obj.number;
        uint32_t type_bit = 1u << obj_type;

        int32_t dx = other_x - self_x;
        int32_t dz = other_z - self_z;
        int32_t dist_sq = dx * dx + dz * dz;

        if (params->friends & type_bit) {
            num_friends++;
            if (dist_sq < dist_to_friend) {
                dist_to_friend = dist_sq;
                closest_friend_idx = obj_idx;
            }
        }

        if (params->enemies & type_bit) {
            num_enemies++;
            if (dist_sq < dist_to_enemy) {
                dist_to_enemy = dist_sq;
                closest_enemy_idx = obj_idx;
            }
        }

        obj_idx++;
    }

    /* ---- Decision tree ---- */
    if (num_enemies > 0) {
        /* Combatant path */
        int16_t agg = params->aggression;
        int16_t coop = params->cooperation;
        int outnumbered = num_enemies - num_friends;

        bool should_attack = false;
        bool should_follow = false;
        bool should_retreat = false;

        if (agg > 20) {
            /* Very aggressive */
            if (num_friends > 1 && coop >= 20) {
                should_follow = true; /* CAC */
            } else {
                should_attack = true; /* CA */
            }
        } else if (agg <= 10) {
            /* Very unaggressive */
            if (num_friends > 0 && coop > 10) {
                should_follow = true; /* CNAC */
            } else {
                should_retreat = true; /* CNA */
            }
        } else {
            /* Medium aggression */
            if (outnumbered > 0) {
                should_retreat = true; /* CNA */
            } else {
                should_attack = true; /* CA */
            }
        }

        if (should_attack && closest_enemy_idx >= 0) {
            /* CA: Head toward closest enemy and attack */
            int16_t ex, ez;
            ai_get_obj_pos(level, closest_enemy_idx, &ex, &ez);

            int16_t speed = NASTY_MAXSPD(*obj);
            if (speed == 0) speed = 6;
            speed = (int16_t)(speed * state->temp_frames);
            if (params->armed) speed >>= 1; /* Armed units move slower */

            MoveContext ctx;
            move_context_init(&ctx);
            ctx.oldx = self_x;
            ctx.oldz = self_z;
            ctx.objroom = (uint8_t*)from_room;
            ctx.thing_height = 128 * 128;
            ctx.step_up_val = 20 * 256;

            int16_t facing = NASTY_FACING(*obj);
            head_towards_angle(&ctx, &facing, ex, ez, speed, 120);
            NASTY_SET_FACING(*obj, facing);

            move_object(&ctx, level);

            /* Update position */
            if (level->object_points) {
                uint8_t *pts = level->object_points + self_idx * 8;
                obj_sw(pts, (int16_t)ctx.newx);
                obj_sw(pts + 4, (int16_t)ctx.newz);
            }
            /* Update zone from room */
            if (ctx.objroom) {
                int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
                OBJ_SET_ZONE(obj, new_zone);
            }

            /* Fire if armed */
            if (params->armed) {
                /* Determine which player is the enemy */
                GameObject *enemy_obj = (GameObject*)(level->object_data +
                                        closest_enemy_idx * OBJECT_SIZE);
                int player_num = (enemy_obj->obj.number == OBJ_NBR_PLR1) ? 1 :
                                 (enemy_obj->obj.number == OBJ_NBR_PLR2) ? 2 : 0;
                if (player_num > 0) {
                    /* Fire projectile at player (AlienControl.s FireAtPlayer) */
                    int8_t *cooldown = (int8_t*)&obj->obj.type_data[8];
                    *cooldown -= (int8_t)state->temp_frames;
                    if (*cooldown <= 0) {
                        enemy_fire_at_player(obj, state, player_num,
                                             params->shot_type, params->shot_power,
                                             params->shot_speed, params->shot_shift);
                        *cooldown = (int8_t)(50 + (rand() & 0x1F));
                    }
                }
            }

        } else if (should_retreat && closest_enemy_idx >= 0) {
            /* CNA: Move away from closest enemy */
            int16_t ex, ez;
            ai_get_obj_pos(level, closest_enemy_idx, &ex, &ez);

            int16_t speed = NASTY_MAXSPD(*obj);
            if (speed == 0) speed = 6;
            speed = (int16_t)(-(speed * state->temp_frames));

            MoveContext ctx;
            move_context_init(&ctx);
            ctx.oldx = self_x;
            ctx.oldz = self_z;
            ctx.objroom = (uint8_t*)from_room;
            ctx.thing_height = 128 * 128;
            ctx.step_up_val = 20 * 256;

            int16_t facing = NASTY_FACING(*obj);
            head_towards_angle(&ctx, &facing, ex, ez, speed, 120);
            NASTY_SET_FACING(*obj, facing);

            move_object(&ctx, level);

            if (level->object_points) {
                uint8_t *pts = level->object_points + self_idx * 8;
                obj_sw(pts, (int16_t)ctx.newx);
                obj_sw(pts + 4, (int16_t)ctx.newz);
            }
            if (ctx.objroom) {
                int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
                OBJ_SET_ZONE(obj, new_zone);
            }

        } else if (should_follow && closest_friend_idx >= 0) {
            /* CAC/CNAC: Follow closest friend */
            int16_t fx, fz;
            ai_get_obj_pos(level, closest_friend_idx, &fx, &fz);

            int16_t speed = NASTY_MAXSPD(*obj);
            if (speed == 0) speed = 6;
            speed = (int16_t)(speed * state->temp_frames);

            MoveContext ctx;
            move_context_init(&ctx);
            ctx.oldx = self_x;
            ctx.oldz = self_z;
            ctx.objroom = (uint8_t*)from_room;
            ctx.thing_height = 128 * 128;
            ctx.step_up_val = 20 * 256;

            int16_t facing = NASTY_FACING(*obj);
            head_towards_angle(&ctx, &facing, fx, fz, speed, 120);
            NASTY_SET_FACING(*obj, facing);

            move_object(&ctx, level);

            if (level->object_points) {
                uint8_t *pts = level->object_points + self_idx * 8;
                obj_sw(pts, (int16_t)ctx.newx);
                obj_sw(pts + 4, (int16_t)ctx.newz);
            }
            if (ctx.objroom) {
                int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
                OBJ_SET_ZONE(obj, new_zone);
            }

            /* Fire if armed (CAC path) */
            if (params->armed && should_attack && closest_enemy_idx >= 0) {
                /* Same fire logic as CA */
            }
        }

    } else {
        /* NonCombatant path */
        if (num_friends > 0 && params->cooperation > 10 && closest_friend_idx >= 0) {
            /* FollowOthers */
            int16_t fx, fz;
            ai_get_obj_pos(level, closest_friend_idx, &fx, &fz);

            int16_t speed = NASTY_MAXSPD(*obj);
            if (speed == 0) speed = 6;
            speed = (int16_t)(speed * state->temp_frames);

            MoveContext ctx;
            move_context_init(&ctx);
            ctx.oldx = self_x;
            ctx.oldz = self_z;
            ctx.objroom = (uint8_t*)from_room;
            ctx.thing_height = 128 * 128;
            ctx.step_up_val = 20 * 256;

            int16_t facing = NASTY_FACING(*obj);
            head_towards_angle(&ctx, &facing, fx, fz, speed, 120);
            NASTY_SET_FACING(*obj, facing);

            move_object(&ctx, level);

            if (level->object_points) {
                uint8_t *pts = level->object_points + self_idx * 8;
                obj_sw(pts, (int16_t)ctx.newx);
                obj_sw(pts + 4, (int16_t)ctx.newz);
            }
            if (ctx.objroom) {
                int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
                OBJ_SET_ZONE(obj, new_zone);
            }
        }
        /* else: idle/wander handled by the caller's generic enemy_wander() */
    }
}
