/*
 * Alien Breed 3D I - PC Port
 * visibility.h - Zone ordering and line-of-sight
 *
 * Translated from: OrderZones.s, AB3DI.s (CanItBeSeen)
 *
 * The rendering system needs zones ordered front-to-back for the painter's
 * algorithm. OrderZones builds this list by traversing the room graph
 * from the viewer's position.
 *
 * CanItBeSeen traces visibility between two points through the zone graph
 * to determine if enemies/players can see each other.
 */

#ifndef VISIBILITY_H
#define VISIBILITY_H

#include "game_state.h"

/* Maximum rooms that can be visible at once */
#define MAX_VISIBLE_ROOMS   256
#define MAX_ORDER_ENTRIES   256

/* Ordered list of zones to draw */
typedef struct {
    int16_t  zones[MAX_ORDER_ENTRIES];
    int      count;
} ZoneOrder;

/*
 * OrderZones - Build back-to-front zone draw order.
 *
 * Translated from OrderZones.s.
 *
 * Traverses ListOfGraphRooms and outputs zone ids in list order (no sorting).
 */
void order_zones(ZoneOrder *out, const LevelState *level,
                 int32_t viewer_x, int32_t viewer_z,
                 int32_t move_dx, int32_t move_dz,
                 int viewer_angle,
                 const uint8_t *list_of_graph_rooms);

/*
 * CanItBeSeen - Line-of-sight check between two points.
 *
 * Returns bitmask: bit 0 = target can see viewer, bit 1 = viewer can see target.
 * Uses the zone graph to trace through rooms.
 */
/* full_height: if non-zero, accept crossing heights in either floor section
 * (used for hitscan so a lower-floor shooter can hit upper-floor targets). */
uint8_t can_it_be_seen(const LevelState *level,
                       const uint8_t *from_room, const uint8_t *to_room,
                       int16_t to_zone_id,
                       int16_t viewer_x, int16_t viewer_z, int16_t viewer_y,
                       int16_t target_x, int16_t target_z, int16_t target_y,
                       int8_t viewer_top, int8_t target_top,
                       int full_height);

#endif /* VISIBILITY_H */
