/*
 * Alien Breed 3D I - PC Port
 * visibility.c - Zone ordering and line-of-sight (full implementation)
 *
 * Translated from: OrderZones.s, AB3DI.s (CanItBeSeen)
 */

#include "visibility.h"
#include "math_tables.h"
#include <string.h>

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

/* 68020 btst/bset on data registers uses bit index modulo 32. */
static inline uint32_t reg_bit32(unsigned int bit_index)
{
    return (uint32_t)1u << (bit_index & 31u);
}

static inline int reg_btst32(uint32_t value, unsigned int bit_index)
{
    return (value & reg_bit32(bit_index)) != 0u;
}

/* Floor line offsets. Amiga side test uses words at 4 and 6 directly: result = dx*word6 - dz*word4. */
#define FLINE_SIZE    16
#define FLINE_X       0
#define FLINE_Z       2
#define FLINE_WORD4   4   /* Amiga muls 4(a1,d0.w),d3; our layout: xlen */
#define FLINE_WORD6   6   /* Amiga muls 6(a1,d0.w),d2; our layout: zlen */
#define FLINE_XLEN    FLINE_WORD4
#define FLINE_ZLEN    FLINE_WORD6
#define FLINE_CONNECT 8
#define FLINE_DIVISOR 10   /* Amiga divs 10(a2) for crossing height */

/* Zone data offsets (Defs.i ToZoneFloor etc.) */
#define ZONE_FLOOR_HEIGHT   2
#define ZONE_ROOF_HEIGHT    6
#define ZONE_UPPER_FLOOR    10
#define ZONE_UPPER_ROOF     14
#define ZONE_OFF_UPPER_FLOOR 10
#define ZONE_OFF_UPPER_ROOF 14
#define ZONE_EXIT_LIST      32
#define ZONE_LIST_OF_GRAPH  48

/* Return 1 if node_a appears before node_b when walking from head. */
static int node_before(int head, int node_a, int node_b,
                      const int *next)
{
    int n = head;
    while (n >= 0) {
        if (n == node_a) return 1;
        if (n == node_b) return 0;
        n = next[n];
    }
    return 0;
}

/* Unlink node from list; insert it before node 'before' (so node is drawn before that one). */
static void move_before(int node, int before,
                        int *next, int *prev)
{
    int p = prev[node], n = next[node];
    if (p >= 0) next[p] = n;
    if (n >= 0) prev[n] = p;
    prev[node] = prev[before];
    next[node] = before;
    if (prev[before] >= 0)
        next[prev[before]] = node;
    prev[before] = node;
}

/* -----------------------------------------------------------------------
 * order_zones - Amiga OrderZones: traverse list from current zone, then
 * reorder by portal (exit-line) side test so back-to-front order is correct.
 *
 * 1. Traverse ListOfGraphRooms (viewer's zone list at zone_data + 48).
 * 2. Build linked list in that order.
 * 3. RunThroughList + InsertList: for each zone, look at exit lines; if
 *    connected zone is in list and further away (viewer in front of line),
 *    but currently drawn before current, move current in front of connected.
 * 4. Output final list order.
 * ----------------------------------------------------------------------- */
void order_zones(ZoneOrder *out, const LevelState *level,
                 int32_t viewer_x, int32_t viewer_z,
                 int32_t move_dx, int32_t move_dz,
                 int viewer_angle,
                 const uint8_t *list_of_graph_rooms)
{
    (void)viewer_angle;
    (void)move_dx;
    (void)move_dz;
    out->count = 0;

    if (!level->data || !level->zone_adds || !level->floor_lines) {
        return;
    }

    uint8_t to_draw_tab[256];
    memset(to_draw_tab, 0, sizeof(to_draw_tab));

    /* WorkSpace[zone_id] = long at offset 4 in list entry (Amiga settodraw). */
    uint32_t workspace[256];
    memset(workspace, 0, sizeof(workspace));

    int16_t zone_list[256];
    int num_zones = 0;

    if (list_of_graph_rooms) {
        const uint8_t *lgr = list_of_graph_rooms;
        while (num_zones < MAX_ORDER_ENTRIES) {
            int16_t zid = read_be16(lgr);
            if (zid < 0) break;
            if (zid < 256) {
                to_draw_tab[zid] = 1;
                workspace[zid] = read_be32(lgr + 4);
                zone_list[num_zones++] = zid;
            }
            lgr += 8;
        }
    }
    if (num_zones == 0 && level->num_zones > 0) {
        int n = level->num_zones;
        if (n > 256) n = 256;
        for (int z = 0; z < n; z++) {
            to_draw_tab[z] = 1;
            /* workspace stays 0 when no list; we treat 0 as "all bits set" for indrawlist */
            zone_list[num_zones++] = (int16_t)z;
            if (num_zones >= MAX_ORDER_ENTRIES) break;
        }
    }
    if (num_zones == 0) return;

    /* Linked list by node index: next[i], prev[i], zone_id[i]. Head = 0, tail = num_zones-1. */
    int next[256], prev[256];
    int16_t node_zone[256];
    for (int i = 0; i < num_zones; i++) {
        node_zone[i] = zone_list[i];
        prev[i] = i - 1;
        next[i] = i + 1;
    }
    prev[0] = -1;
    next[num_zones - 1] = -1;
    int head = 0, tail = num_zones - 1;

    /* RunThroughList: multiple passes, each pass walk list from tail to head. */
    enum { k_order_passes = 100 };
    for (int pass = 0; pass < k_order_passes; pass++) {
        int node = tail;
        while (node >= 0) {
            int16_t cur_zone = node_zone[node];
            int32_t zone_off = read_be32(level->zone_adds + (int)cur_zone * 4);
            if (zone_off != 0) {
                const uint8_t *zone_data = level->data + zone_off;
                int16_t exit_rel = read_be16(zone_data + ZONE_EXIT_LIST);
                if (exit_rel != 0) {
                    const uint8_t *exit_list = zone_data + exit_rel;
                    uint32_t d6 = workspace[cur_zone];
                    for (int ei = 0; ei < 64; ei++) {
                        int16_t line_idx = read_be16(exit_list + ei * 2);
                        if (line_idx < 0) break;
                        int16_t connect = read_be16(level->floor_lines + (int)line_idx * FLINE_SIZE + FLINE_CONNECT);
                        if (connect < 0 || connect >= 256 || !to_draw_tab[connect]) continue;

                        /* Amiga InsertList bit flow:
                         *   b   = d7 (indrawlist gate)
                         *   b+1 = evaluated marker
                         *   b+2 = cached "mustdo" marker
                         * d7 advances by 3 per exit; btst/bset are register ops (bit index mod 32).
                         * WorkSpace=0 fallback: when d6 is 0, still run side test and reorder. */
                        unsigned int b = (unsigned)(ei * 3);
                        if (d6 != 0 && !reg_btst32(d6, b)) continue;

                        /* Amiga InsertList: side = dx*word6 - dz*word4; ble PutDone => reorder when side > 0 */
                        if (!reg_btst32(d6, b + 1u)) {
                            /* First time: mark evaluated, run side test. */
                            d6 |= reg_bit32(b + 1u);
                            const uint8_t *fline = level->floor_lines + (int)line_idx * FLINE_SIZE;
                            int32_t lx = (int32_t)read_be16(fline + FLINE_X);
                            int32_t lz = (int32_t)read_be16(fline + FLINE_Z);
                            int32_t word4 = (int32_t)read_be16(fline + FLINE_WORD4);
                            int32_t word6 = (int32_t)read_be16(fline + FLINE_WORD6);
                            int32_t dx = viewer_x - lx, dz = viewer_z - lz;  /* Amiga: move.w xoff/zoff */
                            int32_t side = dx * word6 - dz * word4;
                            if (side <= 0) continue;  /* Amiga: ble PutDone */
                            d6 |= reg_bit32(b + 2u);  /* mustdo */
                        } else {
                            if (!reg_btst32(d6, b + 2u)) continue;  /* wealreadyknow: only if mustdo set */
                        }

                        /* mustdo: connected is further; if it's earlier in list, move current in front of it (Amiga iscloser). */
                        int conn_node = -1;
                        for (int k = 0; k < num_zones; k++) {
                            if (node_zone[k] == connect) { conn_node = k; break; }
                        }
                        if (conn_node < 0) continue;
                        if (!node_before(head, conn_node, node, next)) continue;  /* connected not earlier, nothing to do */
                        if (node == head) head = (next[node] >= 0) ? next[node] : node;
                        if (node == tail) tail = (prev[node] >= 0) ? prev[node] : node;
                        move_before(node, conn_node, next, prev);
                        if (conn_node == head) head = node;
                        /* Amiga: bra InsertLoop - no return, so multiple reorders per node per pass */
                    }
                    workspace[cur_zone] = d6;  /* Amiga allinlist: move.l d6,(a6) */
                }
            }
            node = prev[node];
        }
    }

    /* Output final order (walk from head) */
    int n = head;
    int out_i = 0;
    while (n >= 0 && out_i < MAX_ORDER_ENTRIES) {
        out->zones[out_i++] = node_zone[n];
        n = next[n];
    }
    out->count = out_i;
}

/* -----------------------------------------------------------------------
 * can_it_be_seen - Line-of-sight (ObjectMove.s CanItBeSeen)
 *
 * 1. Same room: visible only if viewer_top == target_top.
 * 2. Else: to_room must be in from_room list-of-graph (when graph available).
 * 3. Clip points: left/right clip test when clips/points available.
 * 4. GoThroughZones: exit cross test, crossing height (fline+10 divisor),
 *    clearance vs current room (ViewerTop), GotIn (entry_top), target_top at end.
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

    /* Same room (insameroom): visible only if same floor */
    if (from_room == to_room) {
        return (viewer_top == target_top) ? 0x03u : 0u;
    }

    if (!level->zone_adds) return 0;

    int16_t to_zone_id = read_be16(to_room);
    const uint8_t *list = from_room + ZONE_LIST_OF_GRAPH;
    bool in_list = false;
    int16_t clip_off = -1;

    /* InList: is to_room in from_room's list-of-graph? */
    if (level->zone_graph_adds && level->graphics) {
        const uint8_t *entry = list;
        for (int i = 0; i < 256; i++) {
            int16_t list_index = read_be16(entry);
            if (list_index < 0) break;
            uint32_t gfx_off = (uint32_t)read_be32(level->zone_graph_adds + (unsigned)list_index * 8u);
            int16_t entry_zone_id = read_be16(level->graphics + gfx_off);
            if (entry_zone_id == to_zone_id) {
                in_list = true;
                clip_off = read_be16(entry + 2);
                break;
            }
            entry += 8;
        }
        if (!in_list) return 0;
    } else {
        /* No graph data: allow and skip clip check (fallback) */
        in_list = true;
    }

    int32_t dx = (int32_t)target_x - (int32_t)viewer_x;
    int32_t dz = (int32_t)target_z - (int32_t)viewer_z;

    /* Clip check (left/right) when clips/points available */
    if (in_list && clip_off >= 0 && level->clips && level->points) {
        const uint8_t *clip_ptr = level->clips + (unsigned)clip_off * 2u;
        for (;;) {
            int16_t pt_idx = read_be16(clip_ptr);
            if (pt_idx < 0) break;
            int16_t px = read_be16(level->points + (unsigned)pt_idx * 4u);
            int16_t pz = read_be16(level->points + (unsigned)pt_idx * 4u + 2u);
            int32_t cross = ((int32_t)px - (int32_t)viewer_x) * dz - ((int32_t)pz - (int32_t)viewer_z) * dx;
            if (cross <= 0) return 0;
            clip_ptr += 2;
        }
        clip_ptr += 2;
        for (;;) {
            int16_t pt_idx = read_be16(clip_ptr);
            if (pt_idx < 0) break;
            int16_t px = read_be16(level->points + (unsigned)pt_idx * 4u);
            int16_t pz = read_be16(level->points + (unsigned)pt_idx * 4u + 2u);
            int32_t cross = ((int32_t)px - (int32_t)viewer_x) * dz - ((int32_t)pz - (int32_t)viewer_z) * dx;
            if (cross >= 0) return 0;
            clip_ptr += 2;
        }
    }

    /* GoThroughZones */
    int32_t dy = (int32_t)target_y - (int32_t)viewer_y;
    const uint8_t *current_room = from_room;
    int8_t d2 = viewer_top;
    int max_depth = 20;

    for (int depth = 0; depth < max_depth; depth++) {
        int16_t exit_rel = read_be16(current_room + ZONE_EXIT_LIST);
        if (exit_rel == 0) break;

        const uint8_t *exit_list = current_room + exit_rel;
        bool found_exit = false;

        for (int i = 0; i < 50; i++) {
            int16_t line_idx = read_be16(exit_list + i * 2);
            if (line_idx < 0) break;

            const uint8_t *fline = level->floor_lines + (unsigned)line_idx * (unsigned)FLINE_SIZE;
            int16_t lx = read_be16(fline + FLINE_X);
            int16_t lz = read_be16(fline + FLINE_Z);
            int16_t lxlen = read_be16(fline + FLINE_XLEN);
            int16_t lzlen = read_be16(fline + FLINE_ZLEN);
            int16_t connect = read_be16(fline + FLINE_CONNECT);

            int32_t d3 = (int32_t)viewer_x - (int32_t)lx;
            int32_t d4 = (int32_t)viewer_z - (int32_t)lz;
            int32_t view_side = d3 * (int32_t)lzlen - d4 * (int32_t)lxlen;
            if (view_side <= 0) continue;

            int32_t d5 = d3 + (int32_t)lxlen;
            int32_t d6 = d4 + (int32_t)lzlen;
            int32_t targ_side = d5 * (int32_t)lzlen - d6 * (int32_t)lxlen;
            if (targ_side >= 0) continue;

            if (connect < 0) return 0;

            int16_t divisor = read_be16(fline + FLINE_DIVISOR);
            if (divisor == 0) divisor = 1;
            int32_t num_t = (int32_t)(target_x - lx) * (int32_t)lzlen - (int32_t)(target_z - lz) * (int32_t)lxlen;
            int32_t num_v = (int32_t)(viewer_x - lx) * (int32_t)lzlen - (int32_t)(viewer_z - lz) * (int32_t)lxlen;
            num_t /= (int32_t)divisor;
            num_v /= (int32_t)divisor;
            int32_t den = num_t + num_v;
            int32_t cross_y_16 = (int16_t)viewer_y;
            if (den != 0) {
                int32_t frac = (int32_t)dy * num_v / den;
                cross_y_16 = (int16_t)viewer_y + (int16_t)frac;
            }
            int32_t cross_y = (int32_t)cross_y_16 << 7;

            int32_t floor_h = read_be32(current_room + ZONE_FLOOR_HEIGHT);
            int32_t roof_h = read_be32(current_room + ZONE_ROOF_HEIGHT);
            if (d2) {
                floor_h = read_be32(current_room + ZONE_UPPER_FLOOR);
                roof_h = read_be32(current_room + ZONE_UPPER_ROOF);
            }
            if (cross_y < roof_h || cross_y > floor_h) continue;

            int32_t next_off = read_be32(level->zone_adds + (unsigned)connect * 4u);
            const uint8_t *next_zone = level->data + next_off;

            int32_t next_floor = read_be32(next_zone + ZONE_FLOOR_HEIGHT);
            int32_t next_roof = read_be32(next_zone + ZONE_ROOF_HEIGHT);
            int8_t entry_top = 0;
            if (cross_y > next_floor) return 0;
            if (cross_y <= next_roof) {
                entry_top = 0;
            } else {
                int32_t up_floor = read_be32(next_zone + ZONE_UPPER_FLOOR);
                int32_t up_roof = read_be32(next_zone + ZONE_UPPER_ROOF);
                if (cross_y > up_floor) return 0;
                if (cross_y < up_roof) return 0;
                entry_top = 1;
            }

            if (next_zone == to_room) {
                return (entry_top == target_top) ? 0x03u : 0u;
            }
            current_room = next_zone;
            d2 = entry_top;
            found_exit = true;
            break;
        }
        if (!found_exit) break;
    }

    return 0;
}
