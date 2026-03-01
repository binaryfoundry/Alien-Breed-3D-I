/*
 * Alien Breed 3D I - PC Port
 * objects.c - Object system (full implementation)
 *
 * Translated from: Anims.s, ObjectMove.s, AI.s, NormalAlien.s,
 *                  Robot.s, BigRedThing.s, HalfWorm.s, FlameMarine.s,
 *                  ToughMarine.s, MutantMarine.s, BigUglyAlien.s,
 *                  BigClaws.s, FlyingScalyBall.s, Tree.s
 *
 * The main entry point is objects_update(), called once per frame.
 */

#include "objects.h"
#include "ai.h"
#include "game_data.h"
#include "game_types.h"
#include "level.h"
#include "movement.h"
#include "math_tables.h"
#include "stub_audio.h"
#include "visibility.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LIFT_ENTRY_SIZE 20

/* Big-endian read/write helpers (Amiga data is big-endian) */
static inline int16_t be16(const uint8_t *p) {
    return (int16_t)((p[0] << 8) | p[1]);
}
static inline int32_t be32(const uint8_t *p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}
static inline void write_be16(uint8_t *p, int16_t w) {
    p[0] = (uint8_t)((unsigned)w >> 8);
    p[1] = (uint8_t)(unsigned)w;
}
static inline void wbe16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)((uint16_t)v >> 8); p[1] = (uint8_t)v;
}
static inline void wbe32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)((uint32_t)v >> 24); p[1] = (uint8_t)((uint32_t)v >> 16);
    p[2] = (uint8_t)((uint32_t)v >> 8);  p[3] = (uint8_t)v;
}

/* -----------------------------------------------------------------------
 * Visibility arrays (from AB3DI.s CalcPLR1InLine, CalcPLR2InLine)
 * ----------------------------------------------------------------------- */
int8_t  plr1_obs_in_line[MAX_OBJECTS];
int8_t  plr2_obs_in_line[MAX_OBJECTS];
int16_t plr1_obj_dists[MAX_OBJECTS];
int16_t plr2_obj_dists[MAX_OBJECTS];

/* Animation timer (from Anims.s) */
static int16_t anim_timer = 2;

/* Walk-cycle counter (Amiga: alan[] table / alanptr, advances each game tick).
 * Range 0-31 (wraps at 32). Each frame step spans 8 ticks → step = counter >> 3, range 0-3.
 * Amiga: alan[] has 8 copies of each value 0-3 → 32 entries, cycles at game tick rate. */
static uint8_t walk_cycle = 0;

/* -----------------------------------------------------------------------
 * enemy_viewpoint - Amiga AlienControl.s ViewpointToDraw
 * Returns 0=towards, 1=right, 2=away, 3=left based on angle between
 * enemy facing direction and enemy-to-player direction vector.
 *
 * Exact translation of Amiga assembly:
 *   d0 = dx*sin + dz*cos  (forward: > 0 means player is ahead)
 *   d4 = dx*cos - dz*sin  (lateral: > 0 means player is to enemy's right)
 * ----------------------------------------------------------------------- */
static int enemy_viewpoint(GameObject *obj, int16_t plr_x, int16_t plr_z,
                            const LevelState *level)
{
    int cid = (int)OBJ_CID(obj);
    int16_t ox = 0, oz = 0;
    if (level->object_points && cid >= 0 && cid < level->num_object_points) {
        const uint8_t *p = level->object_points + cid * 8;
        ox = be16(p);
        oz = be16(p + 4);
    }

    /* dx, dz = direction from enemy toward player (as in HeadTowards) */
    int32_t dx = (int32_t)plr_x - ox;
    int32_t dz = (int32_t)plr_z - oz;

    int16_t facing = NASTY_FACING(*obj);
    int32_t sf = sin_lookup(facing);  /* Amiga: d2 = SineTable[facing] */
    int32_t cf = cos_lookup(facing);  /* Amiga: d3 = SineTable[facing+1024] */

    /* Amiga: d0 = dx*sin + dz*cos (forward), d4 = dx*cos - dz*sin (lateral) */
    int32_t fwd  = dx * sf + dz * cf;   /* positive = player ahead of enemy */
    int32_t lat  = dx * cf - dz * sf;   /* positive = player to enemy's right */

    /* Amiga quadrant logic (tst/cmp/bgt chain):
     *   FacingTowardsPlayer (fwd > 0):
     *     lat > 0 && lat > fwd  → RIGHT(1)
     *     lat > 0 && lat <= fwd → TOWARDS(0)
     *     lat <= 0 && -lat > fwd → LEFT(3)
     *     lat <= 0 && -lat <= fwd → TOWARDS(0)
     *   FacingAway (fwd <= 0):
     *     lat > 0 && lat > -fwd → RIGHT(1)
     *     lat > 0 && lat <= -fwd → AWAY(2)
     *     lat <= 0 && -lat > -fwd → LEFT(3)
     *     lat <= 0 && -lat <= -fwd → AWAY(2)
     */
    if (fwd > 0) {
        if (lat > 0)
            return (lat > fwd)  ? 1 : 0;  /* RIGHT or TOWARDS */
        else
            return (-lat > fwd) ? 3 : 0;  /* LEFT  or TOWARDS */
    } else {
        int32_t afwd = -fwd;
        if (lat > 0)
            return (lat > afwd)  ? 1 : 2; /* RIGHT or AWAY */
        else
            return (-lat > afwd) ? 3 : 2; /* LEFT  or AWAY */
    }
}

/* -----------------------------------------------------------------------
 * enemy_update_anim - set raw[8..11] (OBJ_DEADH|OBJ_DEADL) each tick.
 * Translates Amiga: asl.l #2,d0; add.l alframe,d0 [; add.l #vectnum_high,d0]; move.l d0,8(a0)
 * vect_num is the sprite WAD index for this enemy type (high word of 8(a0)).
 * frame = angle*4 + walk_step (0-15, low word of 8(a0)).
 * ----------------------------------------------------------------------- */
static void enemy_update_anim(GameObject *obj, GameState *state, int16_t vect_num)
{
    /* Which player to use for viewpoint (use player 1, or 2 if 1 has invalid zone) */
    int16_t plr_x, plr_z;
    if (state->plr1.zone >= 0) {
        plr_x = (int16_t)state->plr1.p_xoff;
        plr_z = (int16_t)state->plr1.p_zoff;
    } else {
        plr_x = (int16_t)state->plr2.p_xoff;
        plr_z = (int16_t)state->plr2.p_zoff;
    }

    int angle = enemy_viewpoint(obj, plr_x, plr_z, &state->level);
    int walk_step = (walk_cycle >> 3) & 3;  /* 0-3, steps every 8 ticks */
    int16_t frame = (int16_t)(angle * 4 + walk_step);

    /* Store as big-endian long at raw[8..11]:
     * raw[8..9] = vect_num (high word), raw[10..11] = frame (low word) */
    wbe16(obj->raw + 8,  vect_num);
    wbe16(obj->raw + 10, frame);
}

/* -----------------------------------------------------------------------
 * Set world width/height in each object record from type defaults when unset (Amiga: each type sets 6(a0)).
 * ----------------------------------------------------------------------- */
void object_init_world_sizes_from_types(LevelState *level)
{
    if (!level->object_data || level->num_object_points <= 0) return;
    for (int i = 0; i < level->num_object_points; i++) {
        uint8_t *raw = level->object_data + i * OBJECT_SIZE;
        int t = (int8_t)raw[16];
        if (t < 0 || t > 20) continue;
        if (raw[6] == 0 || raw[7] == 0) {
            raw[6] = (uint8_t)default_object_world_size[t].w;
            raw[7] = (uint8_t)default_object_world_size[t].h;
        }
    }
}

/* -----------------------------------------------------------------------
 * Object iteration helpers
 * ----------------------------------------------------------------------- */
static GameObject *get_object(LevelState *level, int index)
{
    if (!level->object_data) return NULL;
    return (GameObject *)(level->object_data + index * OBJECT_SIZE);
}

/* Get object X/Z position from ObjectPoints array (big-endian) */
static void get_object_pos(const LevelState *level, int index,
                           int16_t *x, int16_t *z)
{
    if (level->object_points) {
        const uint8_t *p = level->object_points + index * 8;
        *x = obj_w(p);
        *z = obj_w(p + 4);
    } else {
        *x = 0;
        *z = 0;
    }
}

/* -----------------------------------------------------------------------
 * Enemy common: check damage and handle death
 *
 * Returns: true if enemy is dead (caller should return early)
 * ----------------------------------------------------------------------- */
static bool enemy_check_damage(GameObject *obj, const EnemyParams *params, GameState *state)
{
    int8_t damage = NASTY_DAMAGE(*obj);
    if (damage <= 0) return false;

    NASTY_DAMAGE(*obj) = 0;

    /* Apply damage reduction */
    if (params->damage_shift > 0) {
        damage >>= params->damage_shift;
        if (damage < 1) damage = 1;
    }

    int8_t lives = NASTY_LIVES(*obj);
    lives -= damage;

    int obj_idx = (int)(((uint8_t *)obj - state->level.object_data) / OBJECT_SIZE);
    int lives_before = (int)NASTY_LIVES(*obj);
    printf("[ENEMY] damage obj=%d type=%d applied=%d lives %d -> %d%s\n",
           obj_idx, (int)obj->obj.number, (int)damage, lives_before, (int)lives,
           lives <= 0 ? " (killed)" : "");

    if (lives <= 0) {
        /* Death */
        if (params->death_sound >= 0) {
            audio_play_sample(params->death_sound, 64);
        }
        /* Marines (Flame/Tough/Mutant): also play scream on death */
        if (params->scream_sound >= 0 &&
            (obj->obj.number == OBJ_NBR_FLAME_MARINE ||
             obj->obj.number == OBJ_NBR_TOUGH_MARINE ||
             obj->obj.number == OBJ_NBR_MARINE)) {
            audio_play_sample(params->scream_sound, 50);
        }

        /* Amiga: ExplodeIntoBits is always called on death when damage > 1.
         * explode_threshold > 0 means "instant kill threshold" (skip death animation),
         * not the gib-spawn threshold.
         * All enemies: gibs only, no explosion animation. */
        if (damage > 1) {
            explode_into_bits(obj, state);
        }

        /* Instant kill (Amiga: damage/4 >= 40 → zone=-1, no animation).
         * explode_threshold > 0: skip death animation if damage >= threshold. */
        bool instant_kill = (params->explode_threshold > 0 && damage >= params->explode_threshold);

        if (!instant_kill && params->death_frames[0] >= 0) {
            /* Store original type so we can advance animation and render (type_data[0]=death index, [1]=original type) */
            int8_t original_type = obj->obj.number;
            obj->obj.type_data[0] = 0;   /* death frame index */
            obj->obj.type_data[1] = original_type;
            OBJ_SET_DEADH(obj, params->death_frames[0]);  /* first frame number to display */
            OBJ_SET_DEADL(obj, 0);
            obj->obj.number = OBJ_NBR_DEAD;
            /* Keep zone so object is still processed for death advance and drawing */
        } else {
            OBJ_SET_ZONE(obj, -1); /* Instant kill or no animation: remove from active */
        }
        return true;
    }

    NASTY_LIVES(*obj) = lives;

    /* Hurt scream */
    if (params->scream_sound >= 0) {
        audio_play_sample(params->scream_sound, 50);
    }

    return false;
}

/* -----------------------------------------------------------------------
 * Enemy common: wander behavior
 *
 * When no player is visible, change direction randomly.
 * Translated from the common pattern in all enemy .s files.
 * ----------------------------------------------------------------------- */
static void enemy_wander(GameObject *obj, const EnemyParams *params,
                         GameState *state)
{
    int16_t timer = NASTY_TIMER(*obj);
    timer -= state->temp_frames;

    if (timer <= 0) {
        /* Change direction randomly */
        int16_t new_facing = (int16_t)(rand() & 8190);
        NASTY_SET_FACING(*obj, new_facing);
        timer = params->wander_timer + (int16_t)(rand() & 0x3F);
    }

    NASTY_SET_TIMER(*obj, timer);

    /* Move in facing direction */
    int16_t facing = NASTY_FACING(*obj);
    int16_t speed = NASTY_MAXSPD(*obj);
    if (speed == 0) speed = 4;

    int16_t s = sin_lookup(facing);
    int16_t c = cos_lookup(facing);

    int16_t obj_x, obj_z;
    int idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
    get_object_pos(&state->level, idx, &obj_x, &obj_z);

    MoveContext ctx;
    move_context_init(&ctx);
    ctx.oldx = obj_x;
    ctx.oldz = obj_z;
    ctx.newx = obj_x - ((int32_t)s * speed * state->temp_frames) / 16384;
    ctx.newz = obj_z - ((int32_t)c * speed * state->temp_frames) / 16384;
    ctx.thing_height = params->thing_height;
    ctx.step_up_val = params->step_up;
    ctx.step_down_val = params->step_down;
    ctx.extlen = params->extlen;
    ctx.awayfromwall = params->awayfromwall;
    ctx.collide_flags = 0x3F7C1; /* standard enemy collision mask */
    ctx.stood_in_top = obj->obj.in_top;
    /* Set objroom from current zone so move_object uses zone-based collision and updates zone on transition */
    if (OBJ_ZONE(obj) >= 0 && state->level.zone_adds && state->level.data &&
        OBJ_ZONE(obj) < state->level.num_zones) {
        int32_t zo = (int32_t)be32(state->level.zone_adds + (uint32_t)OBJ_ZONE(obj) * 4u);
        ctx.objroom = (uint8_t *)(state->level.data + zo);
    }

    move_object(&ctx, &state->level);

    /* Update position and zone after move */
    if (state->level.object_points) {
        uint8_t *pts = state->level.object_points + idx * 8;
        obj_sw(pts, (int16_t)ctx.newx);
        obj_sw(pts + 4, (int16_t)ctx.newz);
    }
    if (ctx.objroom && state->level.data) {
        int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
        if (new_zone >= 0 && new_zone < state->level.num_zones) {
            OBJ_SET_ZONE(obj, new_zone);
        } else {
            int32_t roompt = (int32_t)(ctx.objroom - state->level.data);
            for (int16_t z = 0; z < state->level.num_zones; z++) {
                if ((int32_t)be32(state->level.zone_adds + (uint32_t)z * 4u) == roompt) {
                    OBJ_SET_ZONE(obj, z);
                    break;
                }
            }
        }
        obj->obj.in_top = ctx.stood_in_top;
    }

    /* If hit wall, reverse direction */
    if (ctx.hitwall) {
        NASTY_SET_FACING(*obj, (facing + ANGLE_180) & ANGLE_MASK);
    }
}

/* -----------------------------------------------------------------------
 * Enemy common: attack behavior
 *
 * When player is visible, move toward them and attack.
 * ----------------------------------------------------------------------- */
static void enemy_attack(GameObject *obj, const EnemyParams *params,
                         GameState *state, int player_num)
{
    PlayerState *plr = (player_num == 1) ? &state->plr1 : &state->plr2;

    int16_t obj_x, obj_z;
    int idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
    get_object_pos(&state->level, idx, &obj_x, &obj_z);

    int32_t target_x = (int32_t)plr->p_xoff;
    int32_t target_z = (int32_t)plr->p_zoff;

    /* Calculate distance to player */
    int32_t dx = target_x - obj_x;
    int32_t dz = target_z - obj_z;
    int32_t dist = calc_dist_approx(dx, dz);

    /* Move toward player */
    int16_t facing = NASTY_FACING(*obj);
    int16_t speed = NASTY_MAXSPD(*obj);
    if (speed == 0) speed = 6;

    MoveContext ctx;
    move_context_init(&ctx);
    ctx.oldx = obj_x;
    ctx.oldz = obj_z;
    ctx.thing_height = params->thing_height;
    ctx.step_up_val = params->step_up;
    ctx.step_down_val = params->step_down;
    ctx.extlen = params->extlen;
    ctx.awayfromwall = params->awayfromwall;
    ctx.stood_in_top = obj->obj.in_top;
    /* Set objroom from current zone so move_object uses zone-based collision and updates zone on transition */
    if (OBJ_ZONE(obj) >= 0 && state->level.zone_adds && state->level.data &&
        OBJ_ZONE(obj) < state->level.num_zones) {
        int32_t zo = (int32_t)be32(state->level.zone_adds + (uint32_t)OBJ_ZONE(obj) * 4u);
        ctx.objroom = (uint8_t *)(state->level.data + zo);
    }

    head_towards_angle(&ctx, &facing, target_x, target_z,
                       speed * state->temp_frames, 120);
    NASTY_SET_FACING(*obj, facing);

    move_object(&ctx, &state->level);

    /* Update position and zone after move */
    if (state->level.object_points) {
        uint8_t *pts = state->level.object_points + idx * 8;
        obj_sw(pts, (int16_t)ctx.newx);
        obj_sw(pts + 4, (int16_t)ctx.newz);
    }
    if (ctx.objroom && state->level.data) {
        int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
        if (new_zone >= 0 && new_zone < state->level.num_zones) {
            OBJ_SET_ZONE(obj, new_zone);
        } else {
            int32_t roompt = (int32_t)(ctx.objroom - state->level.data);
            for (int16_t z = 0; z < state->level.num_zones; z++) {
                if ((int32_t)be32(state->level.zone_adds + (uint32_t)z * 4u) == roompt) {
                    OBJ_SET_ZONE(obj, z);
                    break;
                }
            }
        }
        obj->obj.in_top = ctx.stood_in_top;
    }

    /* Ranged attack */
    if (params->shot_type >= 0 && dist > params->melee_range) {
        /* Check cooldown (reuse SecTimer area) */
        int8_t *cooldown = (int8_t*)&obj->obj.type_data[8]; /* FourthTimer */
        *cooldown -= (int8_t)state->temp_frames;
        if (*cooldown <= 0) {
            enemy_fire_at_player(obj, state, player_num,
                                 params->shot_type, params->shot_power,
                                 params->shot_speed, params->shot_shift);
            *cooldown = (int8_t)(params->shot_cooldown +
                                 (rand() & 0x1F));
        }
    }

    /* Melee attack */
    if (params->melee_damage > 0 && dist <= params->melee_range) {
        int8_t *melee_cd = (int8_t*)&obj->obj.type_data[10]; /* FourthTimer area */
        *melee_cd -= (int8_t)state->temp_frames;
        if (*melee_cd <= 0) {
            plr->energy -= params->melee_damage;
            *melee_cd = (int8_t)params->melee_cooldown;
            if (params->attack_sound >= 0) {
                audio_play_sample(params->attack_sound, 50);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Map ObjNumber -> enemy_params index
 * ----------------------------------------------------------------------- */
static int obj_type_to_enemy_index(int8_t obj_type)
{
    switch (obj_type) {
    case OBJ_NBR_ALIEN:          return 0;
    case OBJ_NBR_ROBOT:          return 1;
    case OBJ_NBR_HUGE_RED_THING: return 2;
    case OBJ_NBR_WORM:           return 3;
    case OBJ_NBR_FLAME_MARINE:   return 4;
    case OBJ_NBR_TOUGH_MARINE:   return 5;
    case OBJ_NBR_MARINE:         return 6;  /* Mutant Marine */
    case OBJ_NBR_BIG_NASTY:      return 7;
    case OBJ_NBR_SMALL_RED_THING:return 8;  /* BigClaws uses same slot */
    case OBJ_NBR_FLYING_NASTY:   return 9;
    case OBJ_NBR_TREE:           return 10;
    default:                     return -1;
    }
}

/* -----------------------------------------------------------------------
 * Update obj->obj.can_see via CanItBeSeen for both players.
 * Bit 0 = can see player 1, bit 1 = can see player 2.
 * Called by every enemy handler each tick (Amiga: each NormalAlien / MutantMarine
 * calls CanItBeSeen itself in its prowl/attack routine).
 * ----------------------------------------------------------------------- */
/* Return non-zero if the zone at zone_id has an upper floor section. */
static int zone_has_upper_floor(const GameState *state, int16_t zone_id)
{
    if (!state->level.zone_adds || !state->level.data || zone_id < 0) return 0;
    int32_t zo = (int32_t)be32(state->level.zone_adds + (uint32_t)zone_id * 4u);
    if (zo <= 0) return 0;
    return be32(state->level.data + zo + 10) != 0;  /* ZD_UPPER_FLOOR != 0 */
}

static void enemy_update_can_see(GameObject *obj, GameState *state)
{
    int16_t enemy_zone = OBJ_ZONE(obj);
    if (enemy_zone < 0) return;
    const uint8_t *from_room = level_get_zone_data_ptr(&state->level, enemy_zone);
    if (!from_room) return;

    int enemy_cid = (int)OBJ_CID(obj);
    int16_t enemy_x = 0, enemy_z = 0;
    get_object_pos(&state->level, enemy_cid, &enemy_x, &enemy_z);
    int16_t viewer_y = (int16_t)obj_w(obj->raw + 4);

    obj->obj.can_see = 0;

    /* Player 1 → bit 0 */
    {
        int16_t plr_zone = state->plr1.zone;

        if (plr_zone == enemy_zone) {
            /* Same zone: always visible unless this is a two-level zone and
             * the enemy and player are on different floors. */
            int same_floor = !zone_has_upper_floor(state, enemy_zone) ||
                             (obj->obj.in_top == state->plr1.stood_in_top);
            if (same_floor) obj->obj.can_see |= 0x01;
        } else {
            const uint8_t *to_room = level_get_zone_data_ptr(&state->level, plr_zone);
            if (to_room) {
                int16_t plr_x    = (int16_t)state->plr1.p_xoff;
                int16_t plr_z    = (int16_t)state->plr1.p_zoff;
                int16_t target_y = (int16_t)(state->plr1.p_yoff >> 7);
                uint8_t vis = can_it_be_seen(&state->level,
                                             from_room, to_room, plr_zone,
                                             enemy_x, enemy_z, viewer_y,
                                             plr_x, plr_z, target_y,
                                             obj->obj.in_top,
                                             state->plr1.stood_in_top, 0);
                if (vis) obj->obj.can_see |= 0x01;
            }
        }
    }
    /* Player 2 → bit 1 */
    {
        int16_t plr_zone = state->plr2.zone;

        if (plr_zone == enemy_zone) {
            int same_floor = !zone_has_upper_floor(state, enemy_zone) ||
                             (obj->obj.in_top == state->plr2.stood_in_top);
            if (same_floor) obj->obj.can_see |= 0x02;
        } else {
            const uint8_t *to_room = level_get_zone_data_ptr(&state->level, plr_zone);
            if (to_room) {
                int16_t plr_x    = (int16_t)state->plr2.p_xoff;
                int16_t plr_z    = (int16_t)state->plr2.p_zoff;
                int16_t target_y = (int16_t)(state->plr2.p_yoff >> 7);
                uint8_t vis = can_it_be_seen(&state->level,
                                             from_room, to_room, plr_zone,
                                             enemy_x, enemy_z, viewer_y,
                                             plr_x, plr_z, target_y,
                                             obj->obj.in_top,
                                             state->plr2.stood_in_top, 0);
                if (vis) obj->obj.can_see |= 0x02;
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Generic enemy handler - used by all enemy types
 * ----------------------------------------------------------------------- */
static void enemy_generic(GameObject *obj, GameState *state, int param_index)
{
    if (!state->nasty) return;
    if (param_index < 0 || param_index >= num_enemy_types) return;

    const EnemyParams *params = &enemy_params[param_index];

    /* Check damage */
    if (enemy_check_damage(obj, params, state)) return;

    int8_t lives = NASTY_LIVES(*obj);
    if (lives <= 0) return;

    enemy_update_can_see(obj, state);

    /* Bit 0 = player 1 visible, bit 1 = player 2 visible */
    int8_t can_see = obj->obj.can_see;
    if (can_see & 0x01) {
        enemy_attack(obj, params, state, 1);
    } else if (can_see & 0x02) {
        enemy_attack(obj, params, state, 2);
    } else {
        enemy_wander(obj, params, state);
    }

    /* Update sprite animation frame (Amiga: ViewpointToDraw + alframe → 8(a0)).
     * Vect_num per type from Amiga .s files: NormalAlien=0, Robot=$5, BigNasty=$3,
     * FlyingNasty=0(EyeBall), Worm=$d, HugeRed/BigClaws=$e, SmallRed/BigRed=$e,
     * Tree=$f, EyeBall=0. */
    {
        static const int16_t vect_by_type[] = {
            /* OBJ_NBR_ALIEN(0)*/       0,
            /* OBJ_NBR_MEDIKIT(1)*/    -1,
            /* OBJ_NBR_BULLET(2)*/     -1,
            /* OBJ_NBR_BIG_GUN(3)*/    -1,
            /* OBJ_NBR_KEY(4)*/        -1,
            /* OBJ_NBR_PLR1(5)*/       -1,
            /* OBJ_NBR_ROBOT(6)*/       5,
            /* OBJ_NBR_BIG_NASTY(7)*/   3,
            /* OBJ_NBR_FLYING_NASTY(8)*/4,
            /* OBJ_NBR_AMMO(9)*/       -1,
            /* OBJ_NBR_BARREL(10)*/    -1,
            /* OBJ_NBR_PLR2(11)*/      -1,
            /* OBJ_NBR_MARINE(12)*/    10,
            /* OBJ_NBR_WORM(13)*/      13,
            /* OBJ_NBR_HUGE_RED(14)*/  14,
            /* OBJ_NBR_SMALL_RED(15)*/ 14,
            /* OBJ_NBR_TREE(16)*/      15,
            /* OBJ_NBR_EYEBALL(17)*/    0,
            /* OBJ_NBR_TOUGH(18)*/     16,
            /* OBJ_NBR_FLAME(19)*/     17,
        };
        int8_t otype = obj->obj.number;
        if (otype >= 0 && otype < (int8_t)(sizeof(vect_by_type)/sizeof(vect_by_type[0]))) {
            int16_t vn = vect_by_type[(int)otype];
            if (vn >= 0)
                enemy_update_anim(obj, state, vn);
        }
    }
}

/* -----------------------------------------------------------------------
 * objects_update - Main per-frame object processing
 *
 * Translated from Anims.s ObjMoveAnim.
 * ----------------------------------------------------------------------- */
void objects_update(GameState *state)
{
    /* Process delayed blasts (barrel splash etc.) - frame-rate independent */
    for (int i = 0; i < state->num_pending_blasts; ) {
        if (state->current_ticks_ms >= state->pending_blasts[i].trigger_time_ms) {
            compute_blast(state,
                state->pending_blasts[i].x, state->pending_blasts[i].z, state->pending_blasts[i].y,
                state->pending_blasts[i].radius, state->pending_blasts[i].power);
            state->num_pending_blasts--;
            state->pending_blasts[i] = state->pending_blasts[state->num_pending_blasts];
            continue;
        }
        i++;
    }

    /* 1. Update player zones (from room pointer if available) */
    /* Zone is already maintained by player_full_control -> MoveObject */

    /* 2. Player shooting - called from game_loop */

    /* 3. Level mechanics */
    switch_routine(state);
    door_routine(state);

    /* Set stood_on_lift from current zone and Y (same frame: use zone floor from previous frame). */
    if (state->level.lift_data && state->level.zone_adds && state->level.data) {
        for (int p = 0; p < 2; p++) {
            PlayerState *plr = (p == 0) ? &state->plr1 : &state->plr2;
            plr->stood_on_lift = 0;
            int16_t zid = plr->zone;
            if (zid < 0 || zid >= state->level.num_zones) continue;
            int32_t zone_off = (int32_t)be32(state->level.zone_adds + (uint32_t)zid * 4u);
            if (zone_off < 0) continue;
            const uint8_t *zd = state->level.data + zone_off;
            int32_t zone_floor = be32(zd + ZONE_OFF_FLOOR);
            int32_t floor_y = zone_floor - plr->s_height;
            int32_t tol = 512;
            if (plr->s_yoff >= floor_y - tol && plr->s_yoff <= floor_y + tol) {
                const uint8_t *lift = state->level.lift_data;
                while (1) {
                    int16_t lz = be16(lift);
                    if (lz < 0) break;
                    if (lz == zid) {
                        plr->stood_on_lift = 1;
                        break;
                    }
                    lift += LIFT_ENTRY_SIZE;
                }
            }
        }
    }

    lift_routine(state);

    /* Water animations */
    do_water_anims(state);

    /* 4. Iterate all objects */
    if (!state->level.object_data) {
        return;
    }

    /* Animation timer */
    anim_timer -= state->temp_frames;
    if (anim_timer <= 0) {
        anim_timer = 2;
        /* Swap RipTear/otherrip buffers (for rendering) */
    }

    /* Advance walk-cycle counter (Amiga: alanptr steps through alan[] table each game tick).
     * walk_cycle 0-31 wraps; walk_step = walk_cycle >> 3 gives 0-3 (8 ticks per step). */
    walk_cycle = (uint8_t)((walk_cycle + (uint8_t)state->temp_frames) & 31u);

    int obj_index = 0;
    while (1) {
        GameObject *obj = get_object(&state->level, obj_index);
        if (!obj) break;
        if (OBJ_CID(obj) < 0) break;
        if (OBJ_ZONE(obj) < 0) {
            obj_index++;
            continue;
        }

        /* Amiga processes all objects with zone >= 0 every frame.
         * Just refresh worry so other systems (e.g. rendering hints) still work. */
        obj->obj.worry = 1;

        /* Update rendering Y position from zone floor height.
         * Anims.s: each handler writes 4(a0) = (ToZoneFloor >> 7) - offset.
         * The offset is per-type; barrel uses -60 (= its world_h).
         * Generic formula: obj[4] = (floor >> 7) - world_h, so that
         *   scr_y + half_h ≈ floor_screen_y  (sprite bottom sits on floor).
         * Proof: (obj[4]<<7 + world_h*128) = floor, matching the floor projection.
         *
         * For two-level zones, use in_top (loaded from the level file, kept in sync
         * by movement.c when the object crosses a floor boundary) to pick the floor. */
        {
            int16_t obj_zone = OBJ_ZONE(obj);
            if (obj_zone >= 0 && state->level.zone_adds && state->level.data) {
                int32_t zo = be32(state->level.zone_adds + obj_zone * 4);
                if (zo > 0) {
                    const uint8_t *zd = state->level.data + zo;
                    int32_t floor_h = be32(zd + 2);  /* default: ToZoneFloor (lower) */

                    /* Two-level zone: if upper floor is set and object is flagged as on
                     * the upper floor, use the upper floor height.
                     * Never modify in_top here – it is authoritative from the level file
                     * and is updated by movement.c when the object moves between floors. */
                    int32_t upper_floor = be32(zd + 10);  /* ZD_UPPER_FLOOR */
                    if (upper_floor != 0 && obj->obj.in_top) {
                        floor_h = upper_floor;
                    }

                    /* world_height at [7] is signed (e.g. barrel -60); only default when 0 */
                    int world_h = (int)(int8_t)obj->raw[7];
                    if (world_h == 0) world_h = 32;
                    int16_t render_y = (int16_t)((floor_h >> 7) - world_h);
                    obj_sw(obj->raw + 4, render_y);
                }
            }
        }

        /* Dispatch by object type */
        int8_t obj_type = obj->obj.number;
        int param_idx;

        switch (obj_type) {
        case OBJ_NBR_ALIEN:
            object_handle_alien(obj, state);
            break;
        case OBJ_NBR_MEDIKIT:
            object_handle_medikit(obj, state);
            break;
        case OBJ_NBR_BULLET:
            object_handle_bullet(obj, state);
            break;
        case OBJ_NBR_BIG_GUN:
            object_handle_big_gun(obj, state);
            break;
        case OBJ_NBR_KEY:
            object_handle_key(obj, state);
            break;
        case OBJ_NBR_PLR1:
        case OBJ_NBR_PLR2:
            break;
        case OBJ_NBR_ROBOT:
            object_handle_robot(obj, state);
            break;
        case OBJ_NBR_BIG_NASTY:
            object_handle_big_nasty(obj, state);
            break;
        case OBJ_NBR_FLYING_NASTY:
        case OBJ_NBR_EYEBALL:
            object_handle_flying_nasty(obj, state);
            break;
        case OBJ_NBR_AMMO:
            object_handle_ammo(obj, state);
            break;
        case OBJ_NBR_BARREL:
            object_handle_barrel(obj, state);
            break;
        case OBJ_NBR_MARINE:
        case OBJ_NBR_TOUGH_MARINE:
        case OBJ_NBR_FLAME_MARINE:
            object_handle_marine(obj, state);
            break;
        case OBJ_NBR_WORM:
            object_handle_worm(obj, state);
            break;
        case OBJ_NBR_HUGE_RED_THING:
        case OBJ_NBR_SMALL_RED_THING:
            object_handle_huge_red(obj, state);
            break;
        case OBJ_NBR_TREE:
            object_handle_tree(obj, state);
            break;
        case OBJ_NBR_GAS_PIPE:
            object_handle_gas_pipe(obj, state);
            break;
        case OBJ_NBR_DEAD:
            /* Advance death animation; original type stored in type_data[1] when we died.
             * Amiga: ThirdTimer counts down from 25, one step per ObjMoveAnim (~0.5 s at 50 Hz).
             * Advance every tick (dead_l >= 1) to match. */
            {
                int8_t original_type = obj->obj.type_data[1];
                param_idx = obj_type_to_enemy_index(original_type);
                if (param_idx >= 0 && param_idx < num_enemy_types) {
                    const EnemyParams *ep = &enemy_params[param_idx];
                    int8_t death_index = obj->obj.type_data[0];
                    int16_t dead_l = OBJ_DEADL(obj);
                    dead_l += state->temp_frames;
                    if (dead_l >= 1) {
                        dead_l = 0;
                        /* Only advance if next frame is valid; otherwise keep showing last frame (don't remove) */
                        if (death_index + 1 < 30 && ep->death_frames[death_index + 1] >= 0) {
                            death_index++;
                            obj->obj.type_data[0] = death_index;
                            OBJ_SET_DEADH(obj, ep->death_frames[death_index]);
                        }
                    }
                    OBJ_SET_DEADL(obj, dead_l);
                }
            }
            break;
        default:
            break;
        }

        /* Set display frame from viewpoint (camera) relative to enemy facing, so the renderer
         * gets the correct frame and down_strip. Amiga: ViewpointToDraw in enemy .s files
         * computes which of 8/16 rotational frames to show based on viewer angle vs facing. */
        if (NASTY_LIVES(*obj) > 0) {
            switch (obj_type) {
            case OBJ_NBR_ALIEN: case OBJ_NBR_ROBOT: case OBJ_NBR_BIG_NASTY:
            case OBJ_NBR_FLYING_NASTY: case OBJ_NBR_EYEBALL:
            case OBJ_NBR_MARINE: case OBJ_NBR_TOUGH_MARINE: case OBJ_NBR_FLAME_MARINE:
            case OBJ_NBR_WORM: case OBJ_NBR_HUGE_RED_THING: case OBJ_NBR_SMALL_RED_THING:
            case OBJ_NBR_TREE:
                {
                    int16_t obj_x, obj_z;
                    get_object_pos(&state->level, OBJ_CID(obj), &obj_x, &obj_z);
                    /* Viewer = current player (camera); use plr1 for single-player. */
                    int16_t view_x = (int16_t)(state->plr1.xoff >> 16);
                    int16_t view_z = (int16_t)(state->plr1.zoff >> 16);
                    int16_t facing = NASTY_FACING(*obj);
                    int16_t frame = viewpoint_to_draw_16(view_x, view_z, obj_x, obj_z, facing);
                    obj_sw(obj->raw + 10, frame);
                }
                break;
            default:
                break;
            }
        }

        obj_index++;
    }

    /* Process nasty_shot_data bullets and gibs (not in object_data list) */
    if (state->level.nasty_shot_data) {
        uint8_t *shots = state->level.nasty_shot_data;
        for (int j = 0; j < 20; j++) {
            GameObject *bullet = (GameObject *)(shots + j * OBJECT_SIZE);
            if (OBJ_ZONE(bullet) < 0) continue;
            if (bullet->obj.number != OBJ_NBR_BULLET) continue;
            object_handle_bullet(bullet, state);
        }
    }

    /* 5. Brightness animations */
    bright_anim_handler(state);
}

/* -----------------------------------------------------------------------
 * Update sprite rotation frames for visible enemies (call every display frame).
 * ----------------------------------------------------------------------- */
void objects_update_sprite_frames(GameState *state)
{
    if (!state->level.object_data || !state->level.zone_adds) return;

    uint8_t vis_zones[256];
    memset(vis_zones, 0, sizeof(vis_zones));
    if (state->zone_order_count > 0) {
        for (int i = 0; i < state->zone_order_count && i < 256; i++) {
            int16_t z = state->zone_order_zones[i];
            if (z >= 0 && z < 256) vis_zones[z] = 1;
        }
    } else {
        for (int z = 0; z < 256; z++) vis_zones[z] = 1;
    }

    int16_t view_x = (int16_t)(state->plr1.xoff >> 16);
    int16_t view_z = (int16_t)(state->plr1.zoff >> 16);

    int obj_index = 0;
    while (1) {
        GameObject *obj = get_object(&state->level, obj_index);
        if (!obj) break;
        if (OBJ_CID(obj) < 0) break;
        int16_t obj_zone = OBJ_ZONE(obj);
        if (obj_zone < 0 || obj_zone >= 256 || !vis_zones[obj_zone]) {
            obj_index++;
            continue;
        }
        if (NASTY_LIVES(*obj) <= 0) {
            obj_index++;
            continue;
        }

        int8_t obj_type = obj->obj.number;
        switch (obj_type) {
        case OBJ_NBR_ALIEN: case OBJ_NBR_ROBOT: case OBJ_NBR_BIG_NASTY:
        case OBJ_NBR_FLYING_NASTY: case OBJ_NBR_EYEBALL:
        case OBJ_NBR_MARINE: case OBJ_NBR_TOUGH_MARINE: case OBJ_NBR_FLAME_MARINE:
        case OBJ_NBR_WORM: case OBJ_NBR_HUGE_RED_THING: case OBJ_NBR_SMALL_RED_THING:
        case OBJ_NBR_TREE:
            {
                int16_t obj_x, obj_z;
                get_object_pos(&state->level, OBJ_CID(obj), &obj_x, &obj_z);
                int16_t facing = NASTY_FACING(*obj);
                int16_t frame = viewpoint_to_draw_16(view_x, view_z, obj_x, obj_z, facing);
                obj_sw(obj->raw + 10, frame);
            }
            break;
        default:
            break;
        }
        obj_index++;
    }
}

/* -----------------------------------------------------------------------
 * Enemy handlers - each delegates to generic handler with type params
 * ----------------------------------------------------------------------- */

void object_handle_alien(GameObject *obj, GameState *state)
{
    enemy_generic(obj, state, 0);
}

void object_handle_robot(GameObject *obj, GameState *state)
{
    enemy_generic(obj, state, 1);
}

void object_handle_huge_red(GameObject *obj, GameState *state)
{
    /* BigRedThing or SmallRedThing based on objNumber */
    if (obj->obj.number == OBJ_NBR_SMALL_RED_THING) {
        enemy_generic(obj, state, 8); /* BigClaws params */
    } else {
        enemy_generic(obj, state, 2);
    }
}

void object_handle_worm(GameObject *obj, GameState *state)
{
    enemy_generic(obj, state, 3);
}

void object_handle_marine(GameObject *obj, GameState *state)
{
    if (!state->nasty) return;

    int8_t type = obj->obj.number;

    /* Marines use the full AI decision tree from AI.s */
    /* First check damage (common to all enemy types) */
    int param_idx;
    if (type == OBJ_NBR_FLAME_MARINE)     param_idx = 4;
    else if (type == OBJ_NBR_TOUGH_MARINE) param_idx = 5;
    else                                   param_idx = 6;

    const EnemyParams *params = &enemy_params[param_idx];
    if (enemy_check_damage(obj, params, state)) return;

    int8_t lives = NASTY_LIVES(*obj);
    if (lives <= 0) return;

    enemy_update_can_see(obj, state);

    /* Armed marines advance at half speed (Amiga AI.s: armed units get speed >>= 1).
     * Halve NASTY_MAXSPD temporarily so enemy_attack uses the slower tactical pace,
     * then restore the original value for the next frame's wander/cooldown logic. */
    int8_t can_see = obj->obj.can_see;
    if (can_see & 0x01) {
        int16_t orig_spd = NASTY_MAXSPD(*obj);
        int16_t use_spd  = (orig_spd != 0) ? orig_spd : 6;
        NASTY_SET_MAXSPD(*obj, (int16_t)(use_spd >> 1));
        enemy_attack(obj, params, state, 1);
        NASTY_SET_MAXSPD(*obj, orig_spd);
    } else if (can_see & 0x02) {
        int16_t orig_spd = NASTY_MAXSPD(*obj);
        int16_t use_spd  = (orig_spd != 0) ? orig_spd : 6;
        NASTY_SET_MAXSPD(*obj, (int16_t)(use_spd >> 1));
        enemy_attack(obj, params, state, 2);
        NASTY_SET_MAXSPD(*obj, orig_spd);
    } else {
        enemy_wander(obj, params, state);
    }

    /* Update sprite animation frame */
    {
        int16_t vn = (type == OBJ_NBR_TOUGH_MARINE) ? 16 :
                     (type == OBJ_NBR_FLAME_MARINE)  ? 17 : 10;
        enemy_update_anim(obj, state, vn);
    }
}

void object_handle_big_nasty(GameObject *obj, GameState *state)
{
    enemy_generic(obj, state, 7);
}

void object_handle_big_claws(GameObject *obj, GameState *state)
{
    enemy_generic(obj, state, 8);
}

/* -----------------------------------------------------------------------
 * Flying Nasty / Eyeball
 *
 * Translated from FlyingScalyBall.s ItsAFlyingNasty.
 *
 * Unique behavior: continuous rotation, vertical bobbing.
 * ----------------------------------------------------------------------- */
void object_handle_flying_nasty(GameObject *obj, GameState *state)
{
    if (!state->nasty) return;

    const EnemyParams *params = &enemy_params[9];

    if (enemy_check_damage(obj, params, state)) return;

    int8_t lives = NASTY_LIVES(*obj);
    if (lives <= 0) return;

    /* Continuous rotation */
    int16_t facing = NASTY_FACING(*obj);
    int16_t turn_speed = obj->obj.type_data[14]; /* TurnSpeed field */
    if (turn_speed == 0) turn_speed = 50;
    facing = (facing + turn_speed * state->temp_frames) & ANGLE_MASK;
    NASTY_SET_FACING(*obj, facing);

    /* Vertical movement (bounce between floor and ceiling) */
    /* Reusing type_data fields for Y velocity */
    int16_t yvel = OBJ_TD_W(obj, 6);
    int32_t accypos = OBJ_TD_L(obj, 24);
    accypos += yvel * state->temp_frames;
    OBJ_SET_TD_L(obj, 24, accypos);
    OBJ_SET_TD_W(obj, 6, yvel);

    /* Update visibility and attack */
    enemy_update_can_see(obj, state);
    int8_t can_see = obj->obj.can_see;
    if (can_see & 0x01) {
        enemy_attack(obj, params, state, 1);
    } else if (can_see & 0x02) {
        enemy_attack(obj, params, state, 2);
    } else {
        enemy_wander(obj, params, state);
    }

    int16_t vn_fly = (obj->obj.number == OBJ_NBR_EYEBALL) ? 0 : 4;
    enemy_update_anim(obj, state, vn_fly);
}

/* -----------------------------------------------------------------------
 * Tree enemy
 *
 * Translated from Tree.s ItsATree.
 * ----------------------------------------------------------------------- */
void object_handle_tree(GameObject *obj, GameState *state)
{
    enemy_generic(obj, state, 10);
}

/* -----------------------------------------------------------------------
 * Gas pipe flame emitter
 *
 * Translated from Anims.s ItsAGasPipe (line ~1230-1325).
 * Periodically spawns flame projectiles in its facing direction.
 * ----------------------------------------------------------------------- */
void object_handle_gas_pipe(GameObject *obj, GameState *state)
{
    obj->obj.worry = 0;

    int16_t tf = state->temp_frames;

    /* ThirdTimer = delay before starting a burst */
    int16_t third = NASTY_TIMER(*obj);
    if (third > 0) {
        NASTY_SET_TIMER(*obj, (int16_t)(third - tf));
        OBJ_SET_TD_W(obj, 6, 5);   /* SecTimer */
        OBJ_SET_TD_W(obj, 10, 10);  /* FourthTimer */
        return;
    }

    /* FourthTimer = interval between shots in burst */
    int16_t fourth = OBJ_TD_W(obj, 10);
    fourth -= tf;
    if (fourth > 0) {
        OBJ_SET_TD_W(obj, 10, fourth);
        return;
    }
    OBJ_SET_TD_W(obj, 10, 10);

    int16_t sec = OBJ_TD_W(obj, 6);
    sec--;
    OBJ_SET_TD_W(obj, 6, sec);
    if (sec <= 0) {
        NASTY_SET_TIMER(*obj, OBJ_TD_W(obj, 14));
    }
    if (sec == 4) audio_play_sample(22, 200);

    /* Spawn flame projectile */
    if (!state->level.nasty_shot_data) return;
    uint8_t *shots = state->level.nasty_shot_data;
    GameObject *bullet = NULL;
    for (int i = 0; i < 20; i++) {
        GameObject *c = (GameObject*)(shots + i * OBJECT_SIZE);
        if (OBJ_ZONE(c) < 0) { bullet = c; break; }
    }
    if (!bullet) return;

    bullet->obj.number = OBJ_NBR_BULLET;
    OBJ_SET_ZONE(bullet, OBJ_ZONE(obj));
    int16_t src_y = (int16_t)((obj->raw[4] << 8) | obj->raw[5]);
    src_y -= 80;
    bullet->raw[4] = (uint8_t)(src_y >> 8);
    bullet->raw[5] = (uint8_t)(src_y);
    SHOT_SET_ACCYPOS(*bullet, (int32_t)src_y << 7);
    SHOT_STATUS(*bullet) = 0;
    SHOT_SET_YVEL(*bullet, 0);
    SHOT_SIZE(*bullet) = 3;
    SHOT_SET_FLAGS(*bullet, 0);
    SHOT_SET_GRAV(*bullet, 0);
    SHOT_POWER(*bullet) = 7;
    SHOT_SET_LIFE(*bullet, 0);

    /* Copy position from gas pipe to bullet in ObjectPoints */
    if (state->level.object_points && state->level.object_data) {
        int self_idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
        int bul_idx = (int)OBJ_CID(bullet);
        if (bul_idx >= 0) {
            uint8_t *sp = state->level.object_points + self_idx * 8;
            uint8_t *dp = state->level.object_points + bul_idx * 8;
            memcpy(dp, sp, 8);
        }
    }

    int16_t facing = NASTY_FACING(*obj);
    int16_t s = sin_lookup(facing);
    int16_t c = cos_lookup(facing);
    SHOT_SET_XVEL(*bullet, (int16_t)(((int32_t)s << 4) >> 16));
    SHOT_SET_ZVEL(*bullet, (int16_t)(((int32_t)c << 4) >> 16));
    NASTY_SET_EFLAGS(*bullet, 0x00100020);
    bullet->obj.worry = 127;
}

/* -----------------------------------------------------------------------
 * Barrel
 *
 * Translated from Anims.s ItsABarrel.
 * Barrels explode on enough damage, dealing area damage.
 * ----------------------------------------------------------------------- */
void object_handle_barrel(GameObject *obj, GameState *state)
{
    int8_t damage = NASTY_DAMAGE(*obj);
    if (damage <= 0) return;

    NASTY_DAMAGE(*obj) = 0;
    int8_t lives = NASTY_LIVES(*obj);
    /* If lives uninitialized (0) from level, treat as one-shot: any damage explodes */
    if (lives <= 0) lives = 1;
    lives -= damage;

    if (lives <= 0) {
        /* Get position and zone before removing object */
        int16_t bx, bz;
        get_object_pos(&state->level, (int)OBJ_CID(obj), &bx, &bz);
        int16_t zone = OBJ_ZONE(obj);
        int32_t y_floor = ((int32_t)obj_w(obj->raw + 4) + (int32_t)(int8_t)obj->raw[7]) << 7;
        if ((int8_t)obj->raw[7] == 0) y_floor = ((int32_t)obj_w(obj->raw + 4) + 32) << 7;
        y_floor += (88 << 7);   /* lower explosions so they sit on the ground */

        /* Spawn explosion particles: larger, more spread out, animate 25% slower. */
        {
            const int num_particles = 10;
            const int spread = 200;   /* wide spread for barrel */
            const int8_t barrel_size = 150;   /* 50% larger sprite */
            const int8_t barrel_anim_rate = 75;  /* 25% slower */
            for (int p = 0; p < num_particles && state->num_explosions < MAX_EXPLOSIONS; p++) {
                int16_t px = (int16_t)(bx + (int16_t)((rand() & (2*spread - 1)) - spread));
                int16_t pz = (int16_t)(bz + (int16_t)((rand() & (2*spread - 1)) - spread));
                explosion_spawn(state, px, pz, zone, y_floor, barrel_size, barrel_anim_rate);
            }
        }

        OBJ_SET_ZONE(obj, -1);

        /* Area damage after a short delay (frame-rate independent), so blast follows the visual */
        if (state->num_pending_blasts < MAX_PENDING_BLASTS) {
            int i = state->num_pending_blasts++;
            state->pending_blasts[i].x = (int32_t)bx;
            state->pending_blasts[i].z = (int32_t)bz;
            state->pending_blasts[i].y = 0;
            state->pending_blasts[i].radius = 280;
            state->pending_blasts[i].power = 40;
            state->pending_blasts[i].trigger_time_ms = state->current_ticks_ms + 100;  /* 100 ms delay */
        }

        audio_play_sample(15, 300);
    } else {
        NASTY_LIVES(*obj) = lives;
    }
}

/* -----------------------------------------------------------------------
 * Pickup: Medikit
 *
 * Translated from Anims.s ItsAMediKit (line ~1463-1591).
 * ----------------------------------------------------------------------- */
void object_handle_medikit(GameObject *obj, GameState *state)
{
    /* Check distance to each player */
    if (pickup_distance_check(obj, state, 1)) {
        PlayerState *plr = &state->plr1;
        if (plr->energy < PLAYER_MAX_ENERGY) {
            plr->energy += HEAL_FACTOR;
            if (plr->energy > PLAYER_MAX_ENERGY)
                plr->energy = PLAYER_MAX_ENERGY;
            printf("[PICKUP] player 1 picked up medikit\n");
            OBJ_SET_ZONE(obj, -1); /* Remove pickup */
            audio_play_sample(4, 50);
        }
    }
    if (state->mode != MODE_SINGLE && pickup_distance_check(obj, state, 2)) {
        PlayerState *plr = &state->plr2;
        if (plr->energy < PLAYER_MAX_ENERGY) {
            plr->energy += HEAL_FACTOR;
            if (plr->energy > PLAYER_MAX_ENERGY)
                plr->energy = PLAYER_MAX_ENERGY;
            printf("[PICKUP] player 2 picked up medikit\n");
            OBJ_SET_ZONE(obj, -1);
            audio_play_sample(4, 50);
        }
    }
}

/* -----------------------------------------------------------------------
 * Pickup: Ammo
 *
 * Translated from Anims.s ItsAnAmmoClip (line ~1601-1744).
 * ----------------------------------------------------------------------- */
void object_handle_ammo(GameObject *obj, GameState *state)
{
    /* Amiga ItsAnAmmoClip: set vect=PICKUPS(1) and frame from AMGR table every tick */
    int16_t ammo_gun_type = OBJ_TD_W(obj, 0);  /* AmmoType EQU 18 */
    if (ammo_gun_type >= 0 && ammo_gun_type < 8) {
        obj_sw(obj->raw + 8,  1);  /* objVectNumber  = PICKUPS vect */
        obj_sw(obj->raw + 10, (int16_t)ammo_graphic_table[(int)ammo_gun_type]);
    }

    if (pickup_distance_check(obj, state, 1)) {
        PlayerState *plr = &state->plr1;
        int gun_idx = plr->gun_selected;
        if (gun_idx >= 0 && gun_idx < MAX_GUNS) {
            int16_t ammo = plr->gun_data[gun_idx].ammo;
            ammo += AMMO_PER_CLIP * 8;
            if (ammo > MAX_AMMO_DISPLAY) ammo = (int16_t)MAX_AMMO_DISPLAY;
            plr->gun_data[gun_idx].ammo = ammo;
            printf("[PICKUP] player 1 picked up ammo (gun %d)\n", gun_idx);
            OBJ_SET_ZONE(obj, -1);
            audio_play_sample(11, 50);
        }
    }
    if (state->mode != MODE_SINGLE && pickup_distance_check(obj, state, 2)) {
        PlayerState *plr = &state->plr2;
        int gun_idx = plr->gun_selected;
        if (gun_idx >= 0 && gun_idx < MAX_GUNS) {
            int16_t ammo = plr->gun_data[gun_idx].ammo;
            ammo += AMMO_PER_CLIP * 8;
            if (ammo > MAX_AMMO_DISPLAY) ammo = (int16_t)MAX_AMMO_DISPLAY;
            plr->gun_data[gun_idx].ammo = ammo;
            printf("[PICKUP] player 2 picked up ammo (gun %d)\n", gun_idx);
            OBJ_SET_ZONE(obj, -1);
            audio_play_sample(11, 50);
        }
    }
}

/* -----------------------------------------------------------------------
 * Pickup: Key
 *
 * Translated from Anims.s ItsAKey (line ~1905-2010).
 * Keys set condition bits that unlock doors.
 * ----------------------------------------------------------------------- */
void object_handle_key(GameObject *obj, GameState *state)
{
    if (pickup_distance_check(obj, state, 1)) {
        /* Determine which key from shared "can_see" */
        game_conditions |= obj->obj.can_see;
        printf("[PICKUP] player 1 picked up key (key_id %d)\n", obj->obj.can_see);
        OBJ_SET_ZONE(obj, -1);
        audio_play_sample(4, 50);
    }
    if (state->mode != MODE_SINGLE && pickup_distance_check(obj, state, 2)) {
        game_conditions |= obj->obj.can_see;
        printf("[PICKUP] player 2 picked up key (key_id %d)\n", obj->obj.can_see);
        OBJ_SET_ZONE(obj, -1);
        audio_play_sample(4, 50);
    }
}

/* -----------------------------------------------------------------------
 * Pickup: Big Gun (weapon pickup)
 *
 * Translated from Anims.s ItsABigGun (line ~1748-1890).
 * ----------------------------------------------------------------------- */
void object_handle_big_gun(GameObject *obj, GameState *state)
{
    if (pickup_distance_check(obj, state, 1)) {
        int gun_idx = obj->obj.type_data[0]; /* which gun */
        if (gun_idx >= 0 && gun_idx < MAX_GUNS) {
            PlayerState *plr = &state->plr1;
            plr->gun_data[gun_idx].visible = -1; /* Mark as acquired */
            /* Add some ammo */
            int16_t ammo_add = ammo_in_guns[gun_idx] * 8;
            plr->gun_data[gun_idx].ammo += ammo_add;
            printf("[PICKUP] player 1 picked up big gun (gun %d)\n", gun_idx);
            OBJ_SET_ZONE(obj, -1);
            audio_play_sample(4, 50);
        }
    }
    if (state->mode != MODE_SINGLE && pickup_distance_check(obj, state, 2)) {
        int gun_idx = obj->obj.type_data[0];
        if (gun_idx >= 0 && gun_idx < MAX_GUNS) {
            PlayerState *plr = &state->plr2;
            plr->gun_data[gun_idx].visible = -1;
            int16_t ammo_add = ammo_in_guns[gun_idx] * 8;
            plr->gun_data[gun_idx].ammo += ammo_add;
            printf("[PICKUP] player 2 picked up big gun (gun %d)\n", gun_idx);
            OBJ_SET_ZONE(obj, -1);
            audio_play_sample(4, 50);
        }
    }
}

/* -----------------------------------------------------------------------
 * Bullet processing
 *
 * Translated from Anims.s ItsABullet (line ~2774-3384).
 * ----------------------------------------------------------------------- */
void object_handle_bullet(GameObject *obj, GameState *state)
{
    int16_t xvel = SHOT_XVEL(*obj);
    int16_t zvel = SHOT_ZVEL(*obj);
    int16_t yvel = SHOT_YVEL(*obj);
    int16_t grav = SHOT_GRAV(*obj);
    int16_t life = SHOT_LIFE(*obj);
    int16_t flags = SHOT_FLAGS(*obj);
    int8_t  shot_status = SHOT_STATUS(*obj);
    int8_t  shot_size = SHOT_SIZE(*obj);
    bool    timed_out = false;

    /* If already popping (impact animation), skip movement */
    if (shot_status != 0) {
        /* Pop animation is rendering-only, just decrement and remove */
        OBJ_SET_ZONE(obj, -1);
        return;
    }

    /* Check lifetime against gun data */
    if (shot_size >= 0 && shot_size < 8) {
        int16_t max_life = default_plr1_guns[shot_size].bullet_lifetime;
        if (max_life >= 0 && life >= max_life) {
            timed_out = true;
        }
    }
    /* Increment life */
    life += state->temp_frames;
    SHOT_SET_LIFE(*obj, life);

    /* Advance bullet animation (Amiga ItsABullet notpopping path).
     * BulletTypes[shot_size].anim_ptr drives size/vect/frame each tick. */
    if (shot_status == 0 && shot_size >= 0 && shot_size < MAX_BULLET_ANIM_IDX && bullet_anim_tables[shot_size]) {
        uint8_t anim_idx = SHOT_ANIM(*obj);
        const BulletAnimFrame *f = &bullet_anim_tables[shot_size][anim_idx];
        /* Wrap at end-of-sequence sentinel (width == -1) */
        if (f->width == (int8_t)-1) {
            anim_idx = 0;
            f = &bullet_anim_tables[shot_size][0];
        }
        /* Write size to obj[6:7], vect to obj[8:9], frame to obj[10:11] */
        obj->obj.width_or_3d  = f->width;
        obj->obj.world_height = f->height;
        obj_sw(obj->raw + 8,  f->vect_num);
        obj_sw(obj->raw + 10, f->frame_num);
        /* Update src_cols/rows (obj[14:15]).
         * Gibs (50-53): each frame is 16×16 in the alien sheet. eff = src*2, so src=8 → eff=16. */
        if (shot_size >= 50) {
            obj->raw[14] = 8;
            obj->raw[15] = 8;
        } else {
            obj->raw[14] = bullet_fly_src_cols[(uint8_t)shot_size < MAX_BULLET_ANIM_IDX ? (uint8_t)shot_size : 0];
            obj->raw[15] = bullet_fly_src_rows[(uint8_t)shot_size < MAX_BULLET_ANIM_IDX ? (uint8_t)shot_size : 0];
        }
        /* Advance for next tick */
        SHOT_ANIM(*obj) = anim_idx + 1;
    }

    /* Apply gravity to Y velocity */
    if (grav != 0) {
        int32_t grav_delta = (int32_t)grav * state->temp_frames;
        int32_t new_yvel = yvel + (int16_t)grav_delta;
        /* Clamp to ±10*256 */
        if (new_yvel > 10 * 256) new_yvel = 10 * 256;
        if (new_yvel < -10 * 256) new_yvel = -10 * 256;
        yvel = (int16_t)new_yvel;
        SHOT_SET_YVEL(*obj, yvel);
    }

    /* Position is in object_points at OBJ_CID (works for both object_data and nasty_shot_data bullets). */
    int idx = (int)OBJ_CID(obj);
    if (idx < 0 || (state->level.object_points && idx >= state->level.num_object_points)) {
        if (shot_size >= 50) {
            printf("[GIB-KILL] CID=%d out of range (num_pts=%d) size=%d\n",
                   idx, state->level.num_object_points, (int)shot_size);
        }
        OBJ_SET_ZONE(obj, -1);
        return;
    }
    int16_t bx, bz;
    get_object_pos(&state->level, idx, &bx, &bz);

    int16_t new_bx = bx + xvel * state->temp_frames;
    int16_t new_bz = bz + zvel * state->temp_frames;

    /* Update Y position */
    int32_t accypos = SHOT_ACCYPOS(*obj);
    int32_t y_delta = (int32_t)yvel * state->temp_frames;
    accypos += y_delta;
    SHOT_SET_ACCYPOS(*obj, accypos);

    /* Floor/roof collision (from Anims.s ItsABullet lines 2882-3015) */
    if (state->level.zone_adds && state->level.data && OBJ_ZONE(obj) >= 0) {
        const uint8_t *za = state->level.zone_adds;
        int16_t zone = OBJ_ZONE(obj);
        int32_t zone_off = (int32_t)((za[zone*4]<<24)|(za[zone*4+1]<<16)|
                           (za[zone*4+2]<<8)|za[zone*4+3]);
        const uint8_t *zd = state->level.data + zone_off;
        int zd_off = obj->obj.in_top ? 8 : 0;

        /* Roof check (Amiga: blt .nohitroof → hit when roof - accypos >= 10*128) */
        int32_t roof = (int32_t)((zd[6+zd_off]<<24)|(zd[7+zd_off]<<16)|
                       (zd[8+zd_off]<<8)|zd[9+zd_off]);
        if (roof - accypos >= 10 * 128) {
            if (shot_size >= 50) printf("[GIB-ROOF] roof=%d accypos=%d diff=%d\n",(int)roof,(int)accypos,(int)(roof-accypos));
            if (flags & 1) {
                /* Bounce off roof */
                SHOT_SET_YVEL(*obj, (int16_t)(-yvel));
                accypos = roof + 10 * 128;
                SHOT_SET_ACCYPOS(*obj, accypos);
                if (flags & 2) {
                    SHOT_SET_XVEL(*obj, xvel >> 1);
                    SHOT_SET_ZVEL(*obj, zvel >> 1);
                }
            } else {
                timed_out = true; /* Impact on roof */
            }
        }

        /* Floor check (Amiga: bgt .nohitfloor → hit when floor - accypos <= 10*128) */
        int32_t floor_h = (int32_t)((zd[2+zd_off]<<24)|(zd[3+zd_off]<<16)|
                          (zd[4+zd_off]<<8)|zd[5+zd_off]);
        if (floor_h - accypos <= 10 * 128) {
            if (shot_size >= 50) printf("[GIB-FLOOR] floor=%d accypos=%d diff=%d\n",(int)floor_h,(int)accypos,(int)(floor_h-accypos));
            if (flags & 1) {
                /* Bounce off floor */
                if (yvel > 0) {
                    SHOT_SET_YVEL(*obj, (int16_t)(-(yvel >> 1)));
                    accypos = floor_h - 10 * 128;
                    SHOT_SET_ACCYPOS(*obj, accypos);
                    if (flags & 2) {
                        SHOT_SET_XVEL(*obj, xvel >> 1);
                        SHOT_SET_ZVEL(*obj, zvel >> 1);
                    }
                }
            } else {
                timed_out = true; /* Impact on floor */
            }
        }
    }

    /* MoveObject for wall collision */
    MoveContext ctx;
    move_context_init(&ctx);
    ctx.oldx = bx;
    ctx.oldz = bz;
    ctx.newx = new_bx;
    ctx.newz = new_bz;
    ctx.newy = accypos - 5 * 128;
    ctx.thing_height = 10 * 128;
    ctx.step_up_val = 0;
    ctx.step_down_val = 0x1000000;
    ctx.extlen = 0;
    ctx.awayfromwall = -1;
    ctx.exitfirst = (flags & 1) ? 0 : 1;
    ctx.wallbounce = (int8_t)(flags & 1);
    ctx.stood_in_top = obj->obj.in_top;
    ctx.wall_flags = 0x0400;
    /* Set objroom from bullet's current zone so move_object uses zone-based collision and updates zone on transition */
    if (OBJ_ZONE(obj) >= 0 && state->level.zone_adds && state->level.data &&
        OBJ_ZONE(obj) < state->level.num_zones) {
        int32_t zo = (int32_t)be32(state->level.zone_adds + (uint32_t)OBJ_ZONE(obj) * 4u);
        ctx.objroom = (uint8_t *)(state->level.data + zo);
    }

    if (new_bx != bx || new_bz != bz) {
        move_object(&ctx, &state->level);
    }

    obj->obj.in_top = ctx.stood_in_top;

    /* Wall bounce physics (Anims.s lines 3098-3140) */
    if (ctx.wallbounce && ctx.hitwall) {
        /* Reflection: already handled by simple negation for now.
         * Full reflection would use wallxsize/wallzsize/walllength
         * but that requires MoveObject to output wall normal. */
        SHOT_SET_XVEL(*obj, (int16_t)(-xvel));
        SHOT_SET_ZVEL(*obj, (int16_t)(-zvel));
        if (flags & 2) {
            /* Friction on bounce */
            SHOT_SET_XVEL(*obj, SHOT_XVEL(*obj) >> 1);
            SHOT_SET_ZVEL(*obj, SHOT_ZVEL(*obj) >> 1);
        }
    } else if (!ctx.wallbounce && ctx.hitwall) {
        timed_out = true; /* Non-bouncing bullet hit a wall */
    }

    /* Impact handling */
    if (timed_out) {
        SHOT_STATUS(*obj) = 1; /* Set to popping */

        /* Hit sound */
        if (shot_size >= 0 && shot_size < 8 &&
            bullet_types[shot_size].hit_noise >= 0) {
            audio_play_sample(bullet_types[shot_size].hit_noise,
                              bullet_types[shot_size].hit_volume);
        }
        /* Gibs: splatter sound when they hit floor/roof/wall */
        if (shot_size >= 50) {
            audio_play_sample(13, 64);  /* splotch */
        }

        /* Explosive force + explosion animation */
        if (shot_size >= 0 && shot_size < 8 &&
            bullet_types[shot_size].explosive_force > 0) {
            explosion_spawn(state, (int16_t)ctx.newx, (int16_t)ctx.newz,
                            OBJ_ZONE(obj), accypos, 100, 100);
            compute_blast(state, ctx.newx, ctx.newz, accypos,
                          bullet_types[shot_size].explosive_force,
                          SHOT_POWER(*obj));
        }

        OBJ_SET_ZONE(obj, -1); /* Remove (pop animation is rendering only) */
        return;
    }

    /* Update position in ObjectPoints */
    if (state->level.object_points) {
        uint8_t *pts = state->level.object_points + idx * 8;
        obj_sw(pts, (int16_t)ctx.newx);
        obj_sw(pts + 4, (int16_t)ctx.newz);
    }

    /* Update render Y (obj[4]) from accypos so bullets/gibs draw at correct height */
    {
        int world_h = (int)(int8_t)obj->raw[7];
        if (world_h <= 0) world_h = 32;
        obj_sw(obj->raw + 4, (int16_t)((accypos >> 7) - world_h));
    }

    /* Update zone from room (zone word at offset 0; fallback derive from roompt if invalid) */
    if (ctx.objroom && state->level.data) {
        int16_t new_zone = (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]);
        if (new_zone >= 0 && new_zone < state->level.num_zones) {
            OBJ_SET_ZONE(obj, new_zone);
        } else if (state->level.zone_adds) {
            int32_t roompt = (int32_t)(ctx.objroom - state->level.data);
            for (int16_t z = 0; z < state->level.num_zones; z++) {
                if ((int32_t)be32(state->level.zone_adds + (uint32_t)z * 4u) == roompt) {
                    OBJ_SET_ZONE(obj, z);
                    break;
                }
            }
        }
    }

    /* ---- Object-to-object hit detection (ObjectMove.s Collision) ---- */
    uint32_t enemy_flags = NASTY_EFLAGS(*obj);
    if (enemy_flags == 0) return;

    if (!state->level.object_data) return;

    /* Bullet's collision box (Amiga: a3 = ColBoxTable + bullet_type*8) */
    int bullet_type = (int)obj->obj.number;
    if (bullet_type < 0 || bullet_type > 20) bullet_type = OBJ_NBR_BULLET;
    int bullet_width = col_box_table[bullet_type].width;

    int check_idx = 0;
    while (1) {
        GameObject *target = (GameObject*)(state->level.object_data +
                             check_idx * OBJECT_SIZE);
        if (OBJ_CID(target) < 0) break;

        if (OBJ_ZONE(target) < 0 || target == obj) {
            check_idx++;
            continue;
        }

        /* Check enemy flags bitmask */
        int tgt_type = target->obj.number;
        if (tgt_type < 0 || tgt_type > 20 || !(enemy_flags & (1u << tgt_type))) {
            check_idx++;
            continue;
        }

        /* Dead enemies (death animation or removed) do not receive shot hits */
        if (tgt_type == OBJ_NBR_DEAD) {
            check_idx++;
            continue;
        }
        /* Check lives (barrels with 0 lives can still be hit to trigger explosion) */
        if (NASTY_LIVES(*target) <= 0 && tgt_type != OBJ_NBR_BARREL) {
            check_idx++;
            continue;
        }

        /* Same floor (Amiga: objInTop(a0) eor StoodInTop; bne checkcol) */
        if (obj->obj.in_top != target->obj.in_top) {
            check_idx++;
            continue;
        }

        /* Height check using collision box.
         * raw[4] is (floor>>7)-world_h (sprite top); Amiga uses center, so use center = top + half_height. */
        const CollisionBox *box = &col_box_table[tgt_type];
        int16_t obj_y = (int16_t)(accypos >> 7);
        int16_t tgt_top = obj_w(target->raw + 4);
        int16_t tgt_center_y = (int16_t)((int32_t)tgt_top + box->half_height);
        int16_t ydiff = obj_y - tgt_center_y;
        if (ydiff < 0) ydiff = (int16_t)(-ydiff);
        if (ydiff > box->half_height) {
            check_idx++;
            continue;
        }

        /* Position (Amiga: (a1,d0.w*8) = target point index) */
        int16_t tx, tz;
        get_object_pos(&state->level, OBJ_CID(target), &tx, &tz);

        /* Amiga horizontal: at new position, max(|dx|,|dz|) - bullet_width <= target width */
        int32_t adx = (int32_t)tx - (int32_t)ctx.newx;
        int32_t adz = (int32_t)tz - (int32_t)ctx.newz;
        if (adx < 0) adx = -adx;
        if (adz < 0) adz = -adz;
        int32_t max_d = (adx > adz) ? adx : adz;
        if (max_d - (int32_t)bullet_width > (int32_t)box->width) {
            check_idx++;
            continue;
        }

        /* Amiga: bullet must have got closer (dist_new_sq <= dist_old_sq) */
        int32_t dx_old = (int32_t)tx - (int32_t)bx;
        int32_t dz_old = (int32_t)tz - (int32_t)bz;
        int32_t dist_old_sq = dx_old * dx_old + dz_old * dz_old;
        int32_t dist_new_sq = adx * adx + adz * adz;
        if (dist_new_sq > dist_old_sq) {
            check_idx++;
            continue;
        }

        /* HIT! Apply damage */
        NASTY_SET_DAMAGE(target, (int8_t)(NASTY_DAMAGE(*target) + SHOT_POWER(*obj)));

        /* Set bullet to popping */
        SHOT_STATUS(*obj) = 1;

        /* Hit sound + explosion */
        if (shot_size >= 0 && shot_size < 8 &&
            bullet_types[shot_size].hit_noise >= 0) {
            audio_play_sample(bullet_types[shot_size].hit_noise,
                              bullet_types[shot_size].hit_volume);
        }
        if (shot_size >= 0 && shot_size < 8 &&
            bullet_types[shot_size].explosive_force > 0) {
            explosion_spawn(state, (int16_t)ctx.newx, (int16_t)ctx.newz,
                            OBJ_ZONE(obj), accypos, 100, 100);
            compute_blast(state, ctx.newx, ctx.newz, accypos,
                          bullet_types[shot_size].explosive_force,
                          SHOT_POWER(*obj));
        }

        OBJ_SET_ZONE(obj, -1);
        return;
    }
}

/* Door wall list: 6 bytes per entry (fline w, gfx_off l). Floor line: 16 bytes, x/z at 0,2 and xlen/zlen at 4,6 (movement.c FLINE_*). */
#define DOOR_WALL_ENT_SIZE   6
#define DOOR_FLINE_SIZE      16
#define DOOR_FLINE_X         0
#define DOOR_FLINE_Z         2
#define DOOR_FLINE_XLEN      4
#define DOOR_FLINE_ZLEN      6
#define DOOR_NEAR_THRESH     4   /* player within this distance of a door fline counts as "at door" */

/* Returns true if player is in door's zone or within DOOR_NEAR_THRESH of any door wall floor line (dot with fline plane < 4). */
static bool player_at_door_zone(GameState *state, int16_t door_zone_id, int16_t player_zone, int door_idx, int plr_num)
{
    if (player_zone < 0) return false;
    if (player_zone == door_zone_id) return true;

    if (!state->level.door_wall_list || !state->level.door_wall_list_offsets || !state->level.floor_lines) return false;
    if (door_idx < 0 || (uint32_t)door_idx >= state->level.num_doors) return false;

    int32_t px, pz;
    if (plr_num == 0) {
        px = state->plr1.xoff >> 16;
        pz = state->plr1.zoff >> 16;
    } else {
        px = state->plr2.xoff >> 16;
        pz = state->plr2.zoff >> 16;
    }

    uint32_t start = state->level.door_wall_list_offsets[door_idx];
    uint32_t end   = state->level.door_wall_list_offsets[door_idx + 1];
    for (uint32_t j = start; j < end; j++) {
        const uint8_t *ent = state->level.door_wall_list + j * DOOR_WALL_ENT_SIZE;
        int16_t fi = be16(ent);
        if (fi < 0 || (int32_t)fi >= state->level.num_floor_lines) continue;
        const uint8_t *fl = state->level.floor_lines + (uint32_t)(int16_t)fi * DOOR_FLINE_SIZE;
        int32_t lx   = (int32_t)(int16_t)be16(fl + DOOR_FLINE_X);
        int32_t lz   = (int32_t)(int16_t)be16(fl + DOOR_FLINE_Z);
        int32_t lxlen = (int32_t)(int16_t)be16(fl + DOOR_FLINE_XLEN);
        int32_t lzlen = (int32_t)(int16_t)be16(fl + DOOR_FLINE_ZLEN);
        /* Signed distance from point to line (dot with plane): (P - A) · N where N = (-lzlen, lxlen). */
        int64_t cross = (int64_t)(px - lx) * lzlen - (int64_t)(pz - lz) * lxlen;
        int64_t len_sq = (int64_t)lxlen * lxlen + (int64_t)lzlen * lzlen;
        if (len_sq <= 0) continue;
        /* |cross|/sqrt(len_sq) < 4  <=>  cross*cross < 16*len_sq */
        if (cross < 0) cross = -cross;
        if (cross * cross < 4000 * len_sq) return true;
    }
    return false;
}

/* Same as player_at_door_zone but for lifts: uses lift wall list and returns true if player is in
 * the lift zone or within DOOR_NEAR_THRESH of any lift wall floor line. */
static bool player_at_lift_zone(GameState *state, int16_t lift_zone_id, int16_t player_zone, int lift_idx, int plr_num)
{
    if (player_zone < 0) return false;
    if (player_zone == lift_zone_id) return true;

    if (!state->level.lift_wall_list || !state->level.lift_wall_list_offsets || !state->level.floor_lines) return false;
    if (lift_idx < 0 || (uint32_t)lift_idx >= state->level.num_lifts) return false;

    int32_t px, pz;
    if (plr_num == 0) {
        px = state->plr1.xoff >> 16;
        pz = state->plr1.zoff >> 16;
    } else {
        px = state->plr2.xoff >> 16;
        pz = state->plr2.zoff >> 16;
    }

    uint32_t start = state->level.lift_wall_list_offsets[lift_idx];
    uint32_t end   = state->level.lift_wall_list_offsets[lift_idx + 1];
    for (uint32_t j = start; j < end; j++) {
        const uint8_t *ent = state->level.lift_wall_list + j * DOOR_WALL_ENT_SIZE;
        int16_t fi = be16(ent);
        if (fi < 0 || (int32_t)fi >= state->level.num_floor_lines) continue;
        const uint8_t *fl = state->level.floor_lines + (uint32_t)(int16_t)fi * DOOR_FLINE_SIZE;
        int32_t lx   = (int32_t)(int16_t)be16(fl + DOOR_FLINE_X);
        int32_t lz   = (int32_t)(int16_t)be16(fl + DOOR_FLINE_Z);
        int32_t lxlen = (int32_t)(int16_t)be16(fl + DOOR_FLINE_XLEN);
        int32_t lzlen = (int32_t)(int16_t)be16(fl + DOOR_FLINE_ZLEN);
        int64_t cross = (int64_t)(px - lx) * lzlen - (int64_t)(pz - lz) * lxlen;
        int64_t len_sq = (int64_t)lxlen * lxlen + (int64_t)lzlen * lzlen;
        if (len_sq <= 0) continue;
        if (cross < 0) cross = -cross;
        if (cross * cross < 4000 * len_sq) return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Door routine
 *
 * Translated from Anims.s DoorRoutine (line ~642-796).
 *
 * Door data format (per door, 16 bytes in DoorData array):
 *   0: zone index (word)
 *   2: door type (word) - 0=player+space, 1=condition, 2=condition2, etc.
 *   4: door position (long) - current Y (*256), 10: top (open), 14: bot (closed). 22 bytes per entry.
 *   8: door velocity (word) - current speed
 *  10: door max (word) - maximum opening height
 *  12: timer (word) - close delay
 *  14: flags (word) - for type 0: 0 = player-only (space at door); else condition mask (switch bit)
 * ----------------------------------------------------------------------- */
void door_routine(GameState *state)
{
    if (!state->level.door_data) return;

    uint8_t *door = state->level.door_data;
    int door_idx = 0;

    /* Iterate door entries (22 bytes each, terminated by -1). Same layout as lift: pos/top/bot (world *256). */
    while (1) {
        int16_t zone_id = be16(door);
        if (zone_id < 0) break;

        int16_t door_type = be16(door + 2);
        int32_t door_pos = be32(door + 4);
        int16_t door_vel = be16(door + 8);
        int32_t door_top = be32(door + 10)+1024;  /* open position (more negative) */
        int32_t door_bot = be32(door + 14);  /* closed position (more positive) */
        int16_t timer = be16(door + 18);
        uint16_t door_flags = (uint16_t)be16(door + 20);

        /* Type 0: flags 0 = player-only (space at door); flags != 0 = condition (switch or key). */
        if (door_type == 0) {
            int satisfied;
            int plr1_at = player_at_door_zone(state, zone_id, state->plr1.zone, door_idx, 0);
            int plr2_at = player_at_door_zone(state, zone_id, state->plr2.zone, door_idx, 1);
            int at_door = plr1_at || plr2_at;
            int space_tap = (plr1_at && state->plr1.p_spctap) || (plr2_at && state->plr2.p_spctap);

            if (door_flags == 0) {
                /* Player-only: open when player at door taps space; stay open while at door. */
                satisfied = at_door && (space_tap || door_pos < door_bot);
            } else {
                int conditions_met = ((uint16_t)game_conditions & door_flags) == door_flags;
                int is_switch_door = door_flags & 0xFF0;
                if (is_switch_door) {
                    satisfied = conditions_met;
                } else {
                    /* Key (or mixed) door: must have condition and be at door and interact (or door already open). */
                    satisfied = conditions_met && at_door && (space_tap || door_pos < door_bot);
                }
            }

            const int32_t open_speed = -16;
            const int32_t close_speed = 4;
            int16_t prev_vel = door_vel;
            if (satisfied) {
                if (door_pos > door_top)
                    door_vel = (int16_t)open_speed;
                else {
                    door_vel = 0;
                    door_pos = door_top;
                }
            } else {
                if (door_pos < door_bot)
                    door_vel = (int16_t)close_speed;
                else {
                    door_vel = 0;
                    door_pos = door_bot;
                }
            }
            if (door_vel != 0 && prev_vel == 0)
                audio_play_sample(5, 64);  /* newdoor: play when door starts opening or closing */
        }

        door_pos += (int32_t)door_vel * state->temp_frames * 64;
        if (door_pos < door_top) door_pos = door_top;
        if (door_pos > door_bot) door_pos = door_bot;

        /* Write back (big-endian) */
        wbe32(door + 4, door_pos);
        wbe16(door + 8, door_vel);
        wbe16(door + 18, timer);

        /* Update zone data: write door position directly to zone roof (same as Amiga). */
        if (zone_id >= 0 && zone_id < state->level.num_zones)
            level_set_zone_roof(&state->level, zone_id, door_pos);

        /* Amiga-style: patch floor line 14 and graphics wall record for each door wall (when data was loaded). */
        if (state->level.door_wall_list && state->level.door_wall_list_offsets &&
            state->level.graphics && door_idx < state->level.num_doors) {
            uint32_t start = state->level.door_wall_list_offsets[door_idx];
            uint32_t end   = state->level.door_wall_list_offsets[door_idx + 1];
            for (uint32_t j = start; j < end; j++) {
                const uint8_t *ent = state->level.door_wall_list + j * 6u;
                int16_t fline = be16(ent);
                int32_t gfx_off = (int32_t)be32(ent + 2);
                if (state->level.floor_lines && fline >= 0 && (int32_t)fline < state->level.num_floor_lines) {
                    uint8_t *fl = state->level.floor_lines + (uint32_t)(int16_t)fline * 16u;
                    wbe16(fl + 14, (int16_t)(uint16_t)0x8000);  /* door wall flag (Amiga doorwalls) */
                }
                if (gfx_off >= 0) {
                    uint8_t *wall_rec = state->level.graphics + (uint32_t)gfx_off;
                    uint8_t valshift = wall_rec[15];
                    uint8_t valand = wall_rec[14];
                    int shift = 0; // TODO: fix this properly
                    if (valshift == 8) shift = 7;
                    if (valshift == 6) shift = 8;
                    if (valshift == 4) shift = 9;
                    if (valshift == 2) shift = 10;
                    if (valshift == 1) shift = 11;
                    if (valshift == 0) shift = 12;
                    wbe32(wall_rec + 24, door_pos);   /* Amiga: move.l d3,24(a1) = door height for this wall */
                    int16_t yoff = (int16_t)((uint16_t)((-(door_pos >> shift)) & 0xFFu));
                    wbe32(wall_rec + 10, yoff);
                }
            }
        }

        door_idx++;
        door += 22;
    }
}

/* -----------------------------------------------------------------------
 * Lift routine
 *
 * Translated from Anims.s LiftRoutine (line ~377-627).
 *
 * Lift entry layout (20 bytes): zone(w), type(w), pos(l), vel(w), top(l), bot(l), flags(w).
 *   type: high byte = behaviour when at top (0=space/on lift, 1=player on lift, 2=auto, 3=no move),
 *         low byte = behaviour when at bottom (same).
 *   flags: 0 = always active; else same as door (satisfied when (game_conditions & flags) == flags).
 * ----------------------------------------------------------------------- */
void lift_routine(GameState *state)
{
    if (!state->level.lift_data) return;

    uint8_t *lift = state->level.lift_data;
    int lift_idx = 0;
    /* Per-zone max floor (×64 scale to match doors). Lifts store pos in ×256; we convert when writing to zone/wall. */
    //int32_t zone_max_floor[256];
    //uint8_t zone_lift_seen[256];
    //memset(zone_lift_seen, 0, sizeof(zone_lift_seen));

    /* Iterate lift entries (terminated by -1) */
    while (1) {
        int16_t zone_id = be16(lift);
        if (zone_id < 0) break;

        int16_t lift_type = be16(lift + 2);
        int32_t lift_pos = be32(lift + 4);
        int16_t lift_vel = be16(lift + 8);
        int16_t prev_lift_vel = lift_vel;
        int32_t lift_top = be32(lift + 10);
        int32_t lift_bot = be32(lift + 14);
        uint16_t lift_flags = (uint16_t)be16(lift + 18);

        int plr1_at = player_at_lift_zone(state, zone_id, state->plr1.zone, lift_idx, 0);
        int plr2_at = player_at_lift_zone(state, zone_id, state->plr2.zone, lift_idx, 1);
        int at_lift = plr1_at || plr2_at;
        int space_tap = (plr1_at && state->plr1.p_spctap) || (plr2_at && state->plr2.p_spctap);
        int plr1_on = state->plr1.stood_on_lift && plr1_at;
        int plr2_on = state->plr2.stood_on_lift && plr2_at;
        int any_on_lift = plr1_on || plr2_on;

        /* Conditions: when lift_flags == 0 always satisfied; else same as door (switch bits). */
        int satisfied = (lift_flags == 0) || (((uint16_t)game_conditions & lift_flags) == lift_flags);

        if (satisfied) {
            /* Amiga: two type bytes – high = when at top (can go down), low = when at bottom (can go up). */
            int type_at_top = (lift_type >> 8) & 0xFF;
            int type_at_bot = lift_type & 0xFF;
            int at_top = (lift_pos <= lift_top);
            int at_bot = (lift_pos >= lift_bot);

            if (at_top) {
                /* Decide whether to go down (positive vel = toward lift_bot). */
                switch (type_at_top) {
                    case 0: /* space or player on lift */
                        if (space_tap || (any_on_lift && at_lift))
                            lift_vel = 4;
                        break;
                    case 1: /* player on lift */
                        if (any_on_lift) lift_vel = 4;
                        break;
                    case 2: lift_vel = 4; break;  /* auto down */
                    case 3: lift_vel = 0; break; /* no move */
                    default: break;
                }
            } else if (at_bot) {
                /* Decide whether to go up (negative vel = toward lift_top). */
                switch (type_at_bot) {
                    case 0:
                        if (space_tap || (any_on_lift && at_lift))
                            lift_vel = -4;
                        break;
                    case 1:
                        if (any_on_lift) lift_vel = -4;
                        break;
                    case 2: lift_vel = -4; break;  /* auto up */
                    case 3: lift_vel = 0; break;
                    default: break;
                }
            }
            /* Else neither at top nor at bottom: keep current velocity (already moving). */
        }

        if (lift_vel != 0 && prev_lift_vel == 0)
            audio_play_sample(5, 64);  /* newdoor: play when lift starts moving (same as door) */

        int32_t old_pos = lift_pos;
        lift_pos += (int32_t)lift_vel * state->temp_frames * 64;
        if (lift_pos < lift_top) lift_pos = lift_top;
        if (lift_pos > lift_bot) lift_pos = lift_bot;
        int32_t lift_delta = lift_pos - old_pos;

        wbe32(lift + 4, lift_pos);
        wbe16(lift + 8, lift_vel);

        if (zone_id >= 0 && zone_id < state->level.num_zones)
        {
            level_set_zone_floor(&state->level, (int16_t)zone_id, lift_pos);
            level_set_zone_roof(&state->level, (int16_t)zone_id, lift_top);
        }

        /* Amiga: move players with the lift.
         * lift_pos and s_yoff share the same coordinate system (both come from/write to zone
         * data in the same scale), so adding lift_delta keeps the player glued to the platform. */
        if (lift_delta != 0) {
            if (plr1_on) {
                state->plr1.s_yoff  += lift_delta;
                state->plr1.s_tyoff += lift_delta;
                state->plr1.s_yvel   = 0;
            }
            if (plr2_on) {
                state->plr2.s_yoff  += lift_delta;
                state->plr2.s_tyoff += lift_delta;
                state->plr2.s_yvel   = 0;
            }
        }

        /* Amiga-style: patch floor line 14 and graphics wall record. */
        if (state->level.lift_wall_list && state->level.lift_wall_list_offsets &&
            state->level.graphics && lift_idx < state->level.num_lifts) {
            uint32_t start = state->level.lift_wall_list_offsets[lift_idx];
            uint32_t end   = state->level.lift_wall_list_offsets[lift_idx + 1];
            for (uint32_t j = start; j < end; j++) {
                const uint8_t *ent = state->level.lift_wall_list + j * 6u;
                int16_t fline = be16(ent);
                int32_t gfx_off = (int32_t)be32(ent + 2);
                if (state->level.floor_lines && fline >= 0 && (int32_t)fline < state->level.num_floor_lines) {
                    uint8_t *fl = state->level.floor_lines + (uint32_t)(int16_t)fline * 16u;
                    wbe16(fl + 14, (int16_t)(uint16_t)0x8000);
                }
                if (gfx_off >= 0) {
                    uint8_t *wall_rec = state->level.graphics + (uint32_t)gfx_off;
                    wbe32(wall_rec + 20, lift_pos);
                }
            }
        }

        lift_idx++;
        lift += LIFT_ENTRY_SIZE;
    }
}


/* -----------------------------------------------------------------------
 * Switch routine
 *
 * Translated from Anims.s SwitchRoutine (line ~868-1034).
 * ----------------------------------------------------------------------- */
void switch_routine(GameState *state)
{
    if (!state->level.switch_data) return;

    uint8_t *sw = state->level.switch_data;
    int switch_index = 0;

    while (1) {
        int16_t zone_id = be16(sw);
        if (zone_id < 0) break;

        /* Amiga: condition bit from switch index (offset 4 = point index). Loop d0=7..0 so switch 0→bit 4, 1→5, … 7→bit 11. */
        unsigned int bit_num = 4 + (switch_index % 8);
        uint16_t bit_mask = (uint16_t)(1u << bit_num);

        /* Update switch wall graphic every frame to match game_conditions (e.g. after load or other triggers). */
        if (state->level.graphics) {
            int32_t gfx_off = (int32_t)be32(sw + 6);
            uint8_t *wall_ptr = state->level.graphics + gfx_off;
            int16_t w = be16(wall_ptr);
            w = (int16_t)((w & ~2) | ((game_conditions & bit_mask) ? 2 : 0));
            write_be16(wall_ptr, w);
        }

        int8_t cooldown = *(int8_t*)(sw + 3);  /* byte 3 (Anims: sub.b d1,3(a0)) */

        /* Decrement cooldown */
        if (cooldown > 0) {
            cooldown -= (int8_t)(state->temp_frames * 4);
            if (cooldown < 0) cooldown = 0;
            *(int8_t*)(sw + 3) = cooldown;
        }

        /* Check if player is near, facing the switch, and pressing use (space).
         * Switch record is 14 bytes (Anims.s adda.w #14): zone(2), point_index(2), cooldown(1),
         * pad(1), gfx_offset(4), sw_x(2), sw_z(2). Condition bit derived from switch index. */
        if (cooldown == 0) {
            int16_t sw_x = be16(sw + 10); /* switch X position */
            int16_t sw_z = be16(sw + 12); /* switch Z position */

            bool near_plr1 = (state->plr1.zone == zone_id);
            bool near_plr2 = (state->plr2.zone == zone_id);

            if (near_plr1 && sw_x != 0) {
                int32_t dx = (int32_t)sw_x - state->plr1.p_xoff;
                int32_t dz = (int32_t)sw_z - state->plr1.p_zoff;
                near_plr1 = (dx * dx + dz * dz) < 3600;
                /* Facing check: view direction dot (to switch) > 0.5 * dist (within ~60°) */
                if (near_plr1) {
                    int32_t dist_sq = dx * dx + dz * dz;
                    if (dist_sq > 0) {
                        int16_t sang = sin_lookup((int)state->plr1.p_angpos & ANGLE_MASK);
                        int16_t cang = cos_lookup((int)state->plr1.p_angpos & ANGLE_MASK);
                        int32_t dot = (int32_t)dx * sang + (int32_t)dz * cang;
                        near_plr1 = (dot > 0 && (int64_t)dot * dot > (int64_t)dist_sq / 4);
                    }
                }
            }
            if (near_plr2 && sw_x != 0) {
                int32_t dx = (int32_t)sw_x - state->plr2.p_xoff;
                int32_t dz = (int32_t)sw_z - state->plr2.p_zoff;
                near_plr2 = (dx * dx + dz * dz) < 3600;
                if (near_plr2) {
                    int32_t dist_sq = dx * dx + dz * dz;
                    if (dist_sq > 0) {
                        int16_t sang = sin_lookup((int)state->plr2.p_angpos & ANGLE_MASK);
                        int16_t cang = cos_lookup((int)state->plr2.p_angpos & ANGLE_MASK);
                        int32_t dot = (int32_t)dx * sang + (int32_t)dz * cang;
                        near_plr2 = (dot > 0 && (int64_t)dot * dot > (int64_t)dist_sq / 4);
                    }
                }
            }

            if (state->plr1.p_spctap && near_plr1) {
                game_conditions ^= bit_mask;
                printf("[SWITCH] pressed (plr1) zone=%d bit_mask=0x%04X game_conditions=0x%04X\n",
                       (int)zone_id, (unsigned)(uint16_t)bit_mask, (unsigned)(uint16_t)game_conditions);
                *(int8_t*)(sw + 3) = 20; /* cooldown */
                audio_play_sample(10, 50);
            }
            if (state->plr2.p_spctap && near_plr2) {
                game_conditions ^= bit_mask;
                printf("[SWITCH] pressed (plr2) zone=%d bit_mask=0x%04X game_conditions=0x%04X\n",
                       (int)zone_id, (unsigned)(uint16_t)bit_mask, (unsigned)(uint16_t)game_conditions);
                *(int8_t*)(sw + 3) = 20;
                audio_play_sample(10, 50);
            }
        }

        sw += 14;
        switch_index++;
    }
}

/* -----------------------------------------------------------------------
 * Water animations
 *
 * Translated from Anims.s DoWaterAnims (line ~322-373).
 * ----------------------------------------------------------------------- */
void do_water_anims(GameState *state)
{
    /* Water animation: iterate zones with water, oscillate level.
     * The original iterates a WaterList structure with entries for each
     * water zone, storing current level, min, max, speed, direction.
     * When level data provides this list, the water floor height oscillates. */
    if (!state->level.water_list) return;

    uint8_t *wl = state->level.water_list;
    while (1) {
        int16_t zone_id = (int16_t)((wl[0] << 8) | wl[1]);
        if (zone_id < 0) break;

        int32_t cur_level = (int32_t)((wl[2]<<24)|(wl[3]<<16)|(wl[4]<<8)|wl[5]);
        int32_t min_level = (int32_t)((wl[6]<<24)|(wl[7]<<16)|(wl[8]<<8)|wl[9]);
        int32_t max_level = (int32_t)((wl[10]<<24)|(wl[11]<<16)|(wl[12]<<8)|wl[13]);
        int16_t spd       = (int16_t)((wl[14] << 8) | wl[15]);
        int16_t dir       = (int16_t)((wl[16] << 8) | wl[17]);

        cur_level += dir * spd * state->temp_frames;
        if (cur_level >= max_level) { cur_level = max_level; dir = -1; }
        else if (cur_level <= min_level) { cur_level = min_level; dir = 1; }

        wl[2] = (uint8_t)(cur_level >> 24);
        wl[3] = (uint8_t)(cur_level >> 16);
        wl[4] = (uint8_t)(cur_level >> 8);
        wl[5] = (uint8_t)(cur_level);
        wl[16] = (uint8_t)(dir >> 8);
        wl[17] = (uint8_t)(dir);

        /* Update zone water: write current level directly (same as Amiga). */
        if (zone_id >= 0 && zone_id < state->level.num_zones)
            level_set_zone_water(&state->level, zone_id, cur_level);

        wl += 18;
    }
}

#define BRIGHT_ANIM_SENTINEL 999
/* Advance anim tables every logic tick (50 Hz), matching Amiga vblank. */
#define BRIGHT_ANIM_TICK_DIVIDER 1

/* Advance one brightness animation; returns current value, updates index. */
static int16_t bright_anim_advance(const int16_t *table, unsigned int *idx)
{
    int16_t val = table[*idx];
    if (val == BRIGHT_ANIM_SENTINEL) {
        *idx = 0;
        val = table[0];
        (*idx)++;
    } else {
        (*idx)++;
    }
    return val;
}

/* -----------------------------------------------------------------------
 * Brightness animation handler
 *
 * Advances the three global anims (pulse, flicker, fire) each tick.
 * Zone brightness is read from level data at render time via level_get_zone_brightness();
 * zones with brightness word high/low byte 1/2/3 use these values. No per-zone list.
 * ----------------------------------------------------------------------- */
void bright_anim_handler(GameState *state)
{
    LevelState *lev = &state->level;
    static unsigned int bright_anim_tick_count = 0;

    bright_anim_tick_count++;
    if (bright_anim_tick_count % BRIGHT_ANIM_TICK_DIVIDER == 0) {
        lev->bright_anim_values[0] = bright_anim_advance(pulse_anim, &lev->bright_anim_indices[0]);
        lev->bright_anim_values[1] = bright_anim_advance(flicker_anim, &lev->bright_anim_indices[1]);
        lev->bright_anim_values[2] = bright_anim_advance(fire_flicker_anim, &lev->bright_anim_indices[2]);
    }
}

/* -----------------------------------------------------------------------
 * Utility: fire a projectile from an enemy
 *
 * Translated from the common FireAtPlayer pattern in enemy .s files.
 * Creates a bullet in NastyShotData.
 * ----------------------------------------------------------------------- */
void enemy_fire_at_player(GameObject *obj, GameState *state,
                          int player_num, int shot_type, int shot_power,
                          int shot_speed, int shot_shift)
{
    if (!state->level.nasty_shot_data) return;

    PlayerState *plr = (player_num == 1) ? &state->plr1 : &state->plr2;

    /* Find free slot in NastyShotData (up to 20 slots, 64 bytes each) */
    uint8_t *shots = state->level.nasty_shot_data;
    GameObject *bullet = NULL;
    for (int i = 0; i < 20; i++) {
        GameObject *candidate = (GameObject*)(shots + i * OBJECT_SIZE);
        if (OBJ_ZONE(candidate) < 0) {
            bullet = candidate;
            break;
        }
    }
    if (!bullet) return;

    /* Calculate direction to player (AlienControl.s FireAtPlayer1 lines 360-411) */
    int idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
    int16_t obj_x, obj_z;
    get_object_pos(&state->level, idx, &obj_x, &obj_z);

    int32_t plr_x = plr->p_xoff;
    int32_t plr_z = plr->p_zoff;

    /* Lead prediction: offset target by player velocity * (dist/speed) */
    int32_t dx = plr_x - obj_x;
    int32_t dz = plr_z - obj_z;
    int32_t dist = calc_dist_approx(dx, dz);
    if (dist == 0) dist = 1;

    /* Apply lead if speed > 0 */
    if (shot_speed > 0 && state->xdiff1 != 0) {
        int16_t lead_x = (int16_t)((state->xdiff1 * dist) / (shot_speed * 16));
        int16_t lead_z = (int16_t)((state->zdiff1 * dist) / (shot_speed * 16));
        plr_x += lead_x;
        plr_z += lead_z;
    }

    /* Set up bullet */
    memset(bullet, 0, OBJECT_SIZE);
    OBJ_SET_ZONE(bullet, OBJ_ZONE(obj));
    bullet->obj.number = OBJ_NBR_BULLET;

    /* Use HeadTowards to calculate velocity toward (potentially led) target */
    MoveContext hctx;
    move_context_init(&hctx);
    hctx.oldx = obj_x;
    hctx.oldz = obj_z;
    head_towards(&hctx, (int32_t)plr_x, (int32_t)plr_z, (int16_t)shot_speed);

    int16_t xvel = (int16_t)(hctx.newx - obj_x);
    int16_t zvel = (int16_t)(hctx.newz - obj_z);

    /* Copy bullet position to ObjectPoints */
    int bul_idx = (int)OBJ_CID(bullet);
    if (state->level.object_points && bul_idx >= 0) {
        uint8_t *pts = state->level.object_points + bul_idx * 8;
        obj_sw(pts, (int16_t)hctx.newx);
        obj_sw(pts + 4, (int16_t)hctx.newz);
    }

    SHOT_SET_XVEL(*bullet, xvel);
    SHOT_SET_ZVEL(*bullet, zvel);
    SHOT_POWER(*bullet) = (int8_t)shot_power;
    SHOT_SIZE(*bullet) = (int8_t)shot_type;
    SHOT_SET_LIFE(*bullet, 0);

    /* EnemyFlags = both players (bits 5 and 11) */
    NASTY_SET_EFLAGS(*bullet, 0x00100020);

    /* Y position and vertical aim (AlienControl.s lines 415-439) */
    int16_t obj_y = (int16_t)((obj->raw[4] << 8) | obj->raw[5]);
    int32_t acc_y = ((int32_t)obj_y << 7);
    SHOT_SET_ACCYPOS(*bullet, acc_y);
    bullet->raw[4] = obj->raw[4];
    bullet->raw[5] = obj->raw[5];
    bullet->obj.in_top = obj->obj.in_top;

    /* Vertical aim toward player */
    uint8_t *plr_obj_raw = (player_num == 1) ? state->level.plr1_obj : state->level.plr2_obj;
    if (plr_obj_raw) {
        int16_t plr_y = (int16_t)((plr_obj_raw[4] << 8) | plr_obj_raw[5]);
        int32_t y_diff = ((int32_t)(plr_y - 20) << 7) - acc_y;
        y_diff += y_diff; /* *2 */
        int32_t dist_shifted = dist;
        if (shot_shift > 0) dist_shifted >>= shot_shift;
        if (dist_shifted < 1) dist_shifted = 1;
        SHOT_SET_YVEL(*bullet, (int16_t)(y_diff / dist_shifted));
    }

    /* Gravity and flags from gun data (for shot_type) */
    if (shot_type >= 0 && shot_type < 8) {
        SHOT_SET_GRAV(*bullet, default_plr1_guns[shot_type].shot_gravity);
        SHOT_SET_FLAGS(*bullet, default_plr1_guns[shot_type].shot_flags);
    }

    bullet->obj.worry = 127;

    /* Play firing sound */
    audio_play_sample(3, 100);
}

/* -----------------------------------------------------------------------
 * Utility: compute blast damage
 *
 * Translated from the ExplodeIntoBits/ComputeBlast patterns.
 * ----------------------------------------------------------------------- */
void compute_blast(GameState *state, int32_t x, int32_t z, int32_t y,
                   int16_t radius, int16_t power)
{
    if (!state->level.object_data) return;

    int obj_index = 0;
    while (1) {
        GameObject *obj = get_object(&state->level, obj_index);
        if (!obj || OBJ_CID(obj) < 0) break;
        if (OBJ_ZONE(obj) < 0) {
            obj_index++;
            continue;
        }

        int16_t ox, oz;
        get_object_pos(&state->level, (int)OBJ_CID(obj), &ox, &oz);

        int32_t dx = x - ox;
        int32_t dz = z - oz;
        int32_t dist = calc_dist_approx(dx, dz);

        if (dist < radius) {
            /* Apply damage, scaled by distance */
            int damage = (power * (radius - (int)dist)) / radius;
            if (damage > 0) {
                NASTY_SET_DAMAGE(obj, (int8_t)(NASTY_DAMAGE(*obj) + damage));
            }
        }

        obj_index++;
    }

    /* Also damage players */
    {
        int32_t dx = x - state->plr1.p_xoff;
        int32_t dz = z - state->plr1.p_zoff;
        int32_t dist = calc_dist_approx(dx, dz);
        if (dist < radius) {
            int damage = (power * (radius - (int)dist)) / radius;
            state->plr1.energy -= (int16_t)damage;
        }
    }
    if (state->mode != MODE_SINGLE) {
        int32_t dx = x - state->plr2.p_xoff;
        int32_t dz = z - state->plr2.p_zoff;
        int32_t dist = calc_dist_approx(dx, dz);
        if (dist < radius) {
            int damage = (power * (radius - (int)dist)) / radius;
            state->plr2.energy -= (int16_t)damage;
        }
    }

    (void)y; /* Y currently not used for blast radius */
}

/* -----------------------------------------------------------------------
 * Explosion animation (visual only; damage is compute_blast).
 * Amiga: explosion/bullet pop advances one step per ObjMoveAnim (per vblank).
 * Advance by 1 per call so duration is consistent regardless of temp_frames.
 * ----------------------------------------------------------------------- */
void explosion_spawn(GameState *state, int16_t x, int16_t z, int16_t zone, int32_t y_floor,
                    int8_t size_scale, int8_t anim_rate)
{
    if (state->num_explosions >= MAX_EXPLOSIONS) return;
    if (size_scale <= 0) size_scale = 100;
    if (anim_rate <= 0) anim_rate = 100;
    int i = state->num_explosions++;
    state->explosions[i].x = x;
    state->explosions[i].z = z;
    state->explosions[i].zone = zone;
    state->explosions[i].y_floor = y_floor;
    state->explosions[i].frame = 0;
    state->explosions[i].frame_frac = 0;
    state->explosions[i].size_scale = size_scale;
    state->explosions[i].anim_rate = anim_rate;
    /* 0..3 tick delay so particles don't all animate in lockstep */
    state->explosions[i].start_delay = (int8_t)(rand() & 3);
}

/* Amiga: explosion advances one step per ObjMoveAnim (per vblank). We advance by temp_frames
 * so when we simulate multiple vblanks in one tick, explosion timing matches.
 * anim_rate 100 = normal; 75 = 25% slower (barrel). start_delay gives per-particle variation. */
void explosion_advance(GameState *state)
{
    int n = state->num_explosions;
    int tf = state->temp_frames;
    if (tf <= 0) tf = 1;
    for (int i = 0; i < n; i++) {
        if (state->explosions[i].start_delay > 0) {
            state->explosions[i].start_delay = (int8_t)(state->explosions[i].start_delay - tf);
            if (state->explosions[i].start_delay < 0) state->explosions[i].start_delay = 0;
        } else {
            int rate = (int)state->explosions[i].anim_rate;
            if (rate <= 0) rate = 100;
            rate = (rate * 50) / 100;  /* slow down explosion animations by half */
            int frac = (int)state->explosions[i].frame_frac + tf * rate;
            while (frac >= 100) {
                state->explosions[i].frame = (int8_t)(state->explosions[i].frame + 1);
                frac -= 100;
            }
            state->explosions[i].frame_frac = (int8_t)frac;
        }
        if ((int)state->explosions[i].frame >= 9) {
            /* Remove: shift down */
            n--;
            for (int j = i; j < n; j++)
                state->explosions[j] = state->explosions[j + 1];
            state->num_explosions = n;
            i--;
        }
    }
}

/* -----------------------------------------------------------------------
 * Utility: pickup distance check
 *
 * Returns 1 if player is close enough to pick up the object.
 * ----------------------------------------------------------------------- */
int pickup_distance_check(GameObject *obj, GameState *state, int player_num)
{
    PlayerState *plr = (player_num == 1) ? &state->plr1 : &state->plr2;

    int idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
    int16_t ox, oz;
    get_object_pos(&state->level, idx, &ox, &oz);

    int32_t dx = plr->p_xoff - ox;
    int32_t dz = plr->p_zoff - oz;
    int32_t dist_sq = dx * dx + dz * dz;

    return dist_sq < PICKUP_DISTANCE_SQ;
}

/* -----------------------------------------------------------------------
 * USEPLR1 / USEPLR2 - Update player object data for rendering
 *
 * Translated from AB3DI.s USEPLR1 (line ~2302-2537).
 * Copies player state into the player's GameObject in ObjectData.
 * ----------------------------------------------------------------------- */
void use_player1(GameState *state)
{
    if (!state->level.plr1_obj) return;

    GameObject *plr_obj = (GameObject*)state->level.plr1_obj;

    /* Update position in ObjectPoints */
    int idx = 0;
    if (state->level.object_points) {
        idx = (int)(state->level.plr1_obj - state->level.object_data) / OBJECT_SIZE;
        uint8_t *pts = state->level.object_points + idx * 8;
        obj_sw(pts, (int16_t)(state->plr1.xoff >> 16));
        obj_sw(pts + 4, (int16_t)(state->plr1.zoff >> 16));
    }

    /* Update zone */
    OBJ_SET_ZONE(plr_obj, state->plr1.zone);

    /* Update objInTop */
    plr_obj->obj.in_top = state->plr1.stood_in_top;

    /* Damage flash + pain sound (AB3DI.s lines 2323-2348) */
    int8_t damage = NASTY_DAMAGE(*plr_obj);
    if (damage > 0) {
        state->plr1.energy -= damage;
        NASTY_DAMAGE(*plr_obj) = 0;
        state->hitcol = 0xF00; /* Red flash */
        audio_play_sample(19, 200); /* Pain sound */
    }

    /* Update numlives from energy */
    NASTY_LIVES(*plr_obj) = (int8_t)(state->plr1.energy + 1);

    /* Zone brightness (AB3DI.s lines 2358-2366) */
    if (state->plr1.zone >= 0) {
        int16_t zb = level_get_zone_brightness(&state->level, state->plr1.zone,
                                                state->plr1.stood_in_top ? 1 : 0);
        plr_obj->raw[2] = (uint8_t)(zb >> 8);
        plr_obj->raw[3] = (uint8_t)(zb);
    }

    /* Y position: (yoff + height/2) >> 7 (AB3DI.s line 2368) */
    int32_t plr_y = (state->plr1.p_yoff + state->plr1.s_height / 2) >> 7;
    plr_obj->raw[4] = (uint8_t)(plr_y >> 8);
    plr_obj->raw[5] = (uint8_t)(plr_y);

    /* ViewpointToDraw for PLR2 looking at PLR1 (AB3DI.s line 2370) */
    /* This sets the animation frame for how PLR1 looks from PLR2's perspective.
     * Rendering-only but the angle calc is game logic. */
    int16_t frame = viewpoint_to_draw(
        (int16_t)(state->plr2.xoff >> 16), (int16_t)(state->plr2.zoff >> 16),
        (int16_t)(state->plr1.xoff >> 16), (int16_t)(state->plr1.zoff >> 16),
        (int16_t)(state->plr1.angpos * 2));

    /* Facing (AB3DI.s lines 2407-2416) */
    plr_obj->raw[8] = (uint8_t)(state->plr1.angpos >> 8);
    plr_obj->raw[9] = (uint8_t)(state->plr1.angpos);

    /* Animation frame with head bob + viewpoint (AB3DI.s line 2418) */
    int16_t anim = (int16_t)(frame + state->plr1.bob_frame);
    plr_obj->raw[10] = (uint8_t)(anim >> 8);
    plr_obj->raw[11] = (uint8_t)(anim);

    /* Graphic room = -1 (AB3DI.s line 2420) */
    OBJ_SET_GROOM(plr_obj, -1);
}

void use_player2(GameState *state)
{
    if (!state->level.plr2_obj) return;

    GameObject *plr_obj = (GameObject*)state->level.plr2_obj;

    int idx = 0;
    if (state->level.object_points) {
        idx = (int)(state->level.plr2_obj - state->level.object_data) / OBJECT_SIZE;
        uint8_t *pts = state->level.object_points + idx * 8;
        obj_sw(pts, (int16_t)(state->plr2.xoff >> 16));
        obj_sw(pts + 4, (int16_t)(state->plr2.zoff >> 16));
    }

    OBJ_SET_ZONE(plr_obj, state->plr2.zone);
    plr_obj->obj.in_top = state->plr2.stood_in_top;

    int8_t damage = NASTY_DAMAGE(*plr_obj);
    if (damage > 0) {
        state->plr2.energy -= damage;
        NASTY_DAMAGE(*plr_obj) = 0;
        state->hitcol2 = 0xF00;
        audio_play_sample(19, 200);
    }

    NASTY_LIVES(*plr_obj) = (int8_t)(state->plr2.energy + 1);

    if (state->plr2.zone >= 0) {
        int16_t zb = level_get_zone_brightness(&state->level, state->plr2.zone,
                                                state->plr2.stood_in_top ? 1 : 0);
        plr_obj->raw[2] = (uint8_t)(zb >> 8);
        plr_obj->raw[3] = (uint8_t)(zb);
    }

    int32_t plr_y = (state->plr2.p_yoff + state->plr2.s_height / 2) >> 7;
    plr_obj->raw[4] = (uint8_t)(plr_y >> 8);
    plr_obj->raw[5] = (uint8_t)(plr_y);

    int16_t frame = viewpoint_to_draw(
        (int16_t)(state->plr1.xoff >> 16), (int16_t)(state->plr1.zoff >> 16),
        (int16_t)(state->plr2.xoff >> 16), (int16_t)(state->plr2.zoff >> 16),
        (int16_t)(state->plr2.angpos * 2));

    plr_obj->raw[8] = (uint8_t)(state->plr2.angpos >> 8);
    plr_obj->raw[9] = (uint8_t)(state->plr2.angpos);

    int16_t anim = (int16_t)(frame + state->plr2.bob_frame);
    plr_obj->raw[10] = (uint8_t)(anim >> 8);
    plr_obj->raw[11] = (uint8_t)(anim);

    OBJ_SET_GROOM(plr_obj, -1);
}

/* -----------------------------------------------------------------------
 * CalcPLR1InLine / CalcPLR2InLine
 *
 * Translated from AB3DI.s CalcPLR1InLine (line ~4171-4236).
 *
 * For each object, determine if it's in the player's field of view
 * and calculate its distance. Used by auto-aim and rendering.
 * ----------------------------------------------------------------------- */
void calc_plr1_in_line(GameState *state)
{
    if (!state->level.object_data || !state->level.object_points) return;

    int16_t sin_val = state->plr1.sinval;
    int16_t cos_val = state->plr1.cosval;
    int16_t plr_x = (int16_t)(state->plr1.xoff >> 16);
    int16_t plr_z = (int16_t)(state->plr1.zoff >> 16);
    int16_t plr_y = (int16_t)(state->plr1.yoff >> 7);

    /* Player's room pointer (for can_it_be_seen) */
    const uint8_t *plr_room = NULL;
    if (state->level.data && state->level.zone_adds && state->plr1.zone >= 0 &&
        state->plr1.zone < state->level.num_zones) {
        int32_t zoff = be32(state->level.zone_adds + state->plr1.zone * 4);
        if (zoff >= 0) plr_room = state->level.data + zoff;
    }

    int num_pts = state->level.num_object_points;
    if (num_pts > MAX_OBJECTS) num_pts = MAX_OBJECTS;

    for (int i = 0; i <= num_pts; i++) {
        GameObject *obj = get_object(&state->level, i);
        if (!obj || OBJ_CID(obj) < 0) break;

        plr1_obs_in_line[i] = 0;
        plr1_obj_dists[i] = 0;

        if (OBJ_ZONE(obj) < 0) continue;

        /* Get object position via its CID (point index), NOT slot index i */
        int16_t ox, oz;
        get_object_pos(&state->level, OBJ_CID(obj), &ox, &oz);

        int16_t dx = ox - plr_x;
        int16_t dz = oz - plr_z;

        /* Get collision box width for this object type */
        int obj_type = obj->obj.number;
        int16_t box_width = 40; /* default */
        if (obj_type >= 0 && obj_type <= 20) {
            box_width = col_box_table[obj_type].width;
        }

        /* Cross product (perpendicular distance) */
        int32_t cross = (int32_t)dx * cos_val - (int32_t)dz * sin_val;
        cross *= 2;
        if (cross < 0) cross = -cross;
        int16_t perp = (int16_t)(cross >> 16);

        /* Dot product (forward distance) */
        int32_t dot = (int32_t)dx * sin_val + (int32_t)dz * cos_val;
        dot <<= 2;
        int16_t fwd = (int16_t)(dot >> 16);

        /* In line if: forward > 0 && perpendicular/2 < box_width */
        if (fwd > 0 && (perp >> 1) <= box_width) {
            /* LOS check: only mark in-line when no wall/door is between player and object.
             * Pass floor-level as 0 for both ends so can_it_be_seen never rejects a target
             * just because they are on a different floor level (upper/lower). Walls and doors
             * still block shots correctly via the zone-exit geometry. */
            int los_ok = 1;
            int16_t obj_zone = OBJ_ZONE(obj);
            if (plr_room) {
                const uint8_t *obj_room = NULL;
                if (state->level.data && state->level.zone_adds && obj_zone >= 0 &&
                    obj_zone < state->level.num_zones) {
                    int32_t zoff = be32(state->level.zone_adds + obj_zone * 4);
                    if (zoff >= 0) obj_room = state->level.data + zoff;
                }
                int16_t obj_y = (int16_t)((obj->raw[4] << 8) | obj->raw[5]);
                uint8_t vis = can_it_be_seen(&state->level,
                                             plr_room, obj_room, obj_zone,
                                             plr_x, plr_z, plr_y,
                                             ox, oz, obj_y,
                                             0, 0, 1); /* full_height for hitscan */
                los_ok = (vis != 0);
            }
            if (los_ok)
                plr1_obs_in_line[i] = -1; /* 0xFF = in line */
        }

        plr1_obj_dists[i] = fwd;
    }
}

void calc_plr2_in_line(GameState *state)
{
    if (!state->level.object_data || !state->level.object_points) return;

    int16_t sin_val = state->plr2.sinval;
    int16_t cos_val = state->plr2.cosval;
    int16_t plr_x = (int16_t)(state->plr2.xoff >> 16);
    int16_t plr_z = (int16_t)(state->plr2.zoff >> 16);
    int16_t plr_y = (int16_t)(state->plr2.yoff >> 7);

    const uint8_t *plr_room = NULL;
    if (state->level.data && state->level.zone_adds && state->plr2.zone >= 0 &&
        state->plr2.zone < state->level.num_zones) {
        int32_t zoff = be32(state->level.zone_adds + state->plr2.zone * 4);
        if (zoff >= 0) plr_room = state->level.data + zoff;
    }

    int num_pts = state->level.num_object_points;
    if (num_pts > MAX_OBJECTS) num_pts = MAX_OBJECTS;

    for (int i = 0; i <= num_pts; i++) {
        GameObject *obj = get_object(&state->level, i);
        if (!obj || OBJ_CID(obj) < 0) break;

        plr2_obs_in_line[i] = 0;
        plr2_obj_dists[i] = 0;

        if (OBJ_ZONE(obj) < 0) continue;

        int16_t ox, oz;
        get_object_pos(&state->level, OBJ_CID(obj), &ox, &oz);

        int16_t dx = ox - plr_x;
        int16_t dz = oz - plr_z;

        int obj_type = obj->obj.number;
        int16_t box_width = 40;
        if (obj_type >= 0 && obj_type <= 20) {
            box_width = col_box_table[obj_type].width;
        }

        int32_t cross = (int32_t)dx * cos_val - (int32_t)dz * sin_val;
        cross *= 2;
        if (cross < 0) cross = -cross;
        int16_t perp = (int16_t)(cross >> 16);

        int32_t dot = (int32_t)dx * sin_val + (int32_t)dz * cos_val;
        dot <<= 2;
        int16_t fwd = (int16_t)(dot >> 16);

        if (fwd > 0 && (perp >> 1) <= box_width) {
            int los_ok = 1;
            if (plr_room) {
                int16_t obj_zone = OBJ_ZONE(obj);
                const uint8_t *obj_room = NULL;
                if (state->level.data && state->level.zone_adds && obj_zone >= 0 &&
                    obj_zone < state->level.num_zones) {
                    int32_t zoff = be32(state->level.zone_adds + obj_zone * 4);
                    if (zoff >= 0) obj_room = state->level.data + zoff;
                }
                int16_t obj_y = (int16_t)((obj->raw[4] << 8) | obj->raw[5]);
                uint8_t vis = can_it_be_seen(&state->level,
                                             plr_room, obj_room, obj_zone,
                                             plr_x, plr_z, plr_y,
                                             ox, oz, obj_y,
                                             0, 0, 1); /* full_height for hitscan */
                los_ok = (vis != 0);
            }
            if (los_ok)
                plr2_obs_in_line[i] = -1;
        }

        plr2_obj_dists[i] = fwd;
    }
}
