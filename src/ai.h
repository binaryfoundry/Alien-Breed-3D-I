/*
 * Alien Breed 3D I - PC Port
 * ai.h - AI decision system
 *
 * Translated from: AI.s
 *
 * The AI system provides high-level behavior for marines:
 *   - BuildVisibleList: determines which objects are friends/enemies
 *   - AIControl: decision tree based on Aggression/Cooperation/Movement
 *   - Combatant: attack, follow leader, or retreat
 *   - NonCombatant: follow leader or idle
 */

#ifndef AI_H
#define AI_H

#include "game_state.h"

/* AI personality parameters */
typedef struct {
    int16_t  aggression;    /* 0=coward, 30=berserk */
    int16_t  movement;      /* 0=still, 30=fast */
    int16_t  cooperation;   /* 0=solo, 30=team player */
    uint8_t  ident;         /* creature type ID */
    uint32_t friends;       /* bitmask of friendly obj types */
    uint32_t enemies;       /* bitmask of enemy obj types */
    bool     armed;         /* can this AI fire weapons? */
    int16_t  shot_type;     /* bullet type when armed */
    int16_t  shot_power;    /* damage when armed */
    int16_t  shot_speed;    /* projectile speed */
    int16_t  shot_shift;    /* distance shift for Y aim */
} AIParams;

/* Run the full AI decision tree for an object.
 * Translated from AI.s AIControl.
 * obj must be a valid object in ObjectData. */
void ai_control(GameObject *obj, GameState *state, const AIParams *params);

/* GoInDirection - Move object in its facing direction at speed.
 * Translated from ObjectMove.s GoInDirection. */
void go_in_direction(int32_t *newx, int32_t *newz,
                     int32_t oldx, int32_t oldz,
                     int16_t angle, int16_t speed);

/* ExplodeIntoBits - enemy death explosion into fragments.
 * Creates visual debris (rendering side is stubbed). */
void explode_into_bits(GameObject *obj, GameState *state);

/* ViewpointToDraw - determine which sprite frame to draw for an
 * enemy based on the viewer's angle relative to the enemy facing.
 * This is rendering-related but the angle calculation is game logic. */
int16_t viewpoint_to_draw(int16_t viewer_x, int16_t viewer_z,
                          int16_t obj_x, int16_t obj_z,
                          int16_t obj_facing);

/* 16-direction version for sprite types with 16 rotational frames (alien, marine, etc.). */
int16_t viewpoint_to_draw_16(int16_t viewer_x, int16_t viewer_z,
                             int16_t obj_x, int16_t obj_z,
                             int16_t obj_facing);

#endif /* AI_H */
