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
#include "movement.h"
#include "math_tables.h"
#include "stub_audio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
static bool enemy_check_damage(GameObject *obj, const EnemyParams *params)
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

    if (lives <= 0) {
        /* Death */
        if (params->death_sound >= 0) {
            audio_play_sample(params->death_sound, 64);
        }

        /* Check for explosion death */
        if (damage > 1 && params->explode_threshold > 0 &&
            damage >= params->explode_threshold) {
            explode_into_bits(obj, NULL);
        }

        /* Play death animation or remove */
        if (params->death_frames[0] >= 0) {
            /* Set first death animation frame */
            OBJ_SET_DEADH(obj, params->death_frames[0]);
            OBJ_SET_DEADL(obj, 0);
            /* Mark as dying (objNumber stays, but special state) */
            obj->obj.number = OBJ_NBR_DEAD;
        }
        OBJ_SET_ZONE(obj, -1); /* Remove from active objects */
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

    move_object(&ctx, &state->level);

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

    head_towards_angle(&ctx, &facing, target_x, target_z,
                       speed * state->temp_frames, 120);
    NASTY_SET_FACING(*obj, facing);

    move_object(&ctx, &state->level);

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
 * Generic enemy handler - used by all enemy types
 * ----------------------------------------------------------------------- */
static void enemy_generic(GameObject *obj, GameState *state, int param_index)
{
    if (!state->nasty) return;
    if (param_index < 0 || param_index >= num_enemy_types) return;

    const EnemyParams *params = &enemy_params[param_index];

    /* Check damage */
    if (enemy_check_damage(obj, params)) return;

    int8_t lives = NASTY_LIVES(*obj);
    if (lives <= 0) return;

    /* Check visibility */
    int8_t can_see = obj->obj.can_see;

    if (can_see & 0x02) {
        enemy_attack(obj, params, state, 1);
    } else if (can_see & 0x01) {
        enemy_attack(obj, params, state, 2);
    } else {
        enemy_wander(obj, params, state);
    }
}

/* -----------------------------------------------------------------------
 * objects_update - Main per-frame object processing
 *
 * Translated from Anims.s ObjMoveAnim.
 * ----------------------------------------------------------------------- */
void objects_update(GameState *state)
{
    /* 1. Update player zones (from room pointer if available) */
    /* Zone is already maintained by player_full_control -> MoveObject */

    /* 2. Player shooting - called from game_loop */

    /* 3. Level mechanics */
    switch_routine(state);
    door_routine(state);
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

    int obj_index = 0;
    while (1) {
        GameObject *obj = get_object(&state->level, obj_index);
        if (!obj) break;
        if (OBJ_CID(obj) < 0) break;
        if (OBJ_ZONE(obj) < 0) {
            obj_index++;
            continue;
        }

        /* Check worry flag */
        if (obj->obj.worry == 0) {
            obj_index++;
            continue;
        }
        obj->obj.worry--;

        /* Update rendering Y position from zone floor height.
         * Anims.s: each handler writes 4(a0) = (ToZoneFloor >> 7) - offset.
         * The offset is per-type; barrel uses -60 (= its world_h).
         * Generic formula: obj[4] = (floor >> 7) - world_h, so that
         *   scr_y + half_h ≈ floor_screen_y  (sprite bottom sits on floor).
         * Proof: (obj[4]<<7 + world_h*128) = floor, matching the floor projection. */
        {
            int16_t obj_zone = OBJ_ZONE(obj);
            if (obj_zone >= 0 && state->level.zone_adds && state->level.data) {
                int32_t zo = be32(state->level.zone_adds + obj_zone * 4);
                if (zo > 0) {
                    const uint8_t *zd = state->level.data + zo;
                    int32_t floor_h = be32(zd + 2);  /* ToZoneFloor */
                    int world_h = (int)(uint8_t)obj->raw[7];
                    if (world_h < 1) world_h = 32;
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
        default:
            /* Handle dying enemies with death animation (negative zone = dying) */
            param_idx = obj_type_to_enemy_index(obj_type);
            if (param_idx >= 0 && param_idx < num_enemy_types) {
                const EnemyParams *ep = &enemy_params[param_idx];
                /* Advance death animation frame */
                int16_t dead_h = OBJ_DEADH(obj);
                int16_t dead_l = OBJ_DEADL(obj);
                dead_l += state->temp_frames;
                if (dead_l >= 4) { /* advance every 4 frames */
                    dead_l = 0;
                    dead_h++;
                    if (dead_h < 30 && ep->death_frames[dead_h] >= 0) {
                        OBJ_SET_DEADH(obj, dead_h);
                    } else {
                        /* Animation done - mark object as fully dead */
                        OBJ_SET_ZONE(obj, -1);
                    }
                }
                OBJ_SET_DEADL(obj, dead_l);
            }
            break;
        }

        obj_index++;
    }

    /* 5. Brightness animations */
    bright_anim_handler(state);
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
    if (enemy_check_damage(obj, params)) return;

    int8_t lives = NASTY_LIVES(*obj);
    if (lives <= 0) return;

    /* Run AI control (from AI.s ItsAMarine) */
    AIParams ai;
    ai.aggression = 15;
    ai.movement = 0;
    ai.cooperation = 30;
    ai.ident = 0;
    ai.enemies = 0x01;           /* players are enemies (%1) */
    ai.friends = 0xA0;           /* other marines are friends (%10100000) */
    ai.armed = true;

    /* Shot parameters per marine type (from AlienControl.s SHOT* globals) */
    if (type == OBJ_NBR_TOUGH_MARINE) {
        ai.shot_type = 6; ai.shot_power = 7; ai.shot_speed = 32; ai.shot_shift = 2;
    } else if (type == OBJ_NBR_FLAME_MARINE) {
        ai.shot_type = 0; ai.shot_power = 2; ai.shot_speed = 16; ai.shot_shift = 3;
    } else { /* Mutant marine */
        ai.shot_type = 0; ai.shot_power = 4; ai.shot_speed = 16; ai.shot_shift = 3;
    }

    ai_control(obj, state, &ai);

    /* If AI didn't move (no level data), fall back to generic behavior */
    int8_t can_see = obj->obj.can_see;
    if (can_see & 0x02) {
        enemy_attack(obj, params, state, 1);
    } else if (can_see & 0x01) {
        enemy_attack(obj, params, state, 2);
    } else if (!state->level.object_data) {
        enemy_wander(obj, params, state);
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

    if (enemy_check_damage(obj, params)) return;

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

    /* Check visibility and attack */
    int8_t can_see = obj->obj.can_see;
    if (can_see & 0x02) {
        enemy_attack(obj, params, state, 1);
    } else if (can_see & 0x01) {
        enemy_attack(obj, params, state, 2);
    } else {
        enemy_wander(obj, params, state);
    }
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
    lives -= damage;

    if (lives <= 0) {
        /* Explode! */
        OBJ_SET_ZONE(obj, -1);

        /* Get position for blast */
        int idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
        int16_t bx, bz;
        get_object_pos(&state->level, idx, &bx, &bz);

        /* Area damage (radius 40, from Anims.s ItsABarrel) */
        compute_blast(state, bx, bz, 0, 40, 8);

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
    if (pickup_distance_check(obj, state, 1)) {
        PlayerState *plr = &state->plr1;
        int gun_idx = plr->gun_selected;
        if (gun_idx >= 0 && gun_idx < MAX_GUNS) {
            int16_t ammo = plr->gun_data[gun_idx].ammo;
            ammo += AMMO_PER_CLIP * 8;
            if (ammo > MAX_AMMO_DISPLAY) ammo = (int16_t)MAX_AMMO_DISPLAY;
            plr->gun_data[gun_idx].ammo = ammo;
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
        /* Determine which key (stored in type_data) */
        int key_id = obj->obj.type_data[0] & 0x03; /* 0-3 */
        game_conditions |= (1 << (5 + key_id));
        OBJ_SET_ZONE(obj, -1);
        audio_play_sample(4, 50);
    }
    if (state->mode != MODE_SINGLE && pickup_distance_check(obj, state, 2)) {
        int key_id = obj->obj.type_data[0] & 0x03;
        game_conditions |= (1 << (5 + key_id));
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

    /* Calculate new position */
    int idx = (int)(((uint8_t*)obj - state->level.object_data) / OBJECT_SIZE);
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

        /* Roof check: if roof - accypos < 10*128, hit roof */
        int32_t roof = (int32_t)((zd[6+zd_off]<<24)|(zd[7+zd_off]<<16)|
                       (zd[8+zd_off]<<8)|zd[9+zd_off]);
        if (roof - accypos < 10 * 128) {
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

        /* Floor check: if floor - accypos > -10*128, hit floor */
        int32_t floor_h = (int32_t)((zd[2+zd_off]<<24)|(zd[3+zd_off]<<16)|
                          (zd[4+zd_off]<<8)|zd[5+zd_off]);
        if (floor_h - accypos > 10 * 128) {
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

        /* Explosive force */
        if (shot_size >= 0 && shot_size < 8 &&
            bullet_types[shot_size].explosive_force > 0) {
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

    /* Update zone from room */
    if (ctx.objroom) {
        OBJ_SET_ZONE(obj, (int16_t)((ctx.objroom[0] << 8) | ctx.objroom[1]));
    }

    /* ---- Object-to-object hit detection (Anims.s lines 3202-3382) ---- */
    uint32_t enemy_flags = NASTY_EFLAGS(*obj);
    if (enemy_flags == 0) return;

    if (!state->level.object_data) return;

    /* Calculate bullet travel direction length (Newton-Raphson sqrt) */
    int32_t bdx = ctx.newx - bx;
    int32_t bdz = ctx.newz - bz;
    int32_t travel_dist = calc_dist_approx(bdx, bdz);
    if (travel_dist == 0) travel_dist = 1;
    int32_t check_range_sq = (int32_t)(travel_dist + 40) * (travel_dist + 40);

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

        /* Check lives */
        if (NASTY_LIVES(*target) <= 0) {
            check_idx++;
            continue;
        }

        /* Height check using collision box */
        const CollisionBox *box = &col_box_table[tgt_type];
        int16_t obj_y = (int16_t)(accypos >> 7);
        int16_t tgt_y = target->raw[4] << 8 | target->raw[5]; /* offset 4 = Y */
        int16_t ydiff = obj_y - tgt_y;
        if (ydiff < 0) ydiff = (int16_t)(-ydiff);
        if (ydiff > box->half_height) {
            check_idx++;
            continue;
        }

        /* Position check */
        int16_t tx, tz;
        get_object_pos(&state->level, check_idx, &tx, &tz);

        /* Perpendicular distance from target to bullet path */
        int32_t dx_old = tx - bx;
        int32_t dz_old = tz - bz;
        int32_t cross = dx_old * bdz - dz_old * bdx;
        if (cross < 0) cross = -cross;
        int32_t perp = cross / travel_dist;

        if (perp > box->width) {
            check_idx++;
            continue;
        }

        /* Distance check (both old and new position must be close) */
        int32_t dist_old_sq = dx_old * dx_old + dz_old * dz_old;
        int32_t dx_new = tx - (int32_t)ctx.newx;
        int32_t dz_new = tz - (int32_t)ctx.newz;
        int32_t dist_new_sq = dx_new * dx_new + dz_new * dz_new;

        if (dist_old_sq > check_range_sq && dist_new_sq > check_range_sq) {
            check_idx++;
            continue;
        }

        /* HIT! Apply damage */
        NASTY_DAMAGE(*target) += SHOT_POWER(*obj);

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
            compute_blast(state, ctx.newx, ctx.newz, accypos,
                          bullet_types[shot_size].explosive_force,
                          SHOT_POWER(*obj));
        }

        OBJ_SET_ZONE(obj, -1);
        return;
    }
}

/* -----------------------------------------------------------------------
 * Door routine
 *
 * Translated from Anims.s DoorRoutine (line ~642-796).
 *
 * Door data format (per door, 16 bytes in DoorData array):
 *   0: zone index (word)
 *   2: door type (word) - 0=player+space, 1=condition, 2=condition2, etc.
 *   4: door position (long) - current height offset
 *   8: door velocity (word) - current speed
 *  10: door max (word) - maximum opening height
 *  12: timer (word) - close delay
 *  14: flags (word)
 * ----------------------------------------------------------------------- */
void door_routine(GameState *state)
{
    if (!state->level.door_data) return;

    uint8_t *door = state->level.door_data;

    /* Iterate door entries (16 bytes each, terminated by -1) */
    while (1) {
        int16_t zone_id = be16(door);
        if (zone_id < 0) break;

        int16_t door_type = be16(door + 2);
        int32_t door_pos = be32(door + 4);
        int16_t door_vel = be16(door + 8);
        int16_t door_max = be16(door + 10);
        int16_t timer = be16(door + 12);

        bool should_open = false;

        switch (door_type) {
        case 0: /* Opens on player space key */
            if (state->plr1.p_spctap && state->plr1.zone == zone_id) {
                should_open = true;
            }
            if (state->plr2.p_spctap && state->plr2.zone == zone_id) {
                should_open = true;
            }
            break;
        case 1: /* Opens on condition bit */
            if (game_conditions & 0x900) { /* %100100000000 */
                should_open = true;
            }
            break;
        case 2: /* Opens on condition bit */
            if (game_conditions & 0x400) { /* %10000000000 */
                should_open = true;
            }
            break;
        case 3: /* Opens on condition bit */
            if (game_conditions & 0x200) { /* %1000000000 */
                should_open = true;
            }
            break;
        case 4: /* Always open */
            should_open = true;
            break;
        case 5: /* Never opens */
            break;
        }

        if (should_open && door_vel == 0) {
            door_vel = -16; /* Open speed */
        }

        /* Animate door position */
        if (door_vel != 0) {
            door_pos += (int32_t)door_vel * state->temp_frames * 256;

            if (door_pos <= 0) {
                /* Fully open */
                door_pos = 0;
                door_vel = 0;
                timer = 100; /* Close delay */
            }
            if (door_pos >= (int32_t)door_max * 256) {
                /* Fully closed */
                door_pos = (int32_t)door_max * 256;
                door_vel = 0;
            }
        } else if (!should_open && door_pos < (int32_t)door_max * 256) {
            /* Close timer */
            timer -= state->temp_frames;
            if (timer <= 0 && door_type == 0) {
                door_vel = 4; /* Close speed */
                timer = 0;
            }
        }

        /* Write back (big-endian) */
        wbe32(door + 4, door_pos);
        wbe16(door + 8, door_vel);
        wbe16(door + 12, timer);

        /* Update zone data (door height affects zone roof) */
        if (state->level.zone_adds && state->level.data &&
            zone_id >= 0 && zone_id < state->level.num_zones) {
            const uint8_t *za = state->level.zone_adds;
            int32_t zoff = (int32_t)((za[zone_id*4]<<24)|(za[zone_id*4+1]<<16)|
                           (za[zone_id*4+2]<<8)|za[zone_id*4+3]);
            uint8_t *zd = state->level.data + zoff;
            /* Write door_pos into zone roof height (offset 6, big-endian) */
            zd[6] = (uint8_t)(door_pos >> 24);
            zd[7] = (uint8_t)(door_pos >> 16);
            zd[8] = (uint8_t)(door_pos >> 8);
            zd[9] = (uint8_t)(door_pos);
        }

        door += 16;
    }
}

/* -----------------------------------------------------------------------
 * Lift routine
 *
 * Translated from Anims.s LiftRoutine (line ~377-627).
 * ----------------------------------------------------------------------- */
void lift_routine(GameState *state)
{
    if (!state->level.lift_data) return;

    uint8_t *lift = state->level.lift_data;

    /* Iterate lift entries (terminated by -1) */
    while (1) {
        int16_t zone_id = be16(lift);
        if (zone_id < 0) break;

        int16_t lift_type = be16(lift + 2);
        int32_t lift_pos = be32(lift + 4);
        int16_t lift_vel = be16(lift + 8);
        int32_t lift_top = be32(lift + 10);
        int32_t lift_bot = be32(lift + 14);

        bool should_move = false;

        switch (lift_type) {
        case 0: /* Moves on player space key */
            if (state->plr1.p_spctap && state->plr1.stood_on_lift) {
                should_move = true;
            }
            if (state->plr2.p_spctap && state->plr2.stood_on_lift) {
                should_move = true;
            }
            break;
        case 1: /* Moves if no player on lift */
            if (!state->plr1.stood_on_lift && !state->plr2.stood_on_lift) {
                should_move = true;
            }
            break;
        case 2: /* Always moves */
            should_move = true;
            break;
        case 3: /* Never moves */
            break;
        }

        /* Animate lift */
        if (should_move || lift_vel != 0) {
            if (lift_vel == 0) {
                /* Start moving (toggle direction) */
                if (lift_pos <= lift_top) {
                    lift_vel = 4; /* Down */
                } else {
                    lift_vel = -4; /* Up */
                }
            }

            lift_pos += (int32_t)lift_vel * state->temp_frames * 256;

            /* Clamp */
            if (lift_pos <= lift_top) {
                lift_pos = lift_top;
                lift_vel = 0;
            }
            if (lift_pos >= lift_bot) {
                lift_pos = lift_bot;
                lift_vel = 0;
            }
        }

        wbe32(lift + 4, lift_pos);
        wbe16(lift + 8, lift_vel);

        /* Update zone floor height */
        if (state->level.zone_adds && state->level.data) {
            const uint8_t *za = state->level.zone_adds;
            int32_t zoff = (int32_t)((za[zone_id*4]<<24)|(za[zone_id*4+1]<<16)|
                           (za[zone_id*4+2]<<8)|za[zone_id*4+3]);
            uint8_t *zd = state->level.data + zoff;
            /* Write lift_pos into zone floor height (offset 2, big-endian) */
            zd[2] = (uint8_t)(lift_pos >> 24);
            zd[3] = (uint8_t)(lift_pos >> 16);
            zd[4] = (uint8_t)(lift_pos >> 8);
            zd[5] = (uint8_t)(lift_pos);
        }

        /* Adjust player Y if standing on this lift */
        if (state->plr1.stood_on_lift && state->plr1.zone == zone_id) {
            state->plr1.s_tyoff = lift_pos - state->plr1.s_height;
        }
        if (state->plr2.stood_on_lift && state->plr2.zone == zone_id) {
            state->plr2.s_tyoff = lift_pos - state->plr2.s_height;
        }

        lift += 20;
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

    while (1) {
        int16_t zone_id = be16(sw);
        if (zone_id < 0) break;

        int8_t cooldown = *(int8_t*)(sw + 3);  /* byte 3 (Anims: sub.b d1,3(a0)) */

        /* Decrement cooldown */
        if (cooldown > 0) {
            cooldown -= (int8_t)(state->temp_frames * 4);
            if (cooldown < 0) cooldown = 0;
            *(int8_t*)(sw + 3) = cooldown;
        }

        /* Check if player is near and pressing space.
         * Switch record is 14 bytes (Anims.s adda.w #14): zone(2), bit_mask(2), cooldown(1),
         * pad(1), gfx_offset(4) = offset into level->graphics to the switch wall entry,
         * position(4) = x(2), z(2). */
        if (cooldown == 0) {
            int16_t sw_x = be16(sw + 10); /* switch X position */
            int16_t sw_z = be16(sw + 12); /* switch Z position */

            bool near_plr1 = (state->plr1.zone == zone_id);
            bool near_plr2 = (state->plr2.zone == zone_id);

            if (near_plr1 && sw_x != 0) {
                int32_t dx = state->plr1.p_xoff - sw_x;
                int32_t dz = state->plr1.p_zoff - sw_z;
                near_plr1 = (dx * dx + dz * dz) < 3600;
            }
            if (near_plr2 && sw_x != 0) {
                int32_t dx = state->plr2.p_xoff - sw_x;
                int32_t dz = state->plr2.p_zoff - sw_z;
                near_plr2 = (dx * dx + dz * dz) < 3600;
            }

            if (state->plr1.p_spctap && near_plr1) {
                int16_t bit_mask = be16(sw + 4);
                game_conditions ^= bit_mask;
                *(int8_t*)(sw + 3) = 20; /* cooldown */
                /* Patch switch wall: only flip bit 1 (on/off); preserve point index in first word */
                if (state->level.graphics) {
                    int32_t gfx_off = (int32_t)be32(sw + 6);
                    uint8_t *wall_ptr = state->level.graphics + gfx_off;
                    int16_t w = be16(wall_ptr);
                    w = (int16_t)((w & ~2) | ((game_conditions & bit_mask) ? 2 : 0));
                    write_be16(wall_ptr, w);
                }
                audio_play_sample(10, 50);
            }
            if (state->plr2.p_spctap && near_plr2) {
                int16_t bit_mask = be16(sw + 4);
                game_conditions ^= bit_mask;
                *(int8_t*)(sw + 3) = 20;
                if (state->level.graphics) {
                    int32_t gfx_off = (int32_t)be32(sw + 6);
                    uint8_t *wall_ptr = state->level.graphics + gfx_off;
                    int16_t w = be16(wall_ptr);
                    w = (int16_t)((w & ~2) | ((game_conditions & bit_mask) ? 2 : 0));
                    write_be16(wall_ptr, w);
                }
                audio_play_sample(10, 50);
            }
        }

        sw += 14;
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

        /* Update zone floor to water level */
        if (state->level.zone_adds && state->level.data) {
            const uint8_t *za = state->level.zone_adds;
            int32_t zoff = (int32_t)((za[zone_id*4]<<24)|(za[zone_id*4+1]<<16)|
                           (za[zone_id*4+2]<<8)|za[zone_id*4+3]);
            uint8_t *zd = state->level.data + zoff;
            zd[2] = (uint8_t)(cur_level >> 24);
            zd[3] = (uint8_t)(cur_level >> 16);
            zd[4] = (uint8_t)(cur_level >> 8);
            zd[5] = (uint8_t)(cur_level);
        }

        wl += 18;
    }
}

/* -----------------------------------------------------------------------
 * Brightness animation handler
 *
 * Translated from Anims.s BrightAnimHandler (line ~197-222).
 * ----------------------------------------------------------------------- */
void bright_anim_handler(GameState *state)
{
    /* The brightness animation system modifies zone brightness values
     * based on one of three animation curves: pulse, flicker, fire_flicker.
     *
     * Each animated zone has:
     * - A pointer to an animation table (pulse_anim / flicker_anim / fire_flicker_anim)
     * - A current index into the table
     * - A base brightness value
     *
     * When the table value is 999, loop back to start. */
    if (!state->level.bright_anim_list) return;

    uint8_t *ba = state->level.bright_anim_list;
    while (1) {
        int16_t zone_id = (int16_t)((ba[0] << 8) | ba[1]);
        if (zone_id < 0) break;

        int16_t anim_type = (int16_t)((ba[2] << 8) | ba[3]); /* 0=pulse, 1=flicker, 2=fire */
        int16_t anim_idx  = (int16_t)((ba[4] << 8) | ba[5]);
        int16_t base_bright = (int16_t)((ba[6] << 8) | ba[7]);

        /* Select animation table */
        const int16_t *table = pulse_anim;
        if (anim_type == 1) table = flicker_anim;
        else if (anim_type == 2) table = fire_flicker_anim;

        /* Read current value */
        int16_t val = table[anim_idx];
        if (val == 999) {
            anim_idx = 0;
            val = table[0];
        }

        anim_idx++;
        ba[4] = (uint8_t)(anim_idx >> 8);
        ba[5] = (uint8_t)(anim_idx);

        /* Apply: zone brightness = base_bright + val */
        int16_t final_bright = (int16_t)(base_bright + val);
        if (final_bright < 0) final_bright = 0;
        if (final_bright > 255) final_bright = 255;

        if (state->level.zone_bright_table) {
            state->level.zone_bright_table[zone_id] = (uint8_t)final_bright;
        }

        ba += 8; /* next entry */
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
        get_object_pos(&state->level, obj_index, &ox, &oz);

        int32_t dx = x - ox;
        int32_t dz = z - oz;
        int32_t dist = calc_dist_approx(dx, dz);

        if (dist < radius) {
            /* Apply damage, scaled by distance */
            int damage = (power * (radius - (int)dist)) / radius;
            if (damage > 0) {
                NASTY_DAMAGE(*obj) += (int8_t)damage;
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
    if (state->level.zone_bright_table && state->plr1.zone >= 0) {
        int16_t zb = state->level.zone_bright_table[state->plr1.zone];
        /* If in top, use upper brightness (stored at zone + num_zones) */
        if (state->plr1.stood_in_top && state->level.num_zones > 0) {
            zb = state->level.zone_bright_table[state->plr1.zone +
                                                 state->level.num_zones];
        }
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

    if (state->level.zone_bright_table && state->plr2.zone >= 0) {
        int16_t zb = state->level.zone_bright_table[state->plr2.zone];
        if (state->plr2.stood_in_top && state->level.num_zones > 0) {
            zb = state->level.zone_bright_table[state->plr2.zone +
                                                 state->level.num_zones];
        }
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

    int num_pts = state->level.num_object_points;
    if (num_pts > MAX_OBJECTS) num_pts = MAX_OBJECTS;

    for (int i = 0; i <= num_pts; i++) {
        GameObject *obj = get_object(&state->level, i);
        if (!obj || OBJ_CID(obj) < 0) break;

        plr1_obs_in_line[i] = 0;
        plr1_obj_dists[i] = 0;

        if (OBJ_ZONE(obj) < 0) continue;

        /* Get object position */
        int16_t ox, oz;
        get_object_pos(&state->level, i, &ox, &oz);

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

    int num_pts = state->level.num_object_points;
    if (num_pts > MAX_OBJECTS) num_pts = MAX_OBJECTS;

    for (int i = 0; i <= num_pts; i++) {
        GameObject *obj = get_object(&state->level, i);
        if (!obj || OBJ_CID(obj) < 0) break;

        plr2_obs_in_line[i] = 0;
        plr2_obj_dists[i] = 0;

        if (OBJ_ZONE(obj) < 0) continue;

        int16_t ox, oz;
        get_object_pos(&state->level, i, &ox, &oz);

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
            plr2_obs_in_line[i] = -1;
        }

        plr2_obj_dists[i] = fwd;
    }
}
