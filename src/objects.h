/*
 * Alien Breed 3D I - PC Port
 * objects.h - Object system (animation, movement, interaction)
 *
 * Translated from: Anims.s (ObjMoveAnim, ObjectDataHandler, Objectloop),
 *                  ObjectMove.s, various enemy .s files
 *
 * The object system processes all game entities each frame:
 *   - Player shooting
 *   - Switch/door/lift mechanics
 *   - Object type dispatch (aliens, pickups, bullets, etc.)
 *   - Brightness animations
 */

#ifndef OBJECTS_H
#define OBJECTS_H

#include "game_state.h"

/* -----------------------------------------------------------------------
 * Object processing - called once per frame from the game loop
 * Equivalent to ObjMoveAnim in Anims.s
 * ----------------------------------------------------------------------- */
void objects_update(GameState *state);

/* -----------------------------------------------------------------------
 * Set obj[6]/obj[7] (world width/height) from default_object_world_size when
 * level data has them as 0. Call after level load so each object has size in its record (Amiga style).
 * ----------------------------------------------------------------------- */
void object_init_world_sizes_from_types(LevelState *level);

/* -----------------------------------------------------------------------
 * Individual object type handlers
 * Each translates from the corresponding ItsA* function in Anims.s
 * ----------------------------------------------------------------------- */

/* Aliens/enemies */
void object_handle_alien(GameObject *obj, GameState *state);
void object_handle_flying_nasty(GameObject *obj, GameState *state);
void object_handle_robot(GameObject *obj, GameState *state);
void object_handle_marine(GameObject *obj, GameState *state);
void object_handle_worm(GameObject *obj, GameState *state);
void object_handle_huge_red(GameObject *obj, GameState *state);
void object_handle_big_claws(GameObject *obj, GameState *state);
void object_handle_big_nasty(GameObject *obj, GameState *state);
void object_handle_tree(GameObject *obj, GameState *state);
void object_handle_barrel(GameObject *obj, GameState *state);
void object_handle_gas_pipe(GameObject *obj, GameState *state);

/* Pickups */
void object_handle_medikit(GameObject *obj, GameState *state);
void object_handle_ammo(GameObject *obj, GameState *state);
void object_handle_key(GameObject *obj, GameState *state);
void object_handle_big_gun(GameObject *obj, GameState *state);

/* Bullets */
void object_handle_bullet(GameObject *obj, GameState *state);

/* Level mechanics */
void door_routine(GameState *state);
void lift_routine(GameState *state);
void switch_routine(GameState *state);

/* Water animations */
void do_water_anims(GameState *state);

/* Brightness animation */
void bright_anim_handler(GameState *state);

/* Utility: fire a projectile from an enemy at a player */
void enemy_fire_at_player(GameObject *obj, GameState *state,
                          int player_num, int shot_type, int shot_power,
                          int shot_speed, int shot_shift);

/* Utility: compute blast damage to nearby objects */
void compute_blast(GameState *state, int32_t x, int32_t z, int32_t y,
                   int16_t radius, int16_t power);

/* Utility: player-object pickup distance check */
int pickup_distance_check(GameObject *obj, GameState *state, int player_num);

/* Player object updates (USEPLR1/USEPLR2 from AB3DI.s) */
void use_player1(GameState *state);
void use_player2(GameState *state);

/* CalcInLine - calculate which objects are in player's line of sight */
void calc_plr1_in_line(GameState *state);
void calc_plr2_in_line(GameState *state);

/* Object visibility arrays */
#define MAX_OBJECTS 250
extern int8_t  plr1_obs_in_line[MAX_OBJECTS];
extern int8_t  plr2_obs_in_line[MAX_OBJECTS];
extern int16_t plr1_obj_dists[MAX_OBJECTS];
extern int16_t plr2_obj_dists[MAX_OBJECTS];

#endif /* OBJECTS_H */
