/*
 * Alien Breed 3D I - PC Port
 * stub_io.c - File I/O with procedural test level fallback
 *
 * Attempts to load original Amiga level data from disk.
 * If not found, generates a simple procedural test level
 * so the renderer and movement can be tested.
 */

#include "stub_io.h"
#include "sb_decompress.h"
#include "renderer.h"
#include "game_types.h"
#include "sprite_palettes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

/* -----------------------------------------------------------------------
 * Data path resolution
 *
 * Game data lives in data/ at the project root.
 * We try several paths to find it.
 * ----------------------------------------------------------------------- */
static char g_data_base[512] = "";

static const char *data_base_path(void)
{
    if (g_data_base[0]) return g_data_base;

    /* Try paths relative to the working directory */
    static const char *candidates[] = {
        "",                          /* if CWD has includes/ directly */
        "data/",                     /* if CWD = project root */
        "../",                       /* if CWD = build/, data is at ../includes/ */
        "../data/",                  /* if CWD = build/ */
        "../../",                    /* if CWD = build/Release/ */
        "../../data/",              /* if CWD = build/Release/ */
        "../../../",                /* nested builds */
        "../../../data/",           /* if CWD = build/Debug/ (nested) */
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        char test[512];
        /* Check for a file that exists in the data directory */
        snprintf(test, sizeof(test), "%sincludes/floortile", candidates[i]);
        FILE *f = fopen(test, "rb");
        if (f) {
            fclose(f);
            snprintf(g_data_base, sizeof(g_data_base), "%s", candidates[i]);
            printf("[IO] Found data at: %s\n", g_data_base);
            return g_data_base;
        }
    }

    printf("[IO] WARNING: Could not locate data/ directory\n");
    return "";
}

static void make_data_path(char *buf, size_t bufsize, const char *subpath)
{
    snprintf(buf, bufsize, "%s%s", data_base_path(), subpath);
}

void io_make_data_path(char *buf, size_t bufsize, const char *subpath)
{
    snprintf(buf, bufsize, "%s%s", data_base_path(), subpath);
}

/* -----------------------------------------------------------------------
 * Big-endian write helpers (level data is Amiga format)
 * ----------------------------------------------------------------------- */
static inline void wr16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}
static inline void wr32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* -----------------------------------------------------------------------
 * Procedural test level
 *
 * Creates a small multi-room level with walls, floor, and ceiling
 * so we can test the renderer and navigation.
 *
 * Layout: 4 rooms in a 2x2 grid, connected by doorways
 *
 *   Room 0 (NW)  |  Room 1 (NE)
 *  ──────────────┼──────────────
 *   Room 2 (SW)  |  Room 3 (SE)
 *
 * Each room is 512x512 units, total level is 1024x1024.
 * Floor at y=0 (world), ceiling at y=4096 (64<<6).
 * Player starts in room 0 at (256, 256).
 * ----------------------------------------------------------------------- */

/* Zone data offsets -- matching includes/Defs.i and C code expectations.
 * The C code in movement.c/visibility.c reads WallList/ExitList as be32.
 * We write them as 32-bit absolute offsets so the C code works. */
#define ZD_ZONE_NUM       0    /* word */
#define ZD_FLOOR          2    /* long */
#define ZD_ROOF           6    /* long */
#define ZD_UPPER_FLOOR   10    /* long */
#define ZD_UPPER_ROOF    14    /* long */
#define ZD_WATER         18    /* long */
#define ZD_BRIGHTNESS    22    /* word */
#define ZD_UPPER_BRIGHT  24    /* word */
#define ZD_CPT           26    /* word */
#define ZD_WALL_LIST     28    /* word: RELATIVE offset from zone data to wall list (movement.c read_be16) */
#define ZD_EXIT_LIST     32    /* word: RELATIVE offset from zone data to exit list (movement.c read_be16) */
/* renderer.c reads byte 34 as a WORD relative offset (ToZonePts).
 * We write it as a word at byte 34 relative to zone start. */
#define ZD_PTS_REL       34    /* word: RELATIVE offset from zone data to pts list */
#define ZD_BACK          36    /* word */
#define ZD_TEL_ZONE      38    /* word: teleport destination zone (-1 = none) */
#define ZD_TEL_X         40    /* word */
#define ZD_TEL_Z         42    /* word */
/* List of graph rooms (ToListOfGraph) at 48: 8 bytes per entry (zone_id, clip_off, workspace long), -1 terminator */
#define ZD_LIST_OF_GRAPH 48
#define ZD_LIST_ENTRY_SIZE 8
#define ZD_SIZE          (ZD_LIST_OF_GRAPH + (NUM_ZONES + 1) * ZD_LIST_ENTRY_SIZE)  /* room for list + terminator */

/* Floor line data */
#define FL_X              0    /* word */
#define FL_Z              2    /* word */
#define FL_XLEN           4    /* word */
#define FL_ZLEN           6    /* word */
#define FL_CONNECT        8    /* word: connected zone or -1 */
#define FL_AWAY          10    /* byte: push-away shift */
#define FL_SIZE          16    /* bytes per floor line (must match movement.c/visibility.c) */

#define NUM_ZONES         4
#define ROOM_SIZE       512
#define FLOOR_H           0                /* floor at y=0 */
#define ROOF_H        (-(24 * 1024))      /* ceiling at -24576 (above camera in Y-down convention) */

/* Points: corners of the 4 rooms */
/*
 *   0─────1─────2
 *   │  0  │  1  │
 *   3─────4─────5
 *   │  2  │  3  │
 *   6─────7─────8
 */
#define NUM_POINTS 9

static void build_test_level_data(LevelState *level)
{
    /* Calculate buffer sizes */
    int hdr_size = 54;                      /* Level header */
    int zone_table_size = NUM_ZONES * 4;    /* Zone offset table */
    int zone_data_size = NUM_ZONES * ZD_SIZE;
    int points_size = NUM_POINTS * 4;       /* x,z pairs as words */
    /* Floor lines: 4 walls per room (16) + 4 doorways = 20 lines */
    #define NUM_FLINES 20
    int flines_size = NUM_FLINES * FL_SIZE;
    /* Combined exit+wall lists per zone (Amiga format: exits, -1, walls, -2)
     * Max 4 lines + 2 sentinels = 6 words = 12 bytes per zone */
    int combined_lists_size = NUM_ZONES * 12;
    /* Points-to-rotate list */
    int ptr_list_size = (NUM_POINTS + 1) * 2; /* indices + sentinel */
    /* Object data: just 2 player objects */
    int obj_data_size = 3 * OBJECT_SIZE; /* 2 players + 1 terminator */
    /* Object points: 2 for players + 20 for nasty_shot_data (bullets/gibs) - Amiga shares this pool */
    int num_obj_pts = 32;
    int obj_points_size = num_obj_pts * 8;

    int total = hdr_size + zone_table_size + zone_data_size + points_size +
                flines_size + combined_lists_size + ptr_list_size +
                obj_data_size + obj_points_size + 256; /* padding */

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)total);
    if (!buf) return;

    /* Point coordinates (grid corners) */
    static const int16_t pt_coords[NUM_POINTS][2] = {
        {   0,    0}, { 512,    0}, {1024,    0},  /* top row */
        {   0,  512}, { 512,  512}, {1024,  512},  /* middle row */
        {   0, 1024}, { 512, 1024}, {1024, 1024},  /* bottom row */
    };

    /* ---- Offsets ---- */
    int off_zone_table = hdr_size;
    int off_zones = off_zone_table + zone_table_size;
    int off_points = off_zones + zone_data_size;
    int off_flines = off_points + points_size;
    int off_combined = off_flines + flines_size;
    int off_ptr_list = off_combined + combined_lists_size;
    int off_obj_data = off_ptr_list + ptr_list_size;
    int off_obj_points = off_obj_data + obj_data_size;

    /* ---- Header (matches level.c parser) ---- */
    uint8_t *hdr = buf;
    wr16(hdr + 0,  256);        /* PLR1 start X */
    wr16(hdr + 2,  256);        /* PLR1 start Z */
    wr16(hdr + 4,  0);          /* PLR1 start zone */
    wr16(hdr + 6,  256);        /* PLR1 start angle */
    wr16(hdr + 8,  768);        /* PLR2 start X */
    wr16(hdr + 10, 768);        /* PLR2 start Z */
    wr16(hdr + 12, 3);          /* PLR2 start zone */
    wr16(hdr + 14, 0);          /* PLR2 start angle */
    wr16(hdr + 16, 0);          /* num control points (unused) */
    wr16(hdr + 18, NUM_POINTS); /* num points */
    wr16(hdr + 20, NUM_ZONES);  /* num zones */
    wr16(hdr + 22, NUM_FLINES); /* num floor lines */
    wr16(hdr + 24, (int16_t)num_obj_pts);  /* num object points (players + nasty shot slots) */
    wr32(hdr + 26, off_points); /* offset to points */
    wr32(hdr + 30, off_flines); /* offset to floor lines */
    wr32(hdr + 34, off_obj_data);    /* offset to object data */
    wr32(hdr + 38, 0);               /* offset to player shot data */
    wr32(hdr + 42, 0);               /* offset to nasty shot data */
    wr32(hdr + 46, off_obj_points);  /* offset to object points */
    wr32(hdr + 50, off_obj_data);    /* offset to PLR1 obj */

    /* ---- Zone offset table ---- */
    for (int z = 0; z < NUM_ZONES; z++) {
        wr32(buf + off_zone_table + z * 4, off_zones + z * ZD_SIZE);
    }

    /* ---- Zone data ---- */
    for (int z = 0; z < NUM_ZONES; z++) {
        uint8_t *zd = buf + off_zones + z * ZD_SIZE;
        wr16(zd + ZD_ZONE_NUM, (int16_t)z);
        wr32(zd + ZD_FLOOR, FLOOR_H);
        wr32(zd + ZD_ROOF, ROOF_H);
        wr32(zd + ZD_UPPER_FLOOR, 0);
        wr32(zd + ZD_UPPER_ROOF, 0);
        wr32(zd + ZD_WATER, 0);
        wr16(zd + ZD_BRIGHTNESS, 8);        /* lower floor brightness */
        wr16(zd + ZD_UPPER_BRIGHT, 8);      /* upper floor brightness */
        wr16(zd + ZD_CPT, -1);              /* no connect point */
        wr16(zd + ZD_WALL_LIST, 0);  /* unused by MoveObject (Amiga never reads it) */
        wr16(zd + ZD_EXIT_LIST, (int16_t)((off_combined + z * 12) - (off_zones + z * ZD_SIZE)));
        /* ToZonePts: relative offset from zone data to points list.
         * renderer.c reads room+34 as be16, treats as relative offset. */
        wr16(zd + ZD_PTS_REL, (int16_t)(off_ptr_list - (off_zones + z * ZD_SIZE)));
        wr16(zd + ZD_BACK, 0);
        wr16(zd + ZD_TEL_ZONE, -1);         /* no teleport */
        wr16(zd + ZD_TEL_X, 0);
        wr16(zd + ZD_TEL_Z, 0);
        /* List of graph rooms at offset 48 (Amiga ToListOfGraph). Stub: same list for all zones.
         * Real levels: list comes from level data at zone_data+48. */
        {
            uint8_t *lgr = zd + ZD_LIST_OF_GRAPH;
            for (int g = 0; g < NUM_ZONES; g++) {
                wr16(lgr + 0, (int16_t)g);
                wr16(lgr + 2, -1);
                wr32(lgr + 4, 0);
                lgr += ZD_LIST_ENTRY_SIZE;
            }
            wr16(lgr + 0, -1);
        }
    }

    /* ---- Points ---- */
    for (int i = 0; i < NUM_POINTS; i++) {
        wr16(buf + off_points + i * 4, pt_coords[i][0]);
        wr16(buf + off_points + i * 4 + 2, pt_coords[i][1]);
    }

    /* ---- Floor lines (must match rendered walls exactly)
     * Use the same room_pts and pt_coords as build_test_level_graphics so
     * collision lines are the same edges the renderer draws. */
    static const int room_pts[4][4] = {
        {0,1,4,3}, {1,2,5,4}, {3,4,7,6}, {4,5,8,7}
    };
    /* connect[zone][wall]: -1 = solid, else zone index for exit */
    static const int8_t wall_connect[4][4] = {
        {-1, 1,-1, 2}, {-1,-1,-1, 0}, { 0, 3,-1,-1}, {-1,-1,-1, 2}
    };
    int line_idx = 0;
    for (int z = 0; z < NUM_ZONES && line_idx < NUM_FLINES; z++) {
        for (int w = 0; w < 4 && line_idx < NUM_FLINES; w++) {
            int p1 = room_pts[z][w];
            int p2 = room_pts[z][(w + 1) % 4];
            int16_t x1 = (int16_t)pt_coords[p1][0];
            int16_t z1 = (int16_t)pt_coords[p1][1];
            int16_t xlen = (int16_t)(pt_coords[p2][0] - pt_coords[p1][0]);
            int16_t zlen = (int16_t)(pt_coords[p2][1] - pt_coords[p1][1]);
            int16_t connect = (int16_t)wall_connect[z][w];
            uint8_t *fl = buf + off_flines + line_idx * FL_SIZE;
            wr16(fl + FL_X, x1);
            wr16(fl + FL_Z, z1);
            wr16(fl + FL_XLEN, xlen);
            wr16(fl + FL_ZLEN, zlen);
            wr16(fl + FL_CONNECT, connect);
            fl[FL_AWAY] = 4;
            line_idx++;
        }
    }
    /* Extra exit lines (reverse direction: from_z -> to_z) */
    static const int16_t exit_pairs[4][2] = {
        {1,0}, {0,2}, {3,2}, {3,1}
    };
    for (int i = 0; i < 4 && line_idx < NUM_FLINES; i++) {
        int from_z = exit_pairs[i][0];
        int to_z   = exit_pairs[i][1];
        int w = 0;
        while (w < 4 && wall_connect[from_z][w] != to_z) w++;
        if (w < 4) {
            int p1 = room_pts[from_z][(w + 1) % 4];
            int p2 = room_pts[from_z][w];
            int16_t x1 = (int16_t)pt_coords[p1][0];
            int16_t z1 = (int16_t)pt_coords[p1][1];
            int16_t xlen = (int16_t)(pt_coords[p2][0] - pt_coords[p1][0]);
            int16_t zlen = (int16_t)(pt_coords[p2][1] - pt_coords[p1][1]);
            uint8_t *fl = buf + off_flines + line_idx * FL_SIZE;
            wr16(fl + FL_X, x1);
            wr16(fl + FL_Z, z1);
            wr16(fl + FL_XLEN, xlen);
            wr16(fl + FL_ZLEN, zlen);
            wr16(fl + FL_CONNECT, (int16_t)to_z);
            fl[FL_AWAY] = 4;
            line_idx++;
        }
    }

    /* ---- Combined exit+wall lists per zone (Amiga format) ----
     * Format: [line_indices..., -1, -2]
     * MoveObject walks this list: >= 0 → check line, -1 → skip, -2 → stop.
     * The connect field of each floor line determines exit vs wall behavior. */
    int16_t cl0[] = {0, 1, 2, 3, -1, -2};
    int16_t cl1[] = {4, 5, 6, 7, -1, -2};
    int16_t cl2[] = {8, 9, 10, 11, -1, -2};
    int16_t cl3[] = {12, 13, 14, 15, -1, -2};
    int16_t *clists[] = {cl0, cl1, cl2, cl3};

    for (int z = 0; z < NUM_ZONES; z++) {
        uint8_t *cl = buf + off_combined + z * 12;
        for (int j = 0; j < 6; j++) {
            wr16(cl + j * 2, clists[z][j]);
        }
    }

    /* ---- Points-to-rotate list ---- */
    {
        uint8_t *ptl = buf + off_ptr_list;
        for (int i = 0; i < NUM_POINTS; i++) {
            wr16(ptl + i * 2, (int16_t)i);
        }
        wr16(ptl + NUM_POINTS * 2, -1); /* sentinel */
    }

    /* ---- Object data (2 player objects + terminator) ---- */
    memset(buf + off_obj_data, 0, (size_t)obj_data_size);
    /* PLR1/PLR2 use point indices 30 and 31 so 0..19 stay free for nasty_shot_data (bullets/gibs) */
    /* PLR1 object */
    wr16(buf + off_obj_data + 0, 30);    /* collision_id = point index 30 */
    wr16(buf + off_obj_data + 12, 0);    /* zone = 0 */
    /* PLR2 object */
    wr16(buf + off_obj_data + OBJECT_SIZE + 0, 31);  /* collision_id = point index 31 */
    wr16(buf + off_obj_data + OBJECT_SIZE + 12, 3);   /* zone = 3 */
    /* Terminator object: collision_id = -1 ends the list */
    wr16(buf + off_obj_data + 2 * OBJECT_SIZE + 0, -1);
    wr16(buf + off_obj_data + 2 * OBJECT_SIZE + 12, -1); /* zone = -1 (inactive) */

    /* ---- Object points: 0..19 for bullets/gibs, 30..31 for players ---- */
    wr16(buf + off_obj_points + 30 * 8 + 0, 256);   /* PLR1 x */
    wr16(buf + off_obj_points + 30 * 8 + 4, 256);   /* PLR1 z */
    wr16(buf + off_obj_points + 31 * 8 + 0, 768);  /* PLR2 x */
    wr16(buf + off_obj_points + 31 * 8 + 4, 768);   /* PLR2 z */
    /* 0..19 zeroed by calloc - used by bullets/gibs when spawned */

    /* Store in level state - set pointers directly (bypass level_parse
     * since our test data isn't in the exact original header format) */
    level->data = buf;
    level->data_byte_count = (size_t)total;
    level->zone_adds = buf + off_zone_table;
    level->points = buf + off_points;
    level->floor_lines = buf + off_flines;
    level->object_data = buf + off_obj_data;
    level->object_points = buf + off_obj_points;
    level->plr1_obj = buf + off_obj_data;
    level->plr2_obj = buf + off_obj_data + OBJECT_SIZE;
    level->num_object_points = num_obj_pts;
    level->num_zones = NUM_ZONES;
    level->num_floor_lines = NUM_FLINES;
    level->point_brights = NULL; /* No per-point brightness for test level */

    /* Allocate player shot data (20 bullet slots for projectile weapons).
     * Each slot is OBJECT_SIZE bytes.  zone < 0 means the slot is free. */
    {
        int shot_slots = 20;
        int shot_buf_size = shot_slots * OBJECT_SIZE;
        uint8_t *shot_buf = (uint8_t *)calloc(1, (size_t)shot_buf_size);
        if (shot_buf) {
            /* Mark all slots as free (zone = -1) */
            for (int i = 0; i < shot_slots; i++) {
                wr16(shot_buf + i * OBJECT_SIZE + 12, -1); /* obj.zone = -1 */
            }
            level->player_shot_data = shot_buf;
        }
    }

    /* Allocate nasty shot data (20 enemy bullet slots + 64*20 extra) */
    {
        int nasty_shots = 20;
        int nasty_buf_size = nasty_shots * OBJECT_SIZE + 64 * 20;
        uint8_t *nasty_buf = (uint8_t *)calloc(1, (size_t)nasty_buf_size);
        if (nasty_buf) {
            for (int i = 0; i < nasty_shots; i++) {
                wr16(nasty_buf + i * OBJECT_SIZE + 12, -1);
            }
            level->nasty_shot_data = nasty_buf;
            level->other_nasty_data = nasty_buf + nasty_shots * OBJECT_SIZE;
        }
    }

    level->connect_table = NULL;
    level->water_list = NULL;

    printf("[IO] Test level: %d zones, %d points, %d lines, %d bytes\n",
           NUM_ZONES, NUM_POINTS, NUM_FLINES, total);
}

static void build_test_level_graphics(LevelState *level)
{
    /* Graphics data layout:
     *   [zone_graph_adds: NUM_ZONES * 8 bytes]  (lower gfx offset, upper gfx offset)
     *   [list_of_graph_rooms: (NUM_ZONES+1) * 8 bytes]  (zone_id, clip_off, flags, pad)
     *   [per-zone graphics: N * per_zone bytes]
     *   [reserved: 2*NUM_ZONES int16_t - was zone_bright_table; brightness now from zone data]
     */

    /* per_zone: zone_num(2) + 4 walls(4*30) + floor entry(24) + roof entry(24) + sentinel(2) */
    int per_zone = 2 + (4 * 30) + 24 + 24 + 2;
    int graph_adds_size = NUM_ZONES * 8;
    int lgr_size = (NUM_ZONES + 1) * 8; /* +1 for -1 terminator */
    int bright_table_size = 2 * NUM_ZONES * (int)sizeof(int16_t);
    int total = graph_adds_size + lgr_size + NUM_ZONES * per_zone + bright_table_size + 256;

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)total);
    if (!buf) return;

    int off_lgr = graph_adds_size;
    int off_gfx_data = off_lgr + lgr_size;
    int off_bright = off_gfx_data + NUM_ZONES * per_zone;

    /* ---- Zone graph adds (8 bytes per zone: lower gfx offset, upper gfx offset) ---- */
    for (int z = 0; z < NUM_ZONES; z++) {
        int zone_gfx_off = off_gfx_data + z * per_zone;
        wr32(buf + z * 8, zone_gfx_off);       /* lower room graphics */
        wr32(buf + z * 8 + 4, 0);              /* no upper room */
    }

    /* ---- ListOfGraphRooms (8 bytes each: zone_id, clip_off, flags, pad) ---- */
    for (int z = 0; z < NUM_ZONES; z++) {
        uint8_t *lgr = buf + off_lgr + z * 8;
        wr16(lgr + 0, (int16_t)z);    /* zone id */
        wr16(lgr + 2, -1);            /* clip_offset = -1 (no clipping) */
        wr16(lgr + 4, 0);             /* flags */
        wr16(lgr + 6, 0);             /* pad */
    }
    /* Terminator */
    wr16(buf + off_lgr + NUM_ZONES * 8, -1);

    /* Room corner point indices:
     * Room 0: 0,1,4,3   Room 1: 1,2,5,4
     * Room 2: 3,4,7,6   Room 3: 4,5,8,7 */
    static const int room_pts[4][4] = {
        {0,1,4,3}, {1,2,5,4}, {3,4,7,6}, {4,5,8,7}
    };

    /* ---- Per-zone graphics (wall polygon data) ---- */
    for (int z = 0; z < NUM_ZONES; z++) {
        uint8_t *gfx = buf + off_gfx_data + z * per_zone;
        int p = 0;

        /* Zone number */
        wr16(gfx + p, (int16_t)z); p += 2;

        /* 4 walls: type 0 (wall), 28 bytes data each */
        for (int w = 0; w < 4; w++) {
            int p1 = room_pts[z][w];
            int p2 = room_pts[z][(w + 1) % 4];

            wr16(gfx + p, 0);          p += 2; /* type = wall */
            wr16(gfx + p, (int16_t)p1);p += 2; /* point1 */
            wr16(gfx + p, (int16_t)p2);p += 2; /* point2 */
            wr16(gfx + p, 0);          p += 2; /* strip_start */
            wr16(gfx + p, 127);        p += 2; /* strip_end */
            wr16(gfx + p, 0);          p += 2; /* texture_tile */
            wr16(gfx + p, 0);          p += 2; /* totalyoff */
            wr16(gfx + p, 0);          p += 2; /* texture_id */
            gfx[p++] = 63;                     /* VALAND */
            gfx[p++] = 0;                      /* VALSHIFT */
            wr16(gfx + p, 127);        p += 2; /* HORAND */
            wr32(gfx + p, ROOF_H);     p += 4; /* topofwall */
            wr32(gfx + p, FLOOR_H);    p += 4; /* botofwall */
            wr16(gfx + p, (int16_t)(w + z * 2)); p += 2; /* brightness */
        }

        /* Floor polygon (type 1): 2+2+2+8+10 = 24 bytes after type word
         * ypos = FLOOR_H >> 6, 4-sided polygon using room corner points */
        wr16(gfx + p, 1);          p += 2; /* type = floor */
        wr16(gfx + p, (int16_t)(FLOOR_H >> 6)); p += 2; /* ypos */
        wr16(gfx + p, 3);          p += 2; /* num_sides - 1 = 3 (4 sides) */
        for (int s = 0; s < 4; s++) {
            wr16(gfx + p, (int16_t)room_pts[z][s]); p += 2;
        }
        memset(gfx + p, 0, 10);    p += 10; /* 4 bytes padding + 6 bytes extra */

        /* Roof polygon (type 2): same format as floor */
        wr16(gfx + p, 2);          p += 2; /* type = roof */
        wr16(gfx + p, (int16_t)(ROOF_H >> 6)); p += 2; /* ypos */
        wr16(gfx + p, 3);          p += 2; /* num_sides - 1 = 3 (4 sides) */
        for (int s = 0; s < 4; s++) {
            wr16(gfx + p, (int16_t)room_pts[z][s]); p += 2;
        }
        memset(gfx + p, 0, 10);    p += 10; /* 4 bytes padding + 6 bytes extra */

        /* End sentinel */
        wr16(gfx + p, -1);
    }

    /* ---- Zone brightness table (16-bit per zone) - in buffer for layout, then our own copy ---- */
    {
        int16_t *bt = (int16_t *)(buf + off_bright);
        for (int z = 0; z < NUM_ZONES; z++) {
            bt[z] = 8;                         /* Lower floor brightness */
            bt[z + NUM_ZONES] = 8;             /* Upper floor brightness */
        }
    }

    level->graphics = buf;
    level->zone_graph_adds = buf;              /* graph adds at start of buffer */
    level->list_of_graph_rooms = buf + off_lgr;

    /* Door table for test level: one door in zone 0 (wall between room 0 and 1).
     * Same format as lift: 22 bytes per entry, pos/top/bot (*256). Terminator zone_id -1. */
    {
        uint8_t *door_buf = (uint8_t *)malloc(44);
        if (door_buf) {
            int32_t closed_y = 96 * 256;  /* closed position (matches ROOF_H magnitude) */
            wr16(door_buf + 0, 0);             /* zone_id = 0 */
            wr16(door_buf + 2, 0);             /* door_type = 0 (space/switch) */
            wr32(door_buf + 4, closed_y);     /* pos = closed */
            wr16(door_buf + 8, 0);             /* door_vel = 0 */
            wr32(door_buf + 10, 0);            /* top = open position */
            wr32(door_buf + 14, closed_y);     /* bot = closed position */
            wr16(door_buf + 18, 0);            /* timer */
            wr16(door_buf + 20, 0);            /* door_flags */
            wr16(door_buf + 22, -1);          /* terminator */
            level->door_data = door_buf;
        }
    }

    printf("[IO] Graphics: zone_graph_adds@0, lgr@%d, gfx_data@%d, bright@%d\n",
           off_lgr, off_gfx_data, off_bright);
}

static void build_test_level_clips(LevelState *level)
{
    /* Minimal clips: just an empty clip list */
    int total = 256;
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)total);
    if (!buf) return;
    /* Fill with -1 sentinels */
    for (int i = 0; i < total; i += 2) {
        wr16(buf + i, -1);
    }
    level->clips = buf;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Storage for loaded wall texture data (forward declaration for io_shutdown) */
static uint8_t *g_wall_data[MAX_WALL_TILES];
/* Actual loaded size per wall (total file size) so dump can use correct pixel dimensions */
static size_t g_wall_loaded_size[MAX_WALL_TILES];

static void wall_texture_dims_from_size(int index, int pixel_size, int *out_rows, int *out_valshift);

void io_init(void)
{
    printf("[IO] init\n");
    memset(g_wall_data, 0, sizeof(g_wall_data));
    memset(g_wall_loaded_size, 0, sizeof(g_wall_loaded_size));
}

void io_shutdown(void)
{
    /* Free wall texture data */
    for (int i = 0; i < MAX_WALL_TILES; i++) {
        free(g_wall_data[i]);
        g_wall_data[i] = NULL;
        g_wall_loaded_size[i] = 0;
    }
    printf("[IO] shutdown\n");
}

int io_load_level_data(LevelState *level, int level_num)
{
    /* Try to load real level data */
    char subpath[256], path[512];
    snprintf(subpath, sizeof(subpath),
             "levels/level_%c/twolev.bin", 'a' + level_num);
    make_data_path(path, sizeof(path), subpath);

    uint8_t *data = NULL;
    size_t size = 0;
    if (sb_load_file(path, &data, &size) == 0 && data) {
        level->data = data;
        level->data_byte_count = size;
        printf("[IO] Loaded level data: %s (%zu bytes)\n", path, size);
        return 0;
    }

    /* Fallback: generate procedural test level */
    printf("[IO] Generating test level (level %d)\n", level_num);
    build_test_level_data(level);
    return 0;
}

int io_load_level_graphics(LevelState *level, int level_num)
{
    char subpath[256], path[512];
    snprintf(subpath, sizeof(subpath),
             "levels/level_%c/twolev.graph.bin", 'a' + level_num);
    make_data_path(path, sizeof(path), subpath);

    uint8_t *data = NULL;
    size_t size = 0;
    if (sb_load_file(path, &data, &size) == 0 && data) {
        level->graphics = data;
        printf("[IO] Loaded level graphics: %s (%zu bytes)\n", path, size);
        return 0;
    }

    /* Fallback */
    build_test_level_graphics(level);
    return 0;
}

int io_load_level_clips(LevelState *level, int level_num)
{
    char subpath[256], path[512];
    snprintf(subpath, sizeof(subpath),
             "levels/level_%c/twolev.clips", 'a' + level_num);
    make_data_path(path, sizeof(path), subpath);

    uint8_t *data = NULL;
    size_t size = 0;
    if (sb_load_file(path, &data, &size) == 0 && data) {
        level->clips = data;
        printf("[IO] Loaded level clips: %s (%zu bytes)\n", path, size);
        return 0;
    }

    /* Fallback */
    build_test_level_clips(level);
    return 0;
}

void io_release_level_memory(LevelState *level)
{
    /* player_shot_data and nasty_shot_data point into the data buffer
     * when loaded from real files (level_parse resolves them as offsets
     * into level->data). Only free them if they DON'T point into data. */
    if (level->player_shot_data && level->data) {
        uint8_t *d = level->data;
        if (level->player_shot_data < d || level->player_shot_data > d + 1024*1024) {
            free(level->player_shot_data);
        }
    }
    level->player_shot_data = NULL;

    if (level->nasty_shot_data && level->data) {
        uint8_t *d = level->data;
        if (level->nasty_shot_data < d || level->nasty_shot_data > d + 1024*1024) {
            free(level->nasty_shot_data);
        }
    }
    level->nasty_shot_data = NULL;
    level->other_nasty_data = NULL;

    free(level->workspace);         level->workspace = NULL;

    /* list_of_graph_rooms now points into level->data (zone_data + 48),
     * so it must NOT be freed separately. Just NULL it. */
    level->list_of_graph_rooms = NULL;

    /* Free door/switch/zone_adds tables if we allocated them (LE→BE conversion); do before freeing graphics */
    if (level->door_data_owned && level->door_data) {
        free(level->door_data);
    }
    level->door_data = NULL;
    level->door_data_owned = false;
    if (level->door_wall_list_owned) {
        free(level->door_wall_list);
        free(level->door_wall_list_offsets);
    }
    level->door_wall_list = NULL;
    level->door_wall_list_offsets = NULL;
    level->door_wall_list_owned = false;
    level->num_doors = 0;
    if (level->lift_wall_list_owned) {
        free(level->lift_wall_list);
        free(level->lift_wall_list_offsets);
    }
    level->lift_wall_list = NULL;
    level->lift_wall_list_offsets = NULL;
    level->lift_wall_list_owned = false;
    level->num_lifts = 0;
    if (level->switch_data_owned && level->switch_data) {
        free(level->switch_data);
    }
    level->switch_data = NULL;
    level->switch_data_owned = false;
    if (level->lift_data_owned && level->lift_data) {
        free(level->lift_data);
    }
    level->lift_data = NULL;
    level->lift_data_owned = false;
    if (level->zone_adds_owned && level->zone_adds) {
        free(level->zone_adds);
    }
    level->zone_adds = NULL;
    level->zone_adds_owned = false;
    free(level->data);              level->data = NULL;
    level->data_byte_count = 0;
    free(level->graphics);          level->graphics = NULL;
    free(level->clips);             level->clips = NULL;

    /* Clear remaining pointers (they pointed into the freed buffers) */
    level->lift_data = NULL;
    level->zone_graph_adds = NULL;
    level->zone_adds = NULL;
    level->points = NULL;
    level->point_brights = NULL;
    level->floor_lines = NULL;
    level->num_floor_lines = 0;
    level->object_data = NULL;
    level->object_points = NULL;
    level->plr1_obj = NULL;
    level->plr2_obj = NULL;
    level->connect_table = NULL;
    level->water_list = NULL;
}

/* Wall texture table - matches WallChunk.s wallchunkdata ordering.
 * Each entry: { filename (under includes/walls/), unpacked_size }
 * The order matches the walltiles[] index used by level graphics data. */
static const struct {
    const char *name;
    int unpacked_size;
} wall_texture_table[] = {
    { "GreenMechanic.wad",  18560 },
    { "BlueGreyMetal.wad",  13056 },
    { "TechnoDetail.wad",   13056 },
    { "BlueStone.wad",       4864 },
    { "RedAlert.wad",        7552 },
    { "rock.wad",           10368 },
    { "scummy.wad",         13056 },
    { "stairfronts.wad",     2400 },
    { "BIGDOOR.wad",        13056 },
    { "RedRock.wad",        13056 },
    { "dirt.wad",           24064 },
    { "switches.wad",        3456 },
    { "shinymetal.wad",     24064 },
    { "bluemechanic.wad",   15744 },
    { NULL, 0 }
};

void io_load_walls(void)
{
    printf("[IO] Loading wall textures...\n");

    /* Access the global renderer state to set walltiles pointers */
    extern RendererState g_renderer;

    for (int i = 0; i < MAX_WALL_TILES; i++) {
        g_wall_data[i] = NULL;
        g_wall_loaded_size[i] = 0;
        g_renderer.walltiles[i] = NULL;
        g_renderer.wall_palettes[i] = NULL;
        g_renderer.wall_valand[i] = 0;
        g_renderer.wall_valshift[i] = 0;
    }

    for (int i = 0; wall_texture_table[i].name; i++) {
        if (i >= MAX_WALL_TILES) break;

        char subpath[256], path[512];
        snprintf(subpath, sizeof(subpath), "includes/walls/%s",
                 wall_texture_table[i].name);
        make_data_path(path, sizeof(path), subpath);

        uint8_t *data = NULL;
        size_t size = 0;
        if (sb_load_file(path, &data, &size) == 0 && data) {
            g_wall_data[i] = data;
            g_wall_loaded_size[i] = size;
            /* wall_palettes points to the 2048-byte brightness LUT at the
             * START of the .wad data (ASM: PaletteAddr = walltiles[id]).
             * walltiles points past the LUT to the chunky pixel data
             * (ASM: ChunkAddr = walltiles[id] + 64*32). */
            g_renderer.wall_palettes[i] = data;
            g_renderer.walltiles[i] = (size > 2048) ? data + 2048 : data;
            if (size > 2048) {
                int pixel_size = (int)(size - 2048);
                int rows, valshift;
                wall_texture_dims_from_size(i, pixel_size, &rows, &valshift);
                g_renderer.wall_valand[i] = (uint8_t)(rows - 1);
                g_renderer.wall_valshift[i] = (uint8_t)valshift;
            }
            printf("[IO] Wall %2d: %s (%zu bytes)\n", i,
                   wall_texture_table[i].name, size);
        } else {
            g_wall_loaded_size[i] = 0;
            printf("[IO] Wall %2d: %s (not found)\n", i,
                   wall_texture_table[i].name);
        }
    }
}

void io_load_floor(void)
{
    printf("[IO] Loading floor texture...\n");

    extern RendererState g_renderer;

    char path[512];

    /* Load floor tile texture (256x256 raw 8-bit texels) */
    make_data_path(path, sizeof(path), "includes/floortile");
    printf("[IO] Trying floor tile path: %s\n", path);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        uint8_t *data = (uint8_t *)malloc((size_t)len);
        if (data) {
            fread(data, 1, (size_t)len, f);
            printf("[IO] Floor tile: %ld bytes from %s\n", len, path);
            g_renderer.floor_tile = data;
        }
        fclose(f);
    } else {
        printf("[IO] Floor tile not found: %s\n", path);
    }

    /* Load floor brightness palette (FloorPalScaled)
     * Try binary first, then fall back to parsing assembly .s file */
    make_data_path(path, sizeof(path), "includes/FloorPalScaled");
    printf("[IO] Trying palette path: %s\n", path);
    f = fopen(path, "rb");
    if (!f) {
        /* Try the .s assembly source file in data/ */
        make_data_path(path, sizeof(path), "pal/FloorPalScaled.s");
        printf("[IO] Trying palette path: %s\n", path);
        f = fopen(path, "rb");
    }
    if (!f) {
        /* Try relative to project root */
        snprintf(path, sizeof(path), "data/pal/FloorPalScaled.s");
        printf("[IO] Trying palette path: %s\n", path);
        f = fopen(path, "rb");
    }
    if (!f) {
        snprintf(path, sizeof(path), "../data/pal/FloorPalScaled.s");
        printf("[IO] Trying palette path: %s\n", path);
        f = fopen(path, "rb");
    }
    if (!f) {
        snprintf(path, sizeof(path), "../../data/pal/FloorPalScaled.s");
        printf("[IO] Trying palette path: %s\n", path);
        f = fopen(path, "rb");
    }
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *src = (char *)malloc((size_t)len + 1);
        if (src) {
            fread(src, 1, (size_t)len, f);
            src[len] = '\0';

            /* Check if it's assembly source (contains "dc.b") */
            if (strstr(src, "dc.b")) {
                /* Parse assembly: each line is " dc.b $XX,$YY" = 2 bytes */
                int capacity = 8192;
                uint8_t *data = (uint8_t *)malloc((size_t)capacity);
                if (data) {
                    int idx = 0;
                    char *p = src;
                    while ((p = strstr(p, "dc.b")) != NULL) {
                        p += 4; /* skip "dc.b" */
                        unsigned int b1 = 0, b2 = 0;
                        if (sscanf(p, " $%x,$%x", &b1, &b2) == 2) {
                            if (idx + 2 <= capacity) {
                                data[idx++] = (uint8_t)b1;
                                data[idx++] = (uint8_t)b2;
                            }
                        }
                    }
                    printf("[IO] FloorPalScaled: parsed %d bytes from %s\n", idx, path);
                    g_renderer.floor_pal = data;
                }
            } else {
                /* Binary file - use directly */
                uint8_t *data = (uint8_t *)malloc((size_t)len);
                if (data) {
                    memcpy(data, src, (size_t)len);
                    printf("[IO] FloorPalScaled: %ld bytes from %s\n", len, path);
                    g_renderer.floor_pal = data;
                }
            }
            free(src);
        }
        fclose(f);
    } else {
        printf("[IO] FloorPalScaled not found\n");
    }
}

/* Try opening a file; tries subpath then Amiga-style "disk/includes/<name>". */
static FILE *open_gun_file(const char *subpath, const char *filename, char *path_out, size_t path_size)
{
    make_data_path(path_out, path_size, subpath);
    FILE *f = fopen(path_out, "rb");
    if (f) return f;
    make_data_path(path_out, path_size, "disk/includes/");
    size_t base_len = strlen(path_out);
    if (base_len + strlen(filename) + 1 < path_size) {
        snprintf(path_out + base_len, path_size - base_len, "%s", filename);
        f = fopen(path_out, "rb");
        if (f) return f;
    }
    make_data_path(path_out, path_size, subpath);
    return NULL;
}

/* -----------------------------------------------------------------------
 * Gun overlay (newgunsinhand.wad + .ptr + .pal)
 * Required for drawing the in-hand gun; if missing, no gun is drawn.
 * ----------------------------------------------------------------------- */
#define GUN_PTR_FRAME_SIZE (96 * 4)
#define GUN_PTR_MIN_SIZE   (GUN_PTR_FRAME_SIZE * 28)

void io_load_gun_graphics(void)
{
    printf("[IO] Loading gun graphics...\n");
    extern RendererState g_renderer;

    g_renderer.gun_wad = NULL;
    g_renderer.gun_ptr = NULL;
    g_renderer.gun_pal = NULL;
    g_renderer.gun_wad_size = 0;

    char path[512];
    FILE *f;

    f = open_gun_file("includes/newgunsinhand.wad", "newgunsinhand.wad", path, sizeof(path));
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = (uint8_t *)malloc((size_t)len);
        if (data) {
            if (fread(data, 1, (size_t)len, f) == (size_t)len) {
                g_renderer.gun_wad = data;
                g_renderer.gun_wad_size = (size_t)len;
                printf("[IO] Gun WAD: %ld bytes from %s\n", len, path);
            } else {
                free(data);
            }
        }
        fclose(f);
    } else {
        printf("[IO] Gun WAD not found (tried %s and disk/includes/)\n", path);
    }

    f = open_gun_file("includes/newgunsinhand.ptr", "newgunsinhand.ptr", path, sizeof(path));
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len >= (long)GUN_PTR_MIN_SIZE) {
            uint8_t *data = (uint8_t *)malloc((size_t)len);
            if (data && fread(data, 1, (size_t)len, f) == (size_t)len) {
                g_renderer.gun_ptr = data;
                printf("[IO] Gun PTR: %ld bytes from %s\n", len, path);
            } else if (data) {
                free(data);
            }
        } else {
            printf("[IO] Gun PTR too small: %ld bytes (need %u)\n", len, (unsigned)GUN_PTR_MIN_SIZE);
        }
        fclose(f);
    } else {
        printf("[IO] Gun PTR not found (tried %s and disk/includes/)\n", path);
    }

    make_data_path(path, sizeof(path), "pal/newgunsinhand.pal");
    f = fopen(path, "rb");
    if (!f) {
        make_data_path(path, sizeof(path), "includes/newgunsinhand.pal");
        f = fopen(path, "rb");
    }
    if (!f) {
        make_data_path(path, sizeof(path), "disk/includes/newgunsinhand.pal");
        f = fopen(path, "rb");
    }
    if (f) {
        uint8_t *data = (uint8_t *)malloc(64);
        if (data && fread(data, 1, 64, f) == 64) {
            g_renderer.gun_pal = data;
            printf("[IO] Gun PAL: 64 bytes from %s\n", path);
        } else if (data) {
            free(data);
        }
        fclose(f);
    } else {
        printf("[IO] Gun PAL not found (tried pal/, includes/, disk/includes/)\n");
        /* Do not load a default palette; placeholder gun is not used. */
    }

    if (g_renderer.gun_wad && g_renderer.gun_ptr && g_renderer.gun_pal) {
        printf("[IO] Gun graphics loaded successfully\n");
    } else {
        printf("[IO] Gun graphics incomplete - in-hand gun will not be drawn (wad=%s ptr=%s pal=%s)\n",
               g_renderer.gun_wad ? "ok" : "missing",
               g_renderer.gun_ptr ? "ok" : "missing",
               g_renderer.gun_pal ? "ok" : "missing");
    }
}

/* -----------------------------------------------------------------------
 * Texture dump: write all loaded textures as BMP files into textures/
 *
 * Wall textures: 2048-byte LUT + packed pixel data (3×5-bit texels per
 * 16-bit word, vertical strips). Decoded at middle brightness to 32bpp ARGB.
 * Floor: 256×256 8-bit with 4-way interleaving, using floor_pal if loaded.
 * ----------------------------------------------------------------------- */
/* Override dimensions for textures that infer wrong (e.g. multiple exact fits). -1 = use inferred. */
static const struct { int index; int cols; int rows; } wall_dump_dim_override[] = {
    {  6, 195,  64 },  /* rock.wad: 64 rows, 65 strips (8320 bytes; inference ambiguous with 32 rows) */
    {  8, 126, 128 },  /* BIGDOOR.wad: 128 rows, 42 strips */
    { 10, 258, 128 },  /* dirt.wad: 128 rows, 86 strips (22016 bytes; inference picks 64) */
    { 11,  66,  32 },  /* switches.wad: 32 rows, 22 strips (not 33x64) */
    { 12, 258, 128 },  /* shinymetal.wad: 128 rows, 86 strips */
    { -1, 0, 0 }
};

/* Compute wall texture rows and valshift from pixel size (and overrides). Used by load and dump. */
static void wall_texture_dims_from_size(int index, int pixel_size, int *out_rows, int *out_valshift)
{
    for (int o = 0; wall_dump_dim_override[o].index >= 0; o++) {
        if (wall_dump_dim_override[o].index == index) {
            *out_rows = wall_dump_dim_override[o].rows;
            *out_valshift = (*out_rows == 128) ? 7 : (*out_rows == 64) ? 6 : (*out_rows == 32) ? 5 : (*out_rows == 16) ? 4 : 6;
            return;
        }
    }
    int valshift = -1;
    for (int vs = 6; vs >= 3; vs--) {
        int bps = (1 << vs) * 2;
        if (pixel_size % bps == 0) {
            valshift = vs;
            break;
        }
    }
    if (valshift < 0) {
        int best_strips = 0;
        for (int vs = 6; vs >= 3; vs--) {
            int bps = (1 << vs) * 2;
            int s = pixel_size / bps;
            if (s > best_strips) {
                best_strips = s;
                valshift = vs;
            }
        }
    }
    if (valshift < 0) valshift = 6;
    *out_valshift = valshift;
    *out_rows = 1 << valshift;
}

static uint32_t amiga12_to_argb(uint16_t w)
{
    uint32_t r4 = (w >> 8) & 0xF;
    uint32_t g4 = (w >> 4) & 0xF;
    uint32_t b4 = w & 0xF;
    return 0xFF000000u | (r4 * 0x11u << 16) | (g4 * 0x11u << 8) | (b4 * 0x11u);
}

static int write_bmp(const char *path, int width, int height, const uint32_t *argb)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int row_bytes = width * 4;
    int pad = (4 - (row_bytes & 3)) & 3;
    int row_stride = row_bytes + pad;
    int image_size = row_stride * height;
    int file_size = 14 + 40 + image_size;

    /* BMP file header (14 bytes) */
    uint8_t fh[14] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0  /* pixel data offset */
    };
    fwrite(fh, 1, 14, f);

    /* DIB header (40 bytes) */
    uint8_t ih[40] = { 0 };
    ih[0] = 40;  /* header size */
    *(int32_t*)(ih + 4) = width;
    *(int32_t*)(ih + 8) = height;
    ih[12] = 1; ih[13] = 0;  /* planes */
    ih[14] = 32; ih[15] = 0; /* bits per pixel */
    *(int32_t*)(ih + 16) = 0;  /* compression */
    *(int32_t*)(ih + 20) = image_size;
    fwrite(ih, 1, 40, f);

    /* Pixels: BMP is bottom-up, 32bpp stored as BGRA */
    uint8_t pad_buf[4] = { 0, 0, 0, 0 };
    for (int y = height - 1; y >= 0; y--) {
        const uint32_t *row = argb + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            uint32_t c = row[x];
            uint8_t b = (uint8_t)(c);
            uint8_t g = (uint8_t)(c >> 8);
            uint8_t r = (uint8_t)(c >> 16);
            uint8_t a = (uint8_t)(c >> 24);
            fputc(b, f); fputc(g, f); fputc(r, f); fputc(a, f);
        }
        fwrite(pad_buf, 1, pad, f);
    }

    fclose(f);
    return 0;
}

void io_dump_textures(void)
{
    extern RendererState g_renderer;
    MKDIR("textures");

    const int lut_brightness = 16;  /* middle brightness block */
    const int lut_block_off = lut_brightness * 64;

    for (int i = 0; wall_texture_table[i].name != NULL && i < MAX_WALL_TILES; i++) {
        const uint8_t *pal = g_renderer.wall_palettes[i];
        const uint8_t *tex = g_renderer.walltiles[i];
        if (!pal || !tex) continue;

        /* Use actual loaded size when available so dimensions match the real data. */
        int pixel_size;
        if (g_wall_loaded_size[i] > 2048)
            pixel_size = (int)(g_wall_loaded_size[i] - 2048);
        else
            pixel_size = wall_texture_table[i].unpacked_size - 2048;
        if (pixel_size <= 0) continue;

        int rows, valshift;
        wall_texture_dims_from_size(i, pixel_size, &rows, &valshift);
        int bytes_per_strip = (1 << valshift) * 2;
        int strips = pixel_size / bytes_per_strip;
        int cols = strips * 3;

        uint32_t *argb = (uint32_t *)malloc((size_t)cols * rows * sizeof(uint32_t));
        if (!argb) continue;

        for (int tex_col = 0; tex_col < cols; tex_col++) {
            int strip_index = tex_col / 3;
            int pack_mode = tex_col % 3;
            int strip_offset = strip_index << (valshift + 1);

            for (int ty = 0; ty < rows; ty++) {
                int byte_off = strip_offset + ty * 2;
                if (byte_off + 2 > pixel_size) break;

                uint16_t word = ((uint16_t)tex[byte_off] << 8) | (uint16_t)tex[byte_off + 1];

                uint8_t texel5;
                switch (pack_mode) {
                case 0:  texel5 = (uint8_t)(word & 31);         break;
                case 1:  texel5 = (uint8_t)((word >> 5) & 31);  break;
                default: texel5 = (uint8_t)((word >> 10) & 31); break;
                }

                int lut_off = lut_block_off + texel5 * 2;
                uint16_t color_word = ((uint16_t)pal[lut_off] << 8) | pal[lut_off + 1];
                argb[ty * cols + tex_col] = amiga12_to_argb(color_word);
            }
        }

        char path[256];
        snprintf(path, sizeof(path), "textures/wall_%02d.bmp", i);
        if (write_bmp(path, cols, rows, argb) == 0) {
            printf("[IO] Dumped %s (%dx%d)\n", path, cols, rows);
        }
        free(argb);
    }

    /* Floor texture: 64×64 per tile (game uses U,V & 63), index = ((tv<<8)|tu)*4 */
    if (g_renderer.floor_tile) {
        const uint8_t *tex = g_renderer.floor_tile;
        const uint8_t *pal = g_renderer.floor_pal;
        int pal_level = 7;
        if (pal_level > 14) pal_level = 14;
        const uint8_t *lut = pal ? pal + pal_level * 512 : NULL;

        uint32_t *argb = (uint32_t *)malloc(64 * 64 * sizeof(uint32_t));
        if (argb) {
            for (int v = 0; v < 64; v++) {
                for (int u = 0; u < 64; u++) {
                    int idx = ((v << 8) | u) * 4;
                    uint8_t texel = tex[idx];
                    uint32_t c;
                    if (lut) {
                        uint16_t cw = (uint16_t)((lut[texel * 2] << 8) | lut[texel * 2 + 1]);
                        c = amiga12_to_argb(cw);
                    } else {
                        uint32_t g = (uint32_t)texel * 0x010101u;
                        c = 0xFF000000u | g;
                    }
                    argb[v * 64 + u] = c;
                }
            }
            if (write_bmp("textures/floor.bmp", 64, 64, argb) == 0) {
                printf("[IO] Dumped textures/floor.bmp (64x64)\n");
            }
            free(argb);
        }
    }

    printf("[IO] Texture dump complete. Check the textures/ folder.\n");
}

/* Object sprite files (ObjDraw3.ChipRam.s Objects, LoadFromDisk.s OBJ_NAMES).
 * Index = objVectNumber (object data offset 8). NULL = no file / use placeholder.
 *
 * Amiga format: each object type has 3 files:
 *   .wad = packed pixel data (3 five-bit pixels per 16-bit word)
 *   .ptr = column pointer table (4 bytes per column)
 *   .pal = brightness-graded palette (15 levels * 32 colors * 2 bytes) */
static const char *sprite_wad_names[MAX_SPRITE_TYPES] = {
    "ALIEN2.wad",           /* 0 */
    "PICKUPS.wad",          /* 1 */
    "bigbullet.wad",        /* 2 */
    NULL,                   /* 3 ugly monster (missing) */
    "flyingalien.wad",      /* 4 */
    "keys.wad",             /* 5 */
    "rockets.wad",          /* 6 */
    "barrel.wad",           /* 7 */
    "explosion.wad",        /* 8 */
    "newgunsinhand.wad",    /* 9 */
    "newmarine.wad",        /* 10 */
    "BIGSCARYALIEN.wad",    /* 11 */
    "lamps.wad",            /* 12 */
    "worm.wad",             /* 13 */
    "bigclaws.wad",         /* 14 */
    "tree.wad",             /* 15 */
    "newmarine.wad",        /* 16 tough marine (shared with 10) */
    "newmarine.wad",        /* 17 flame marine (shared with 10) */
    NULL, NULL
};

static const char *sprite_ptr_names[MAX_SPRITE_TYPES] = {
    "ALIEN2.ptr",           /* 0 */
    "PICKUPS.ptr",          /* 1 */
    "bigbullet.ptr",        /* 2 */
    NULL,                   /* 3 */
    "flyingalien.ptr",      /* 4 */
    "keys.ptr",             /* 5 */
    "rockets.ptr",          /* 6 */
    "barrel.ptr",           /* 7 */
    "explosion.ptr",        /* 8 */
    "newgunsinhand.ptr",    /* 9 */
    "newmarine.ptr",        /* 10 */
    "BIGSCARYALIEN.ptr",    /* 11 */
    "lamps.ptr",            /* 12 */
    "worm.ptr",             /* 13 */
    "bigclaws.ptr",         /* 14 */
    "tree.ptr",             /* 15 */
    "newmarine.ptr",        /* 16 */
    "newmarine.ptr",        /* 17 */
    NULL, NULL
};

static const char *sprite_pal_names[MAX_SPRITE_TYPES] = {
    "alien2.pal",           /* 0 */
    "PICKUPS.PAL",          /* 1 */
    "bigbullet.pal",        /* 2 */
    NULL,                   /* 3 */
    "FLYINGalien.pal",      /* 4 */
    "keys.pal",             /* 5 */
    "ROCKETS.pal",          /* 6 */
    "BARREL.pal",           /* 7 */
    "explosion.pal",        /* 8 */
    "newgunsinhand.pal",    /* 9 */
    "newmarine.pal",        /* 10 */
    "BIGSCARYALIEN.pal",    /* 11 */
    "LAMPS.pal",            /* 12 */
    "worm.pal",             /* 13 */
    "bigclaws.pal",         /* 14 */
    "tree.pal",             /* 15 */
    "toughmutant.pal",      /* 16 */
    "flamemutant.pal",      /* 17 */
    NULL, NULL
};

static uint8_t *g_sprite_data[MAX_SPRITE_TYPES];
static uint8_t *g_sprite_ptr_data[MAX_SPRITE_TYPES];
static uint8_t *g_sprite_pal_store[MAX_SPRITE_TYPES];

/* Try loading a file from includes/ or pal/ directories. */
static int load_sprite_file(const char *name, const char *prefix,
                            uint8_t **out_data, size_t *out_size)
{
    char subpath[256], path[512];
    *out_data = NULL;
    *out_size = 0;

    /* Try includes/<name> */
    snprintf(subpath, sizeof(subpath), "includes/%s", name);
    make_data_path(path, sizeof(path), subpath);
    if (sb_load_file(path, out_data, out_size) == 0 && *out_data && *out_size > 0)
        return 1;

    /* Try <prefix>/<name> (e.g. pal/alien2.pal) */
    if (prefix) {
        snprintf(subpath, sizeof(subpath), "%s/%s", prefix, name);
        make_data_path(path, sizeof(path), subpath);
        if (sb_load_file(path, out_data, out_size) == 0 && *out_data && *out_size > 0)
            return 1;
    }

    /* Try disk/includes/<name> */
    snprintf(subpath, sizeof(subpath), "disk/includes/%s", name);
    make_data_path(path, sizeof(path), subpath);
    if (sb_load_file(path, out_data, out_size) == 0 && *out_data && *out_size > 0)
        return 1;

    return 0;
}

void io_load_objects(void)
{
    printf("[IO] Loading object sprites...\n");
    extern RendererState g_renderer;

    for (int i = 0; i < MAX_SPRITE_TYPES; i++) {
        free(g_sprite_data[i]);       g_sprite_data[i] = NULL;
        free(g_sprite_ptr_data[i]);   g_sprite_ptr_data[i] = NULL;
        free(g_sprite_pal_store[i]);  g_sprite_pal_store[i] = NULL;
        g_renderer.sprite_wad[i] = NULL;  g_renderer.sprite_wad_size[i] = 0;
        g_renderer.sprite_ptr[i] = NULL;  g_renderer.sprite_ptr_size[i] = 0;
        g_renderer.sprite_pal_data[i] = NULL; g_renderer.sprite_pal_size[i] = 0;
    }

    for (int i = 0; i < MAX_SPRITE_TYPES; i++) {
        /* Load .wad (packed pixel data) */
        if (sprite_wad_names[i]) {
            uint8_t *data = NULL; size_t sz = 0;
            if (load_sprite_file(sprite_wad_names[i], NULL, &data, &sz)) {
                g_sprite_data[i] = data;
                g_renderer.sprite_wad[i] = data;
                g_renderer.sprite_wad_size[i] = sz;
                printf("[IO] Sprite %2d WAD: %s (%zu bytes)\n", i, sprite_wad_names[i], sz);
            } else {
                printf("[IO] Sprite %2d WAD: %s (not found)\n", i, sprite_wad_names[i]);
            }
        }

        /* Load .ptr (column pointer table) */
        if (sprite_ptr_names[i]) {
            uint8_t *data = NULL; size_t sz = 0;
            if (load_sprite_file(sprite_ptr_names[i], NULL, &data, &sz)) {
                g_sprite_ptr_data[i] = data;
                g_renderer.sprite_ptr[i] = data;
                g_renderer.sprite_ptr_size[i] = sz;
                printf("[IO] Sprite %2d PTR: %s (%zu bytes)\n", i, sprite_ptr_names[i], sz);
            }
        }

        /* .pal: embedded tables only (from data/pal via sprite_palettes_data.h). No runtime load. */
        if (sprite_pal_embedded_size[i] > 0 && sprite_pal_embedded[i] != NULL) {
            g_renderer.sprite_pal_data[i] = sprite_pal_embedded[i];
            g_renderer.sprite_pal_size[i] = sprite_pal_embedded_size[i];
            printf("[IO] Sprite %2d PAL: table (%zu bytes)\n", i, sprite_pal_embedded_size[i]);
        }
    }
}
void io_load_sfx(void)     { printf("[IO] load_sfx (stub)\n"); }
void io_load_panel(void)   { printf("[IO] load_panel (stub)\n"); }

void io_load_prefs(char *prefs_buf, int buf_size)
{
    (void)prefs_buf; (void)buf_size;
}
void io_save_prefs(const char *prefs_buf, int buf_size)
{
    (void)prefs_buf; (void)buf_size;
}
void io_load_passwords(void) { }
void io_save_passwords(void) { }
