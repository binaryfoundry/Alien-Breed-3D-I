/*
 * Alien Breed 3D I - PC Port
 * level.c - Level data parsing
 *
 * Translated from: AB3DI.s blag: section (~line 722-848)
 */

#include "level.h"
#include <stdio.h>
#include <string.h>

/* Helper to read big-endian 16-bit word from buffer */
static int16_t read_word(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}

/* Helper to read big-endian 32-bit long from buffer */
static int32_t read_long(const uint8_t *p)
{
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* -----------------------------------------------------------------------
 * level_parse - Parse loaded level data, resolving internal offsets
 *
 * This mirrors the "blag:" section in AB3DI.s where all offsets in the
 * level data are resolved to absolute pointers.
 * ----------------------------------------------------------------------- */
int level_parse(LevelState *level)
{
    if (!level->data || !level->graphics) {
        printf("[LEVEL] Cannot parse - data not loaded\n");
        return -1;
    }

    printf("[LEVEL] Parsing level data...\n");

    uint8_t *ld = level->data;      /* LEVELDATA */
    uint8_t *lg = level->graphics;  /* LEVELGRAPHICS */

    /* ---- Graphics data header ---- */
    /* Long 0: Offset to doors (0 = no door table) */
    int32_t door_offset = read_long(lg + 0);
    if (door_offset > 0) {
        level->door_data = lg + door_offset;
    } else {
        level->door_data = NULL;
    }

    /* Long 4: Offset to lifts */
    int32_t lift_offset = read_long(lg + 4);
    level->lift_data = lg + lift_offset;

    /* Long 8: Offset to switches */
    int32_t switch_offset = read_long(lg + 8);
    level->switch_data = lg + switch_offset;

    /* Long 12: Offset to zone graph adds */
    int32_t zone_graph_offset = read_long(lg + 12);
    level->zone_graph_adds = lg + zone_graph_offset;

    /* Zone offset table starts at byte 16 of graphics data */
    level->zone_adds = lg + 16;

    /* ---- Level data header ---- */
    /* Byte 14: Number of points (word) */
    int16_t num_points = read_word(ld + 14);

    /* Byte 16: Number of zones (word) */
    level->num_zones = read_word(ld + 16);

    /* Byte 20: Number of object points (word) */
    level->num_object_points = read_word(ld + 20);

    /* Byte 22: Offset to points (long) */
    int32_t points_offset = read_long(ld + 22);
    level->points = ld + points_offset;
    /* Point brights follow points: points + 4*num_points + 4 */
    level->point_brights = level->points + 4 + num_points * 4;

    /* Long 26: Offset to floor lines */
    int32_t floor_offset = read_long(ld + 26);
    level->floor_lines = ld + floor_offset;

    /* Long 30: Offset to object data */
    int32_t obj_offset = read_long(ld + 30);
    level->object_data = ld + obj_offset;

    /* Number of floor lines (16 bytes each) for brute-force collision */
    level->num_floor_lines = (obj_offset - floor_offset) / 16;
    if (level->num_floor_lines < 0) level->num_floor_lines = 0;

    /* Long 34: Offset to player shot data */
    int32_t pshot_offset = read_long(ld + 34);
    level->player_shot_data = ld + pshot_offset;

    /* Long 38: Offset to nasty shot data */
    int32_t nshot_offset = read_long(ld + 38);
    level->nasty_shot_data = ld + nshot_offset;
    /* Other nasty data follows: 64*20 bytes after nasty shots */
    level->other_nasty_data = level->nasty_shot_data + 64 * 20;

    /* Long 42: Offset to object points */
    int32_t objpts_offset = read_long(ld + 42);
    level->object_points = ld + objpts_offset;

    /* Long 46: Offset to player 1 object */
    int32_t plr1_offset = read_long(ld + 46);
    level->plr1_obj = ld + plr1_offset;

    /* Long 50: Offset to player 2 object */
    int32_t plr2_offset = read_long(ld + 50);
    level->plr2_obj = ld + plr2_offset;

    printf("[LEVEL] Parsed: %d zones, %d points, %d obj_points, %d floor_lines\n",
           level->num_zones, num_points, level->num_object_points, level->num_floor_lines);

    return 0;
}

/* -----------------------------------------------------------------------
 * level_assign_clips - Assign clip offsets to zone graph lists
 *
 * Translated from AB3DI.s assignclips loop (~line 812-843).
 *
 * Each zone has a list of graphical elements (walls, floors, etc).
 * The clip data provides pre-computed clipping polygons for these.
 * This function links the clip data into the zone graph lists.
 * ----------------------------------------------------------------------- */
void level_assign_clips(LevelState *level, int16_t num_zones)
{
    if (!level->clips || !level->zone_adds || !level->data) {
        return;
    }

    uint8_t *zone_offsets = level->zone_adds;  /* lg + 16 */
    uint8_t *ld = level->data;
    uint8_t *clips = level->clips;

    int32_t clip_byte_offset = 0;

    for (int16_t z = 0; z <= num_zones; z++) {
        /* Get zone offset from graphics data -> points into level data */
        int32_t zone_add = read_long(zone_offsets + z * 4);
        uint8_t *zone_ptr = ld + zone_add;

        /* Go to the list of graph elements (ToListOfGraph offset) */
        uint8_t *graph_list = zone_ptr + ZONE_OFF_LIST_OF_GRAPH;

        /* Walk the graph list (each entry is 8 bytes: type, clip_offset, ...) */
        while (1) {
            int16_t entry_type = read_word(graph_list);
            if (entry_type < 0) break;  /* End of zone list */

            int16_t clip_ref = read_word(graph_list + 2);
            if (clip_ref >= 0) {
                /* Assign current clip offset */
                int16_t half_offset = (int16_t)(clip_byte_offset >> 1);
                /* Write back (big-endian) */
                graph_list[2] = (uint8_t)(half_offset >> 8);
                graph_list[3] = (uint8_t)(half_offset & 0xFF);

                /* Find next clip boundary (-2 sentinel) */
                while (read_word(clips + clip_byte_offset) != -2) {
                    clip_byte_offset += 2;
                }
                clip_byte_offset += 2;  /* Skip the -2 sentinel */
            }

            graph_list += 8;  /* Next entry */
        }
    }

    /* The connect table starts after all clips */
    level->connect_table = clips + clip_byte_offset;

    printf("[LEVEL] Clips assigned, connect table at offset %d\n",
           (int)clip_byte_offset);
}
