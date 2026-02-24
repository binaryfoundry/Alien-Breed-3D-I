/*
 * Alien Breed 3D I - PC Port
 * game_data.h - All game data tables translated from assembly
 *
 * Contains: Gun data, collision boxes, bullet types, animation tables,
 *           enemy parameters, pickup values, explosion data, level text.
 *
 * Translated from: AB3DI.s, ObjectMove.s, Anims.s, PlayerShoot.s,
 *                  NormalAlien.s, Robot.s, and all enemy .s files.
 */

#ifndef GAME_DATA_H
#define GAME_DATA_H

#include "game_types.h"

/* -----------------------------------------------------------------------
 * Gun data structure (32 bytes per gun, 8 guns)
 * Translated from AB3DI.s PLR1_GunData (~line 2696-2822)
 *
 * Layout per gun entry:
 *   0: Ammo left (word)
 *   2: Ammo per shot (byte)
 *   3: Gun sample number (byte)
 *   4: Ammo in clip (byte)
 *   5: PlrFireBullet flag (byte, -1=instant, 0=projectile)
 *   6: Shot power / bullet damage (byte)
 *   7: Got gun / visible flag (byte, 0 or $FF)
 *   8: Time to shoot / delay (word)
 *  10: Lifetime of bullet (word, -1 = infinite)
 *  12: Click or hold down (word, 0=click, 1=hold)
 *  14: Bullet speed shift (word)
 *  16: Shot gravity (word)
 *  18: Shot flags (word)
 *  20: Bullet Y offset (word)
 *  22: Bullet count / hitting count (word)
 *  24-31: padding
 * ----------------------------------------------------------------------- */
typedef struct {
    int16_t  ammo_left;        /* 0 */
    int8_t   ammo_per_shot;    /* 2 */
    int8_t   gun_sample;       /* 3 */
    int8_t   ammo_in_clip;     /* 4 */
    int8_t   fire_bullet;      /* 5: -1 = instant hit, 0 = projectile */
    int8_t   shot_power;       /* 6 */
    int8_t   got_gun;          /* 7: 0 or -1 ($FF) */
    int16_t  fire_delay;       /* 8 */
    int16_t  bullet_lifetime;  /* 10: -1 = infinite */
    int16_t  click_or_hold;    /* 12: 0=click, 1=hold */
    int16_t  bullet_speed;     /* 14 */
    int16_t  shot_gravity;     /* 16 */
    int16_t  shot_flags;       /* 18 */
    int16_t  bullet_y_offset;  /* 20 */
    int16_t  bullet_count;     /* 22 */
    int16_t  _pad[4];          /* 24-31 */
} GunDataEntry;

_Static_assert(sizeof(GunDataEntry) == 32, "GunDataEntry must be 32 bytes");

/* Default gun data for player 1 (8 guns) */
extern const GunDataEntry default_plr1_guns[8];
/* Default gun data for player 2 (same layout) */
extern const GunDataEntry default_plr2_guns[8];

/* Gun names for debugging */
extern const char *gun_names[8];

/* -----------------------------------------------------------------------
 * Gun animation data
 * Translated from AB3DI.s GunAnims (~line 2662-2688)
 * ----------------------------------------------------------------------- */
#define MAX_GUN_ANIM_FRAMES 64

typedef struct {
    int16_t  frames[MAX_GUN_ANIM_FRAMES];
    int      num_frames;
} GunAnim;

extern const GunAnim gun_anims[8];

/* -----------------------------------------------------------------------
 * Collision box table
 * Translated from ObjectMove.s ColBoxTable (~line 1949-1992)
 *
 * 4 words per object type: width, half_height, full_height, reserved
 * width = horizontal collision radius
 * half_height = half vertical collision extent
 * full_height = full vertical collision extent
 * ----------------------------------------------------------------------- */
typedef struct {
    int16_t  width;
    int16_t  half_height;
    int16_t  full_height;
    int16_t  reserved;
} CollisionBox;

extern const CollisionBox col_box_table[21];

/* -----------------------------------------------------------------------
 * Default world size (width, height) per object type for display.
 * Amiga: each type sets move.w #...,6(a0) (word = high byte w, low byte h).
 * Used to set obj[6]/obj[7] when level data has them as 0.
 * ----------------------------------------------------------------------- */
typedef struct { int8_t w; int8_t h; } ObjectWorldSize;
extern const ObjectWorldSize default_object_world_size[21];

/* -----------------------------------------------------------------------
 * Bullet type data
 * Translated from Anims.s BulletTypes / BulletSizes / ExplosiveForce
 * ----------------------------------------------------------------------- */
typedef struct {
    int16_t  size_x;          /* display width */
    int16_t  size_y;          /* display height */
    int16_t  explosive_force; /* blast radius (0 = none) */
    int16_t  hit_noise;       /* sound on impact (-1 = none) */
    int16_t  hit_volume;      /* volume on impact */
} BulletTypeData;

extern const BulletTypeData bullet_types[8];

/* -----------------------------------------------------------------------
 * Enemy type parameters
 * Consolidated from all enemy .s files
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Movement */
    int32_t  thing_height;     /* collision height (in *128 units) */
    int32_t  step_up;          /* max step-up */
    int32_t  step_down;        /* max step-down */
    int16_t  extlen;           /* wall margin */
    int8_t   awayfromwall;     /* push from wall */
    int16_t  nas_height;       /* vertical offset for rendering */

    /* Combat */
    int16_t  melee_damage;     /* damage per melee hit */
    int16_t  melee_cooldown;   /* frames between melee hits */
    int16_t  melee_range;      /* max melee distance */
    int16_t  shot_type;        /* bullet type for ranged attack (-1=none) */
    int16_t  shot_power;       /* ranged damage */
    int16_t  shot_speed;       /* projectile speed */
    int16_t  shot_shift;       /* speed shift */
    int16_t  shot_cooldown;    /* frames between shots */

    /* Health */
    int8_t   damage_shift;     /* right-shift applied to incoming damage */
    int16_t  explode_threshold; /* damage for explosion death */

    /* AI */
    int16_t  wander_timer;     /* frames between direction changes */
    int16_t  hiss_timer_min;   /* min frames between idle sounds */
    int16_t  hiss_timer_range; /* random range added to min */

    /* Sounds */
    int16_t  death_sound;      /* death SFX id */
    int16_t  scream_sound;     /* hurt SFX id */
    int16_t  hiss_sound;       /* idle SFX id */
    int16_t  attack_sound;     /* attack SFX id */

    /* Death animation */
    int16_t  death_frames[30]; /* sequence of animation frames, -1 terminated */
} EnemyParams;

extern const EnemyParams enemy_params[];
extern const int num_enemy_types;

/* -----------------------------------------------------------------------
 * Pickup constants
 * Translated from Anims.s
 * ----------------------------------------------------------------------- */
#define HEAL_FACTOR         18      /* health restored by medikit */
#define PICKUP_DISTANCE_SQ  10000   /* 100*100 squared distance for pickup */
#define AMMO_PER_CLIP       8       /* rounds per ammo pickup (multiplied by 8) */
#define MAX_AMMO_DISPLAY    (999*8) /* 7992 max ammo (internal) */
#define MAX_AMMO_RAW        999     /* max display ammo */

/* Ammo type per gun (which ammo graphic to use) */
extern const int8_t ammo_graphic_table[8];
/* Ammo given by gun pickup */
extern const int8_t ammo_in_guns[8];

/* -----------------------------------------------------------------------
 * Brightness animation data
 * Translated from Anims.s BrightAnimHandler
 * ----------------------------------------------------------------------- */
extern const int16_t pulse_anim[];
extern const int16_t flicker_anim[];
extern const int16_t fire_flicker_anim[];

/* -----------------------------------------------------------------------
 * Level end zones
 * Translated from AB3DI.s ENDZONES
 * ----------------------------------------------------------------------- */
extern const int16_t end_zones[16];

/* -----------------------------------------------------------------------
 * Level text / blurb
 * Translated from LevelBlurb.s
 * ----------------------------------------------------------------------- */
extern const char *level_text[16];
extern const char *end_game_text;

/* Conditions bitmask for switches/doors */
extern int16_t game_conditions;

#endif /* GAME_DATA_H */
