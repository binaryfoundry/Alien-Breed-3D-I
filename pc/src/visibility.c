/*
 * Alien Breed 3D I - PC Port
 * visibility.c - Zone ordering and line-of-sight (full implementation)
 *
 * Translated from: OrderZones.s, AB3DI.s (CanItBeSeen)
 */

#include "visibility.h"
#include "math_tables.h"
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Helper: read big-endian values from level data
 * ----------------------------------------------------------------------- */
static int16_t read_be16(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}

static int32_t read_be32(const uint8_t *p)
{
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* Floor line offsets */
#define FLINE_SIZE  16
#define FLINE_X     0
#define FLINE_Z     2
#define FLINE_XLEN  4
#define FLINE_ZLEN  6
#define FLINE_CONNECT 14

/* Zone data offsets */
#define ZONE_FLOOR_HEIGHT   2
#define ZONE_ROOF_HEIGHT    6
#define ZONE_UPPER_FLOOR    10
#define ZONE_UPPER_ROOF     14
#define ZONE_OFF_UPPER_FLOOR 10
#define ZONE_OFF_UPPER_ROOF 14
#define ZONE_EXIT_LIST      32
#define ZONE_LIST_OF_GRAPH  48

/* -----------------------------------------------------------------------
 * order_zones - Build back-to-front zone draw order
 *
 * Translated from OrderZones.s (full algorithm).
 *
 * Algorithm:
 * 1. Initialize ToDrawTab[256] = 0 and OrderTab[256]
 * 2. Walk ListOfGraphRooms marking rooms to draw
 * 3. For each marked room, calculate view-space depth (same as renderer:
 *    depth = (mid_x - viewer_x)*sin + (mid_z - viewer_z)*cos for first exit line midpoint)
 * 4. Sort descending by depth (largest = farthest first)
 * 5. Build output array (index 0 = farthest, drawn first for painter's algorithm)
 * ----------------------------------------------------------------------- */
void order_zones(ZoneOrder *out, const LevelState *level,
                 int32_t viewer_x, int32_t viewer_z,
                 int viewer_angle,
                 const uint8_t *list_of_graph_rooms)
{
    out->count = 0;

    if (!level->data || !level->zone_adds) {
        return;
    }

    /* State arrays */
    uint8_t to_draw_tab[256];
    int32_t distances[256];
    int16_t zone_ids[256];
    int num_zones = 0;

    memset(to_draw_tab, 0, sizeof(to_draw_tab));
    memset(distances, 0, sizeof(distances));

    /* Step 1: Mark rooms to draw.
     * If list_of_graph_rooms is provided, walk it (format: int16 zone_id per 8-byte entry, -1 terminator).
     * If NULL or list yields no zones, fallback: mark all zones 0..num_zones-1 so something is drawn.
     */
    if (list_of_graph_rooms) {
        const uint8_t *lgr = list_of_graph_rooms;
        while (1) {
            int16_t zone_id = read_be16(lgr);
            if (zone_id < 0) break;
            if (zone_id < 256) {
                to_draw_tab[zone_id] = 1;
            }
            lgr += 8; /* 8 bytes per entry */
        }
    }
    /* Fallback: if no list or no zones marked (e.g. real level format has list at zone+48 that isn't zone IDs), draw all */
    {
        int any_marked = 0;
        for (int z = 0; z < 256; z++) {
            if (to_draw_tab[z]) { any_marked = 1; break; }
        }
        if (!any_marked && level->num_zones > 0) {
            int n = level->num_zones;
            if (n > 256) n = 256;
            for (int z = 0; z < n; z++) {
                to_draw_tab[z] = 1;
            }
        }
    }

    /* View-space depth uses same rotation as renderer: view_z = dx*sin + dz*cos */
    int16_t sin_v = sin_lookup(viewer_angle);
    int16_t cos_v = cos_lookup(viewer_angle);

    /* Step 2: For each marked zone, use closest extent (min depth over all exit line midpoints).
     * Order by this so we draw the zone whose *closest* boundary is farthest first (correct when zones overlap). */
    if (!level->floor_lines) return;

    for (int z = 0; z < 256; z++) {
        if (!to_draw_tab[z]) continue;

        if (!level->zone_adds) continue;
        int32_t zone_off = read_be32(level->zone_adds + z * 4);
        if (zone_off == 0) continue;

        const uint8_t *zone_data = level->data + zone_off;
        int16_t exit_rel = read_be16(zone_data + ZONE_EXIT_LIST);
        if (exit_rel == 0) continue;
        const uint8_t *exit_list = zone_data + exit_rel;

        /* Min depth over all exit line midpoints = zone's closest extent in view space */
        int32_t min_depth = (int32_t)0x7FFFFFFF; /* INT32_MAX so any real depth is smaller */
        int exit_count = 0;
        const int max_exits = 32;
        while (exit_count < max_exits) {
            int16_t line_idx = read_be16(exit_list + exit_count * 2);
            if (line_idx < 0) break;
            exit_count++;

            const uint8_t *fline = level->floor_lines + line_idx * FLINE_SIZE;
            int16_t lx = read_be16(fline + FLINE_X);
            int16_t lz = read_be16(fline + FLINE_Z);
            int16_t lxlen = read_be16(fline + FLINE_XLEN);
            int16_t lzlen = read_be16(fline + FLINE_ZLEN);
            int32_t mid_x = (int32_t)lx + (int32_t)lxlen / 2;
            int32_t mid_z = (int32_t)lz + (int32_t)lzlen / 2;
            int32_t dx = mid_x - viewer_x;
            int32_t dz = mid_z - viewer_z;
            int32_t d = (int32_t)dx * sin_v + (int32_t)dz * cos_v;
            if (d < min_depth) min_depth = d;
        }
        if (min_depth == (int32_t)0x7FFFFFFF) continue; /* no valid exit */

        zone_ids[num_zones] = (int16_t)z;
        distances[num_zones] = min_depth;
        num_zones++;

        if (num_zones >= MAX_ORDER_ENTRIES) break;
    }

    /* Step 3: Sort by depth descending (farthest first for painter's algorithm).
     * Larger depth = farther in view space. Using `<` in the comparison so we
     * shift elements right when they are LESS than the key, producing descending order. */
    for (int i = 1; i < num_zones; i++) {
        int32_t key_dist = distances[i];
        int16_t key_zone = zone_ids[i];
        int j = i - 1;
        while (j >= 0 && distances[j] < key_dist) {
            distances[j + 1] = distances[j];
            zone_ids[j + 1] = zone_ids[j];
            j--;
        }
        distances[j + 1] = key_dist;
        zone_ids[j + 1] = key_zone;
    }

    /* Step 4: Build output (far to near for painter's algorithm) */
    for (int i = 0; i < num_zones && i < MAX_ORDER_ENTRIES; i++) {
        out->zones[i] = zone_ids[i];
    }
    out->count = num_zones;
}

/* -----------------------------------------------------------------------
 * can_it_be_seen - Line-of-sight check
 *
 * Translated from AB3DI.s CanItBeSeen.
 *
 * Algorithm:
 * 1. If same room -> visible
 * 2. Trace line from viewer to target through room graph
 * 3. At each room boundary, check:
 *    a. Does the line cross an exit line?
 *    b. Is there floor/ceiling clearance at the crossing point?
 * 4. If we reach the target's room -> visible
 * 5. If we run out of rooms to check -> not visible
 * ----------------------------------------------------------------------- */
uint8_t can_it_be_seen(const LevelState *level,
                       const uint8_t *from_room, const uint8_t *to_room,
                       int16_t viewer_x, int16_t viewer_z, int16_t viewer_y,
                       int16_t target_x, int16_t target_z, int16_t target_y,
                       int8_t viewer_top, int8_t target_top)
{
    if (!level->data || !level->floor_lines) {
        return 0;
    }

    /* Same room = always visible */
    if (from_room == to_room) {
        return 0x03; /* Both bits set */
    }

    /* Trace through rooms */
    int32_t dy = target_y - viewer_y;

    const uint8_t *current_room = from_room;
    int max_depth = 20; /* Prevent infinite loops */

    for (int depth = 0; depth < max_depth; depth++) {
        if (!current_room) break;

        /* Get exit list for current room (16-bit relative offset) */
        int16_t exit_rel = read_be16(current_room + ZONE_EXIT_LIST);
        if (exit_rel == 0) break;

        const uint8_t *exit_list = current_room + exit_rel;
        bool found_next = false;

        /* Check each exit line */
        for (int i = 0; i < 50; i++) {
            int16_t line_idx = read_be16(exit_list + i * 2);
            if (line_idx < 0) break;

            const uint8_t *fline = level->floor_lines + line_idx * FLINE_SIZE;

            int16_t lx = read_be16(fline + FLINE_X);
            int16_t lz = read_be16(fline + FLINE_Z);
            int16_t lxlen = read_be16(fline + FLINE_XLEN);
            int16_t lzlen = read_be16(fline + FLINE_ZLEN);
            int16_t connect = read_be16(fline + FLINE_CONNECT);

            if (connect < 0) continue;

            /* Check if line from viewer to target crosses this exit */
            int32_t view_cross = (int32_t)((int32_t)viewer_x - lx) * lzlen -
                                 (int32_t)((int32_t)viewer_z - lz) * lxlen;
            int32_t targ_cross = (int32_t)((int32_t)target_x - lx) * lzlen -
                                 (int32_t)((int32_t)target_z - lz) * lxlen;

            /* Signs differ = line crosses */
            if ((view_cross ^ targ_cross) < 0) {
                /* Calculate crossing height */
                int32_t total_cross = view_cross - targ_cross;
                if (total_cross == 0) continue;

                int32_t t = (view_cross * 256) / total_cross; /* 0-256 = 0.0-1.0 */
                int32_t cross_y = viewer_y + (dy * t) / 256;

                /* Get connected zone */
                int32_t zone_off = read_be32(level->zone_adds + connect * 4);
                const uint8_t *next_zone = level->data + zone_off;

                /* Check height clearance (using upper floor if in_top) */
                int32_t floor_h, roof_h;
                if (viewer_top || target_top) {
                    /* Check upper floor */
                    floor_h = read_be32(next_zone + ZONE_OFF_UPPER_FLOOR);
                    roof_h = read_be32(next_zone + ZONE_OFF_UPPER_ROOF);
                } else {
                    floor_h = read_be32(next_zone + ZONE_FLOOR_HEIGHT);
                    roof_h = read_be32(next_zone + ZONE_ROOF_HEIGHT);
                }

                if (cross_y >= floor_h && cross_y <= roof_h) {
                    /* Can pass through */
                    if (next_zone == to_room) {
                        return 0x03; /* Visible! */
                    }
                    current_room = next_zone;
                    found_next = true;
                    break;
                }
            }
        }

        if (!found_next) break;
    }

    return 0; /* Not visible */
}
