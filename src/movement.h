/*
 * Alien Breed 3D I - PC Port
 * movement.h - Object movement, collision, and physics
 *
 * Translated from: ObjectMove.s, Fall.s
 *
 * The movement system handles:
 *   - Moving objects through the zone/room graph
 *   - Wall collision detection using floor lines
 *   - Object-to-object collision
 *   - Gravity / falling
 *   - Room transitions
 *   - Teleportation
 */

#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "game_state.h"

/* -----------------------------------------------------------------------
 * Movement parameters (set before calling move_object)
 * These mirror the global variables used by MoveObject in the ASM.
 * ----------------------------------------------------------------------- */
typedef struct {
    int32_t  oldx, oldy, oldz;     /* previous position */
    int32_t  newx, newy, newz;     /* target position */
    int32_t  xdiff, zdiff;         /* movement delta */
    int16_t  extlen;               /* wall extension length (collision margin) */
    int8_t   awayfromwall;         /* push away from wall on collision */
    int8_t   exitfirst;            /* check exit lines first */
    int8_t   wallbounce;           /* bounce off walls */
    int8_t   stood_in_top;         /* which floor layer object is on */
    int32_t  thing_height;         /* object height for collision */
    int32_t  step_up_val;          /* maximum step-up height */
    int32_t  step_down_val;        /* maximum step-down height */
    uint8_t *objroom;              /* current room pointer (into level data) */
    uint8_t *no_transition_back;   /* if set, do not transition to this room (stairs: avoid stepping back) */
    int16_t  coll_id;              /* this object's collision ID */
    uint32_t collide_flags;        /* bitmask of what to collide with */
    uint16_t wall_flags;           /* wall type flags */
    int8_t   hitwall;              /* result: did we hit something? */
    int8_t   pos_shift;            /* position scale: 0 = integer, 16 = 16.16 fp */
} MoveContext;

/* -----------------------------------------------------------------------
 * Functions
 * ----------------------------------------------------------------------- */

/* Initialize a MoveContext with default values */
void move_context_init(MoveContext *ctx);

/*
 * MoveObject - Move an object through the level, handling room transitions.
 *
 * Translated from ObjectMove.s MoveObject.
 * Takes old/new positions in ctx, resolves room transitions,
 * checks wall intersections, updates objroom and final position.
 */
void move_object(MoveContext *ctx, LevelState *level);

/*
 * Collision - Check object-to-object collision at position (newx, newz).
 *
 * Translated from ObjectMove.s Collision.
 * Iterates ObjectData, checks collision boxes. Sets ctx->hitwall if hit.
 */
void collision_check(MoveContext *ctx, LevelState *level);

/*
 * Player falling / gravity.
 *
 * Translated from Fall.s Plr1Fall / Plr2Fall.
 * Applies gravity acceleration, water drag, and ground clamping.
 */
void player_fall(int32_t *yoff, int32_t *yvel, int32_t tyoff,
                 int32_t water_level, bool in_water);

/*
 * HeadTowards - Calculate movement vector toward a target point.
 *
 * Translated from ObjectMove.s HeadTowards.
 * Sets newx/newz based on direction to target at given speed.
 */
void head_towards(MoveContext *ctx, int32_t target_x, int32_t target_z,
                  int16_t speed);

/*
 * HeadTowardsAng - Move in a direction with angle-based steering.
 *
 * Translated from ObjectMove.s HeadTowardsAng.
 * Adjusts facing angle toward target and moves forward.
 */
void head_towards_angle(MoveContext *ctx, int16_t *facing,
                        int32_t target_x, int32_t target_z,
                        int16_t speed, int16_t turn_speed);

/*
 * CheckTeleport - Check if current zone has a teleport destination.
 *
 * Translated from ObjectMove.s CheckTeleport.
 * If the zone's ToTelZone field is >= 0, teleports the object to the
 * destination coordinates and adjusts Y for floor height difference.
 *
 * Returns: true if teleported, false otherwise.
 */
bool check_teleport(MoveContext *ctx, LevelState *level, int16_t zone_id);

/*
 * FindCloseRoom - Find the room closest to the player from an object.
 *
 * Translated from ObjectMove.s FindCloseRoom.
 * Used by enemies to find which room they should be in after spawning.
 */
void find_close_room(MoveContext *ctx, LevelState *level, int16_t distance);

#endif /* MOVEMENT_H */
