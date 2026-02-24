/*
 * Alien Breed 3D I - PC Port
 * game_data.c - All game data tables translated from assembly
 *
 * Translated from: AB3DI.s, ObjectMove.s, Anims.s, PlayerShoot.s,
 *                  NormalAlien.s, Robot.s, BigRedThing.s, HalfWorm.s,
 *                  FlameMarine.s, ToughMarine.s, MutantMarine.s,
 *                  BigUglyAlien.s, BigClaws.s, FlyingScalyBall.s, Tree.s
 */

#include "game_data.h"
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Gun data for Player 1
 * Translated from AB3DI.s PLR1_GunData (line ~2696-2822)
 *
 * Gun 0: Pistol (instant hit, 1 pellet)
 * Gun 1: Plasma Gun (projectile)
 * Gun 2: Rocket Launcher (projectile, explosive)
 * Gun 3: Flamethrower (projectile, rapid fire)
 * Gun 4: Grenade Launcher (projectile, arc, explosive)
 * Gun 5: (unused - worm gun)
 * Gun 6: (unused - tough marine gun)
 * Gun 7: Shotgun (instant hit, 7 pellets)
 * ----------------------------------------------------------------------- */
const GunDataEntry default_plr1_guns[8] = {
    /* Gun 0: Pistol */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 8,
        .gun_sample     = 3,
        .ammo_in_clip   = 15,
        .fire_bullet    = -1,     /* instant hit */
        .shot_power     = 4,
        .got_gun        = -1,     /* 0xFF = has gun */
        .fire_delay     = 5,
        .bullet_lifetime = -1,
        .click_or_hold  = 1,
        .bullet_speed   = 0,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 1: Plasma Gun */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 8,
        .gun_sample     = 1,
        .ammo_in_clip   = 20,
        .fire_bullet    = 0,      /* projectile */
        .shot_power     = 16,
        .got_gun        = 0,
        .fire_delay     = 10,
        .bullet_lifetime = -1,
        .click_or_hold  = 0,
        .bullet_speed   = 5,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 2: Rocket Launcher */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 8,
        .gun_sample     = 9,
        .ammo_in_clip   = 2,
        .fire_bullet    = 0,
        .shot_power     = 12,
        .got_gun        = 0,
        .fire_delay     = 30,
        .bullet_lifetime = -1,
        .click_or_hold  = 0,
        .bullet_speed   = 5,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 3: Flamethrower */
    {
        .ammo_left      = 90 * 8,
        .ammo_per_shot  = 1,
        .gun_sample     = 22,
        .ammo_in_clip   = 40,
        .fire_bullet    = 0,
        .shot_power     = 8,
        .got_gun        = 0,
        .fire_delay     = 5,
        .bullet_lifetime = 50,
        .click_or_hold  = 1,
        .bullet_speed   = 4,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 4: Grenade Launcher */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 8,
        .gun_sample     = 9,
        .ammo_in_clip   = 6,
        .fire_bullet    = 0,
        .shot_power     = 8,
        .got_gun        = 0,
        .fire_delay     = 50,
        .bullet_lifetime = 100,
        .click_or_hold  = 1,
        .bullet_speed   = 5,
        .shot_gravity   = 60,
        .shot_flags     = 3,
        .bullet_y_offset = -1000,
        .bullet_count   = 1,
    },
    /* Gun 5: Worm Gun (unused) */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 0,
        .gun_sample     = 0,
        .ammo_in_clip   = 0,
        .fire_bullet    = 0,
        .shot_power     = 0,
        .got_gun        = 0,
        .fire_delay     = 0,
        .bullet_lifetime = -1,
        .click_or_hold  = 0,
        .bullet_speed   = 5,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 6: Tough Marine Gun (unused) */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 0,
        .gun_sample     = 0,
        .ammo_in_clip   = 0,
        .fire_bullet    = 0,
        .shot_power     = 0,
        .got_gun        = 0,
        .fire_delay     = 0,
        .bullet_lifetime = -1,
        .click_or_hold  = 0,
        .bullet_speed   = 5,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 7: Shotgun */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 8,
        .gun_sample     = 21,
        .ammo_in_clip   = 15,
        .fire_bullet    = -1,     /* instant hit */
        .shot_power     = 4,
        .got_gun        = 0,
        .fire_delay     = 50,
        .bullet_lifetime = -1,
        .click_or_hold  = 1,
        .bullet_speed   = 0,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 7,
    },
};

/* Player 2 gun data is identical to Player 1 */
const GunDataEntry default_plr2_guns[8] = {
    /* Gun 0: Pistol */
    {
        .ammo_left      = 0,
        .ammo_per_shot  = 8,
        .gun_sample     = 3,
        .ammo_in_clip   = 15,
        .fire_bullet    = -1,
        .shot_power     = 4,
        .got_gun        = -1,
        .fire_delay     = 5,
        .bullet_lifetime = -1,
        .click_or_hold  = 1,
        .bullet_speed   = 0,
        .shot_gravity   = 0,
        .shot_flags     = 0,
        .bullet_y_offset = 0,
        .bullet_count   = 1,
    },
    /* Gun 1-7 same as P1 */
    { 0, 8, 1, 20, 0, 16, 0, 10, -1, 0, 5, 0, 0, 0, 1, {0} },
    { 0, 8, 9, 2,  0, 12, 0, 30, -1, 0, 5, 0, 0, 0, 1, {0} },
    { 90*8, 1, 22, 40, 0, 8, 0, 5, 50, 1, 4, 0, 0, 0, 1, {0} },
    { 0, 8, 9, 6, 0, 8, 0, 50, 100, 1, 5, 60, 3, -1000, 1, {0} },
    { 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 5, 0, 0, 0, 1, {0} },
    { 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 5, 0, 0, 0, 1, {0} },
    { 0, 8, 21, 15, -1, 4, 0, 50, -1, 1, 0, 0, 0, 0, 7, {0} },
};

const char *gun_names[8] = {
    "Pistol",
    "Plasma Gun",
    "Rocket Launcher",
    "Flamethrower",
    "Grenade Launcher",
    "(Worm Gun)",
    "(Marine Gun)",
    "Shotgun",
};

/* -----------------------------------------------------------------------
 * Gun animation frames
 * Translated from AB3DI.s MachineAnim, PlasmaAnim, etc. (line ~2674-2688)
 * ----------------------------------------------------------------------- */
const GunAnim gun_anims[8] = {
    /* Gun 0: Pistol - MachineAnim */
    { .frames = {0, 1, 2, 3}, .num_frames = 3 },
    /* Gun 1: Plasma - PlasmaAnim */
    { .frames = {0, 1, 2, 3, 3, 3}, .num_frames = 5 },
    /* Gun 2: Rocket - RocketAnim */
    { .frames = {0, 1, 2, 3, 3, 3}, .num_frames = 5 },
    /* Gun 3: Flamethrower - FlameThrowerAnim */
    { .frames = {0, 1, 2, 3, 3, 3}, .num_frames = 5 },
    /* Gun 4: Grenade - GrenadeAnim */
    { .frames = {0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3}, .num_frames = 12 },
    /* Gun 5: unused */
    { .frames = {0}, .num_frames = 0 },
    /* Gun 6: unused */
    { .frames = {0}, .num_frames = 0 },
    /* Gun 7: Shotgun - ShotGunAnim (0, 12x2, 19x1, 11x2, 20x0, 3) */
    { .frames = {
        0,
        2,2,2,2,2,2,2,2,2,2,2,2,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        2,2,2,2,2,2,2,2,2,2,2,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        3
    }, .num_frames = 63 },
};

/* -----------------------------------------------------------------------
 * Collision box table
 * Translated from ObjectMove.s ColBoxTable (line ~1949-1992)
 *
 * 4 words: width, half_height, full_height, 0
 * ----------------------------------------------------------------------- */
const CollisionBox col_box_table[21] = {
    /*  0: red scurrying alien  */ { 40,  60, 120, 0 },
    /*  1: Medipack              */ { 40,  20,  40, 0 },
    /*  2: Bullet                */ { 40,  20,  40, 0 },
    /*  3: Gun                   */ { 40,  20,  40, 0 },
    /*  4: Key                   */ { 40,  20,  40, 0 },
    /*  5: Player1               */ { 40,  40,  80, 0 },
    /*  6: Robot                 */ { 40,  50, 100, 0 },
    /*  7: ?                     */ { 40,  20,  40, 0 },
    /*  8: Flying Nasty          */ { 80,  60, 120, 0 },
    /*  9: Ammo                  */ { 40,  20,  40, 0 },
    /* 10: Barrel                */ { 40,  30,  60, 0 },
    /* 11: Player2               */ { 40,  40,  80, 0 },
    /* 12: Mutant Marine          */ { 40,  40,  80, 0 },
    /* 13: Worm                  */ { 80,  60, 120, 0 },
    /* 14: Huge red thing        */ {160, 100, 200, 0 },
    /* 15: Small red thing       */ { 80,  50, 100, 0 },
    /* 16: Tree                  */ { 80,  60, 120, 0 },
    /* 17: Eye ball              */ { 40,  30,  60, 0 },
    /* 18: Tough Marine           */ { 40,  40,  80, 0 },
    /* 19: Shot Gun Marine        */ { 40,  40,  80, 0 },
    /* 20: Gas pipe              */ { 40,  30,  60, 0 },
};

/* -----------------------------------------------------------------------
 * Default world width/height per object type (obj[6], obj[7]).
 * From Amiga .s: Tree 128*256+128, HalfWorm 90*256+100, FlyingScalyBall #$6060, etc.
 * ----------------------------------------------------------------------- */
const ObjectWorldSize default_object_world_size[21] = {
    /*  0 alien       */ { 32,  32 },
    /*  1 Medipack    */ { 20,  20 },
    /*  2 Bullet      */ { 16,  16 },
    /*  3 Gun         */ { 40,  40 },
    /*  4 Key         */ { 20,  20 },
    /*  5 Player1     */ { 40,  40 },
    /*  6 Robot       */ { 32,  32 },
    /*  7 Big Nasty   */ { 80,  80 },
    /*  8 Flying Nasty*/ { 96,  96 },
    /*  9 Ammo        */ { 16,  16 },
    /* 10 Barrel      */ { 48,  50 },
    /* 11 Player2     */ { 40,  40 },
    /* 12 Mutant Marine */ { 32,  32 },
    /* 13 Worm        */ { 90, 100 },
    /* 14 Huge red    */ {128, 128},
    /* 15 Small red   */ {128, 128},
    /* 16 Tree        */ {128, 128},
    /* 17 Eye ball    */ { 16,  32 },
    /* 18 Tough Marine*/ { 69,  69 },
    /* 19 Flame Marine*/ { 69,  69 },
    /* 20 Gas pipe    */ { 40,  60 },
};

/* -----------------------------------------------------------------------
 * Bullet type data
 * Translated from Anims.s BulletSizes, HitNoises, ExplosiveForce
 * ----------------------------------------------------------------------- */
const BulletTypeData bullet_types[8] = {
    /* Type 0: Plasma bolt    */ { 0x10, 0x10,  0,  -1,   0 },
    /* Type 1: Plasma bolt2   */ { 0x08, 0x08,  0,  -1,   0 },
    /* Type 2: Rocket         */ { 0x10, 0x10, 64,  15, 300 },
    /* Type 3: Flame          */ { 0x10, 0x10,  0,  -1,   0 },
    /* Type 4: Grenade        */ { 0x10, 0x10, 40,   9, 200 },
    /* Type 5: Worm spit      */ { 0x08, 0x08,  0,  -1,   0 },
    /* Type 6: Marine shot    */ { 0x20, 0x20,  0,   3, 100 },
    /* Type 7: Big shot       */ { 0x20, 0x20,  0,  -1,   0 },
};

/* -----------------------------------------------------------------------
 * Enemy type parameters
 * Consolidated from all enemy .s files
 * ----------------------------------------------------------------------- */
const EnemyParams enemy_params[] = {
    /* [0] Normal Alien (NormalAlien.s ItsANasty) */
    {
        .thing_height   = 80 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 2,
        .melee_cooldown = 20,
        .melee_range    = 160,
        .shot_type      = -1,       /* melee only */
        .shot_power     = 0,
        .shot_speed     = 0,
        .shot_shift     = 0,
        .shot_cooldown  = 0,
        .damage_shift   = 0,        /* direct subtraction */
        .explode_threshold = 40,
        .wander_timer   = 50,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 40,
        .scream_sound   = 40,
        .hiss_sound     = 8,
        .attack_sound   = -1,
        .death_frames   = {33,33,33,33,33,33,33,33,33,33,33,
                           32,32,32,32,32,32,32,32,32,32,32,32,32,32,32, -1},
    },
    /* [1] Robot (Robot.s ItsARobot) */
    {
        .thing_height   = 160 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 128,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 0,
        .shot_type      = 4,        /* projectile type 4 */
        .shot_power     = 10,
        .shot_speed     = 16,
        .shot_shift     = 0,
        .shot_cooldown  = 50,
        .damage_shift   = 4,        /* asr.b #4 = divide by 16 */
        .explode_threshold = 0,     /* always explodes on death */
        .wander_timer   = 100,
        .hiss_timer_min = 0,
        .hiss_timer_range = 0,
        .death_sound    = 15,
        .scream_sound   = 8,
        .hiss_sound     = -1,
        .attack_sound   = -1,
        .death_frames   = {-1},
    },
    /* [2] Big Red Thing (BigRedThing.s) */
    {
        .thing_height   = 456 * 128,
        .step_up        = 40 * 256,
        .step_down      = 40 * 256,
        .extlen         = 160,
        .awayfromwall   = 2,
        .nas_height     = 128,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 80,
        .shot_type      = 2,        /* rocket-like */
        .shot_power     = 10,
        .shot_speed     = 32,
        .shot_shift     = 4,
        .shot_cooldown  = 30,
        .damage_shift   = 2,        /* divide by 4 */
        .explode_threshold = 0,
        .wander_timer   = 150,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = -1,
        .death_frames   = {9, -1},
    },
    /* [3] Half Worm (HalfWorm.s) */
    {
        .thing_height   = 200 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 80,
        .shot_type      = 5,        /* worm spit */
        .shot_power     = 10,
        .shot_speed     = 16,
        .shot_shift     = 3,
        .shot_cooldown  = 30,
        .damage_shift   = 1,        /* divide by 2 */
        .explode_threshold = 80,
        .wander_timer   = 50,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = -1,
        .death_frames   = {20,20,20,20,20,20, 19,19,19,19,19,19,19,19,19,19,
                           18,18,18,18,18,18,18,18,18,18, -1},
    },
    /* [4] Flame Marine (FlameMarine.s) */
    {
        .thing_height   = 128 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 2,        /* spread fire, 5 shots */
        .melee_cooldown = 20,
        .melee_range    = 80,
        .shot_type      = -1,       /* uses special spread fire */
        .shot_power     = 2,
        .shot_speed     = 0,
        .shot_shift     = 0,
        .shot_cooldown  = 0,
        .damage_shift   = 0,
        .explode_threshold = 40,
        .wander_timer   = 50,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = 21,
        .death_frames   = {18,18,18,18,18,18, 17,17,17,17,17,17,17,17,17,17,
                           16,16,16,16,16,16,16,16,16,16, -1},
    },
    /* [5] Tough Marine (ToughMarine.s) */
    {
        .thing_height   = 128 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 80,
        .shot_type      = 6,        /* marine shot */
        .shot_power     = 7,
        .shot_speed     = 32,
        .shot_shift     = 4,
        .shot_cooldown  = 50,
        .damage_shift   = 0,
        .explode_threshold = 40,
        .wander_timer   = 50,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = -1,
        .death_frames   = {18,18,18,18,18,18, 17,17,17,17,17,17,17,17,17,17,
                           16,16,16,16,16,16,16,16,16,16, -1},
    },
    /* [6] Mutant Marine (MutantMarine.s) */
    {
        .thing_height   = 128 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 4,
        .melee_cooldown = 50,
        .melee_range    = 80,
        .shot_type      = -1,       /* hitscan */
        .shot_power     = 4,
        .shot_speed     = 0,
        .shot_shift     = 0,
        .shot_cooldown  = 50,
        .damage_shift   = 0,
        .explode_threshold = 40,
        .wander_timer   = 50,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = 3,
        .death_frames   = {18,18,18,18,18,18, 17,17,17,17,17,17,17,17,17,17,
                           16,16,16,16,16,16,16,16,16,16, -1},
    },
    /* [7] Big Ugly Alien (BigUglyAlien.s ItsABigNasty) */
    {
        .thing_height   = 50 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 80,
        .shot_type      = 0,        /* plasma-like */
        .shot_power     = 10,
        .shot_speed     = 16,
        .shot_shift     = 0,
        .shot_cooldown  = 50,
        .damage_shift   = 0,
        .explode_threshold = 0,
        .wander_timer   = 20,
        .hiss_timer_min = 0,
        .hiss_timer_range = 0,
        .death_sound    = 8,
        .scream_sound   = 8,
        .hiss_sound     = -1,
        .attack_sound   = -1,
        .death_frames   = {-1},
    },
    /* [8] Big Claws (BigClaws.s) */
    {
        .thing_height   = 256 * 128,
        .step_up        = 20 * 256,
        .step_down      = 20 * 256,
        .extlen         = 160,
        .awayfromwall   = 2,
        .nas_height     = 128,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 80,
        .shot_type      = 2,        /* rocket-like */
        .shot_power     = 10,
        .shot_speed     = 64,
        .shot_shift     = 6,
        .shot_cooldown  = 30,
        .damage_shift   = 4,        /* divide by 16 */
        .explode_threshold = 0,
        .wander_timer   = 150,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = -1,
        .death_frames   = {9, -1},
    },
    /* [9] Flying Scaly Ball (FlyingScalyBall.s ItsAFlyingNasty) */
    {
        .thing_height   = 96 * 128,
        .step_up        = 0,
        .step_down      = 0x1000000,
        .extlen         = 160,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 120,
        .shot_type      = 0,        /* plasma */
        .shot_power     = 5,
        .shot_speed     = 16,
        .shot_shift     = 3,
        .shot_cooldown  = 50,
        .damage_shift   = 0,
        .explode_threshold = 40,
        .wander_timer   = 50,
        .hiss_timer_min = 0,
        .hiss_timer_range = 0,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = -1,
        .attack_sound   = 20,
        .death_frames   = {-1},
    },
    /* [10] Tree (Tree.s ItsATree) */
    {
        .thing_height   = 200 * 128,
        .step_up        = 0,
        .step_down      = 0,
        .extlen         = 80,
        .awayfromwall   = 1,
        .nas_height     = 64,
        .melee_damage   = 0,
        .melee_cooldown = 0,
        .melee_range    = 80,
        .shot_type      = 2,        /* tree shoots rockets */
        .shot_power     = 10,
        .shot_speed     = 16,
        .shot_shift     = 3,
        .shot_cooldown  = 30,
        .damage_shift   = 1,
        .explode_threshold = 80,
        .wander_timer   = 50,
        .hiss_timer_min = 300,
        .hiss_timer_range = 255,
        .death_sound    = 27,
        .scream_sound   = 27,
        .hiss_sound     = 8,
        .attack_sound   = -1,
        .death_frames   = {-1},
    },
};

const int num_enemy_types = sizeof(enemy_params) / sizeof(enemy_params[0]);

/* -----------------------------------------------------------------------
 * Pickup constants
 * ----------------------------------------------------------------------- */
/* Ammo graphic per gun type: from Anims.s AMGR */
const int8_t ammo_graphic_table[8] = { 3, 4, 5, 0, 29, 0, 0, 28 };

/* Ammo given per gun pickup: from Anims.s AmmoInGuns */
const int8_t ammo_in_guns[8] = { 0, 5, 1, 0, 1, 0, 0, 5 };

/* -----------------------------------------------------------------------
 * Brightness animation tables
 * Translated from Anims.s pulseANIM, flickerANIM, fireFlickerANIM
 * 999 = loop marker
 * ----------------------------------------------------------------------- */
const int16_t pulse_anim[] = {
    -10,-10,-9,-9,-8,-7,-6,-5,-4,-3,-2,-1,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10,
    9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    -1,-2,-3,-4,-5,-6,-7,-8,-9,
    999
};

const int16_t flicker_anim[] = {
    10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    -10,
    10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    10,10,10,10,10,10,10,10,10,10,
    -10,
    10,10,10,10,10,
    -10,
    999
};

const int16_t fire_flicker_anim[] = {
    -10,-9,-6,-10,-6,-5,-5,-7,-5,-10,-9,-8,-7,
    -5,-5,-5,-5,-5,-5,-5,-5,-6,-7,-8,-9,-5,
    -10,-9,-10,-6,-5,-5,-5,-5,-5,-5,-5,-5,
    999
};

/* -----------------------------------------------------------------------
 * End zones per level
 * Translated from AB3DI.s ENDZONES (line ~5940)
 * ----------------------------------------------------------------------- */
const int16_t end_zones[16] = {
    132, 149, 155, 107, 67, 132, 203, 166,
    118, 102, 103, 2, 98, 0, 148, 103
};

/* -----------------------------------------------------------------------
 * Level text
 * Translated from LevelBlurb.s LEVELTEXT
 * ----------------------------------------------------------------------- */
const char *level_text[16] = {
    "LEVEL 1: ORBITAL RESEARCH STATION",
    "LEVEL 2: THE STORAGE DEPOT",
    "LEVEL 3: DEEPER INTO THE BASE",
    "LEVEL 4: THE DARKENED HALLS",
    "LEVEL 5: POWER STATION",
    "LEVEL 6: UNDERGROUND",
    "LEVEL 7: THE LOWER DEPTHS",
    "LEVEL 8: SEWERS",
    "LEVEL 9: THE INNER SANCTUM",
    "LEVEL 10: THE ALIEN HIVE",
    "LEVEL 11: HELL",
    "LEVEL 12: THE CORE",
    "LEVEL 13: EXIT ROUTE",
    "LEVEL 14: THE SURFACE",
    "LEVEL 15: ALMOST THERE",
    "LEVEL 16: THE FINAL BATTLE",
};

const char *end_game_text =
    "CONGRATULATIONS!\n\n"
    "You have successfully completed\n"
    "Alien Breed 3D.\n\n"
    "The alien menace has been destroyed\n"
    "and the research station is safe.\n";

/* Game conditions (shared variable, for switches/doors) */
int16_t game_conditions = 0;
