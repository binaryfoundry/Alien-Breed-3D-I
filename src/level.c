/*
 * Alien Breed 3D I - PC Port
 * level.c - Level data parsing
 *
 * Translated from: AB3DI.s blag: section (~line 722-848)
 */

#include "level.h"
#include <stdio.h>
#include <stdlib.h>
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

static void write_word_be(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v >> 8);
    p[1] = (uint8_t)(uint16_t)v;
}
static void write_long_be(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)((uint32_t)v >> 24);
    p[1] = (uint8_t)((uint32_t)v >> 16);
    p[2] = (uint8_t)((uint32_t)v >> 8);
    p[3] = (uint8_t)(uint32_t)v;
}

/* Amiga door/lift format (Anims.s): 18-byte fixed header then variable wall list.
 * Header: Bottom(w), Top(w), curr(w), dir(w), Ptr(l), zone(w), conditions(w), 2 bytes at 16-17.
 * Wall list at 18: (wall_number(w), ptr(l), graphic(l)) until wall_number < 0, then +2.
 */
static const uint8_t *skip_amiga_wall_list(const uint8_t *p)
{
    while (read_word(p) >= 0)
        p += 2 + 4 + 4;
    return p + 2;
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

    /* ---- Graphics data header (LEVELGRAPHICS) ----
     * Byte  0-3:  door_offset   (long)  offset from lg to door table; 0 = no doors
     * Byte  4-7:  lift_offset   (long)  offset to lift table
     * Byte  8-11: switch_offset (long)  offset to switch table
     * Byte 12-15: zone_graph_offset (long)  offset to zone graph adds
     * Byte 16+:   zone_adds     (num_zones * 4 bytes)  each long = offset into LEVELDATA to that zone's zone data
     *
     * Door table (at lg + door_offset). Entries 16 bytes each, terminated by zone_id < 0.
     *   0-1:  zone_id (int16)   zone this door affects (roof height written here)
     *   2-3:  door_type (int16) 0=space/switch, 1=cond 0x900, 2=0x400, 3=0x200, 4=always open, 5=never
     *   4-7:  door_pos (int32)  current opening (0=open, door_max*256=closed)
     *   8-9:  door_vel (int16)  open/close speed
     *  10-11: door_max (int16)  max opening height (logical units * 256 when closed)
     *  12-13: timer (int16)     close delay
     *  14-15: door_flags (uint16) for type 0: condition bit mask; 0 = any switch opens this door
     *
     * Switch table (at lg + switch_offset). Entries 14 bytes each, terminated by zone_id < 0.
     *   0-1:  zone_id (int16)   zone the switch is in
     *   2-3:  (reserved; byte 3 used as cooldown)
     *   4-5:  bit_mask (uint16) condition bit toggled when pressed; match door_flags for type 0 doors
     *   6-9:  gfx_offset (long) offset into lg to switch wall's first word (for on/off patch)
     *  10-11: sw_x (int16)     switch position X (for facing check)
     *  12-13: sw_z (int16)     switch position Z
     *
     * Note: The procedural test level (stub) does not use this header and leaves door_data/switch_data
     * NULL. Use a real level file (e.g. levels/level_a/twolev.graph.bin) with non-zero door_offset
     * and switch_offset to test doors and switches.
     */
    /* Graphics header: all longs big-endian */
    int32_t door_offset = read_long(lg + 0);
    int32_t lift_offset = read_long(lg + 4);
    int32_t switch_offset = read_long(lg + 8);
    int32_t zone_graph_offset = read_long(lg + 12);
    printf("[LEVEL] Graphics header: door=%ld lift=%ld switch=%ld zone_graph=%ld\n",
           (long)door_offset, (long)lift_offset, (long)switch_offset, (long)zone_graph_offset);
    level->door_data_owned = false;
    level->switch_data_owned = false;
    level->lift_data_owned = false;

    int num_zones = (int)read_word(ld + 16);
    if (num_zones <= 0) num_zones = 256;

    /* Doors: Amiga format (match standalone). 999 terminator; 18-byte header + variable wall list. */
    if (door_offset <= 16) {
        level->door_data = NULL;
    } else {
        const uint8_t *door_src = lg + door_offset;
        int16_t first_w = read_word(door_src);
        if (first_w == 999) {
            level->door_data = NULL;
        } else {
            int nd = 0;
            const uint8_t *d = door_src;
            while (read_word(d) != 999) {
                nd++;
                d = skip_amiga_wall_list(d + 18);
                if (nd > 256) break;
            }
            int16_t zone0 = read_word(door_src + 12);
            if (nd > 0 && nd <= 256 && zone0 >= 0 && zone0 < num_zones) {
                uint8_t *buf = (uint8_t *)malloc((size_t)(nd + 1) * 16u);
                if (buf) {
                    const uint8_t *s = door_src;
                    int out_idx = 0;
                    for (int i = 0; i < nd; i++) {
                        int16_t bottom = read_word(s + 0), top = read_word(s + 2), curr = read_word(s + 4), dir = read_word(s + 6);
                        int16_t zone = read_word(s + 12);
                        int16_t cond = read_word(s + 14);
                        if (zone >= 0 && zone < num_zones) {
                            int16_t range = (int16_t)(top - bottom);
                            if (range < 0) range = 0;
                            int32_t pos = (int32_t)(curr - bottom) * 256;
                            if (pos < 0) pos = 0;
                            if (pos > (int32_t)range * 256) pos = (int32_t)range * 256;
                            uint8_t *t = buf + out_idx * 16;
                            write_word_be(t + 0, zone);
                            write_word_be(t + 2, (int16_t)0);
                            write_long_be(t + 4, pos);
                            write_word_be(t + 8, dir);
                            write_word_be(t + 10, range);
                            write_word_be(t + 12, (int16_t)0);
                            write_word_be(t + 14, cond);
                            out_idx++;
                        }
                        s = skip_amiga_wall_list(s + 18);
                    }
                    write_word_be(buf + out_idx * 16, (int16_t)-1);
                    level->door_data = buf;
                    level->door_data_owned = true;
                } else {
                    level->door_data = NULL;
                }
            } else {
                level->door_data = NULL;
            }
        }
    }

    /* Long 4: Offset to lifts - Amiga format (match standalone): 999 terminator, 18-byte header + variable wall list */
    if (lift_offset <= 16) {
        level->lift_data = NULL;
    } else {
        const uint8_t *lift_src = lg + lift_offset;
        int16_t first_w = read_word(lift_src);
        if (first_w == 999) {
            level->lift_data = NULL;
        } else {
            int nl = 0;
            const uint8_t *d = lift_src;
            while (read_word(d) != 999) {
                nl++;
                d = skip_amiga_wall_list(d + 18);
                if (nl > 256) break;
            }
            int16_t zone0 = read_word(lift_src + 12);
            if (nl > 0 && nl <= 256 && zone0 >= 0 && zone0 < num_zones) {
                uint8_t *buf = (uint8_t *)malloc((size_t)(nl + 1) * 20u);
                if (buf) {
                    const uint8_t *s = lift_src;
                    int out_idx = 0;
                    for (int i = 0; i < nl; i++) {
                        int16_t bottom = read_word(s + 0), top = read_word(s + 2), curr = read_word(s + 4), dir = read_word(s + 6);
                        int16_t zone = read_word(s + 12);
                        if (zone >= 0 && zone < num_zones) {
                            uint8_t *t = buf + out_idx * 20;
                            write_word_be(t + 0, zone);
                            write_word_be(t + 2, (int16_t)0);  /* type 0 = space key */
                            write_long_be(t + 4, (int32_t)curr * 256);
                            write_word_be(t + 8, dir);
                            write_long_be(t + 10, (int32_t)bottom * 256);  /* lift_top = low position */
                            write_long_be(t + 14, (int32_t)top * 256);     /* lift_bot = high position */
                            write_word_be(t + 18, (int16_t)0);  /* padding to 20 bytes */
                            out_idx++;
                        }
                        s = skip_amiga_wall_list(s + 18);
                    }
                    write_word_be(buf + out_idx * 20, (int16_t)-1);
                    level->lift_data = buf;
                    level->lift_data_owned = true;
                } else {
                    level->lift_data = NULL;
                }
            } else {
                level->lift_data = NULL;
            }
        }
    }

    /* Long 8: Offset to switches - 14 bytes per entry (match standalone), zone at 0, zone < 0 = end. Big-endian. */
    if (switch_offset > 16) {
        const uint8_t *sw_src = lg + switch_offset;
        int16_t zone_id = read_word(sw_src);
        if (zone_id < 0)
            level->switch_data = NULL;
        else
            level->switch_data = (uint8_t *)(lg + switch_offset);
    } else {
        level->switch_data = NULL;
    }

    /* Long 12: Offset to zone graph adds */
    level->zone_graph_adds = lg + zone_graph_offset;

    /* Zone offset table starts at byte 16 of graphics data. Assume big-endian; convert if clearly LE. */
    level->zone_adds = lg + 16;
    level->zone_adds_owned = false;
    level->zone_brightness_le = false;

    /* ---- Level data header ---- */
    /* Byte 14: Number of points (word) */
    int16_t num_points = read_word(ld + 14);

    /* Byte 16: Number of zones (word) */
    level->num_zones = read_word(ld + 16);

    /* Zone offset table at lg+16 is big-endian. Log each zone's loaded data. */
    if (level->num_zones > 0) {
        printf("[LEVEL] Zones: %d zones (offset table big-endian)\n", level->num_zones);
        for (int z = 0; z < level->num_zones; z++) {
            int32_t zoff = read_long(level->zone_adds + z * 4);
            size_t data_len = level->data_byte_count;
            if (zoff < 0 || (data_len != 0 && (size_t)zoff + 48u > data_len)) {
                printf("[LEVEL]   zone[%d] offset %ld - out of range (data_len=%zu)\n", z, (long)zoff, data_len);
                continue;
            }
            const uint8_t *zd = ld + zoff;
            int16_t zone_id = read_word(zd + 0);
            int32_t floor_y = read_long(zd + ZONE_OFF_FLOOR);
            int32_t roof_y = read_long(zd + ZONE_OFF_ROOF);
            int16_t bright_lo = read_word(zd + ZONE_OFF_BRIGHTNESS);
            int16_t bright_hi = read_word(zd + ZONE_OFF_UPPER_BRIGHT);
            printf("[LEVEL]   zone[%d] offset %ld id=%d floor=%ld roof=%ld bright=(%d,%d)\n",
                   z, (long)zoff, (int)zone_id, (long)floor_y, (long)roof_y, (int)bright_lo, (int)bright_hi);
        }
    }

    /* Save original zone roof for each zone so door_routine can write base_roof + door_delta. */
    if (level->num_zones > 0) {
        level->zone_base_roof = (int32_t *)malloc((size_t)level->num_zones * sizeof(int32_t));
        if (level->zone_base_roof) {
            size_t data_len = level->data_byte_count;
            for (int z = 0; z < level->num_zones; z++) {
                int32_t zoff = read_long(level->zone_adds + (size_t)z * 4u);
                if (zoff >= 0 && (data_len == 0 || (size_t)zoff + 10u <= data_len))
                    level->zone_base_roof[z] = read_long(ld + zoff + ZONE_OFF_ROOF);
                else
                    level->zone_base_roof[z] = 0;
            }
        } else {
            level->zone_base_roof = NULL;
        }
        level->zone_base_floor = (int32_t *)malloc((size_t)level->num_zones * sizeof(int32_t));
        if (level->zone_base_floor) {
            size_t data_len = level->data_byte_count;
            for (int z = 0; z < level->num_zones; z++) {
                int32_t zoff = read_long(level->zone_adds + (size_t)z * 4u);
                if (zoff >= 0 && (data_len == 0 || (size_t)zoff + 10u <= data_len))
                    level->zone_base_floor[z] = read_long(ld + zoff + ZONE_OFF_FLOOR);
                else
                    level->zone_base_floor[z] = 0;
            }
        } else {
            level->zone_base_floor = NULL;
        }
    } else {
        level->zone_base_roof = NULL;
        level->zone_base_floor = NULL;
    }

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

    /* Debug: dump doors and switches (door_flags / bit_mask must match for switch-to-door link) */
    if (level->door_data) {
        const uint8_t *door = level->door_data;
        int di = 0;
        while (1) {
            int16_t zone_id = read_word(door);
            if (zone_id < 0) break;
            int16_t door_type = read_word(door + 2);
            int32_t door_pos = read_long(door + 4);
            int16_t door_max = read_word(door + 10);
            uint16_t door_flags = (uint16_t)read_word(door + 14);
            printf("[LEVEL] door[%d] zone=%d type=%d pos=%ld max=%d flags=0x%04X (%u)\n",
                   di, (int)zone_id, (int)door_type, (long)door_pos, (int)door_max, door_flags, door_flags);
            door += 16;
            di++;
        }
    }
    if (level->switch_data) {
        const uint8_t *sw = level->switch_data;
        int si = 0;
        while (1) {
            int16_t zone_id = read_word(sw);
            if (zone_id < 0) break;
            uint16_t bit_mask = (uint16_t)read_word(sw + 4);
            int16_t sw_x = read_word(sw + 10);
            int16_t sw_z = read_word(sw + 12);
            printf("[LEVEL] switch[%d] zone=%d bit_mask=0x%04X (%u) pos=(%d,%d)\n",
                   si, (int)zone_id, bit_mask, bit_mask, (int)sw_x, (int)sw_z);
            sw += 14;
            si++;
        }
    }
    if (level->lift_data) {
        const uint8_t *lift = level->lift_data;
        int li = 0;
        while (1) {
            int16_t zone_id = read_word(lift);
            if (zone_id < 0) break;
            int16_t lift_type = read_word(lift + 2);
            int32_t lift_pos = read_long(lift + 4);
            int16_t lift_vel = read_word(lift + 8);
            int32_t lift_top = read_long(lift + 10);
            int32_t lift_bot = read_long(lift + 14);
            printf("[LEVEL] lift[%d] zone=%d type=%d pos=%ld vel=%d top=%ld bot=%ld\n",
                   li, (int)zone_id, (int)lift_type, (long)lift_pos, (int)lift_vel, (long)lift_top, (long)lift_bot);
            lift += 20;
            li++;
        }
    }

    /* Log zones with animated (flashing) brightness (anim 1=pulse, 2=flicker, 3=fire). Big-endian. */
    if (level->zone_adds && level->data && level->num_zones > 0) {
        int32_t max_zoff = 0x00FFFFFF;
        if (level->data_byte_count > 32)
            max_zoff = (int32_t)(level->data_byte_count - 32);
        int flashing_count = 0;
        for (int z = 0; z < level->num_zones; z++) {
            int32_t zoff = read_long(level->zone_adds + z * 4);
            if (zoff < 0 || zoff > max_zoff) continue;
            const uint8_t *zd = ld + zoff;
            int16_t bright_lo = (int16_t)((zd[ZONE_OFF_BRIGHTNESS  ] << 8) | zd[ZONE_OFF_BRIGHTNESS  + 1]);
            int16_t bright_hi = (int16_t)((zd[ZONE_OFF_UPPER_BRIGHT] << 8) | zd[ZONE_OFF_UPPER_BRIGHT + 1]);
            unsigned hi_lo = ((unsigned)((uint16_t)bright_lo >> 8) & 0xFFu), lo_lo = ((uint16_t)bright_lo & 0xFFu);
            unsigned hi_hi = ((unsigned)((uint16_t)bright_hi >> 8) & 0xFFu), lo_hi = ((uint16_t)bright_hi & 0xFFu);
            unsigned anim_lo = (hi_lo >= 1u && hi_lo <= 3u) ? hi_lo : (lo_lo >= 1u && lo_lo <= 3u) ? lo_lo : 0;
            unsigned anim_hi = (hi_hi >= 1u && hi_hi <= 3u) ? hi_hi : (lo_hi >= 1u && lo_hi <= 3u) ? lo_hi : 0;
            if (anim_lo != 0 || anim_hi != 0) {
                const char *name_lo = (anim_lo == 1) ? "pulse" : (anim_lo == 2) ? "flicker" : (anim_lo == 3) ? "fire" : "static";
                const char *name_hi = (anim_hi == 1) ? "pulse" : (anim_hi == 2) ? "flicker" : (anim_hi == 3) ? "fire" : "static";
                printf("[LEVEL] zone[%d] flashing: lower=%s (0x%04X) upper=%s (0x%04X)\n",
                       z, name_lo, (unsigned)(uint16_t)bright_lo, name_hi, (unsigned)(uint16_t)bright_hi);
                flashing_count++;
            }
        }
        printf("[LEVEL] %d zone(s) with animated (flashing) brightness\n", flashing_count);
    }

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

int level_get_zone_info(const LevelState *level, int16_t zone_id, ZoneInfo *out)
{
    if (!out || !level->zone_adds || !level->data || level->num_zones <= 0)
        return -1;
    if (zone_id < 0 || zone_id >= level->num_zones)
        return -1;
    int32_t zoff = read_long(level->zone_adds + (size_t)zone_id * 4u);
    size_t data_len = level->data_byte_count;
    if (zoff < 0 || (data_len != 0 && (size_t)zoff + 48u > data_len))
        return -1;
    const uint8_t *zd = level->data + zoff;
    out->zone_id = read_word(zd + 0);
    out->floor_y = read_long(zd + ZONE_OFF_FLOOR);
    out->roof_y = read_long(zd + ZONE_OFF_ROOF);
    out->upper_floor_y = read_long(zd + ZONE_OFF_UPPER_FLOOR);
    out->upper_roof_y = read_long(zd + ZONE_OFF_UPPER_ROOF);
    out->water_y = read_long(zd + ZONE_OFF_WATER);
    out->brightness = read_word(zd + ZONE_OFF_BRIGHTNESS);
    out->upper_brightness = read_word(zd + ZONE_OFF_UPPER_BRIGHT);
    out->tel_zone = read_word(zd + ZONE_OFF_TEL_ZONE);
    out->tel_x = read_word(zd + ZONE_OFF_TEL_X);
    out->tel_z = read_word(zd + ZONE_OFF_TEL_Z);
    return 0;
}

uint8_t *level_get_zone_data_ptr(LevelState *level, int16_t zone_id)
{
    if (!level->zone_adds || !level->data || level->num_zones <= 0)
        return NULL;
    if (zone_id < 0 || zone_id >= level->num_zones)
        return NULL;
    int32_t zoff = read_long(level->zone_adds + (size_t)zone_id * 4u);
    size_t data_len = level->data_byte_count;
    if (zoff < 0 || (data_len != 0 && (size_t)zoff + 48u > data_len))
        return NULL;

    return level->data + zoff;
}

static inline int16_t swap16(int16_t v)
{
    return (int16_t)(((uint16_t)v >> 8) | ((uint16_t)v << 8));
}
static inline int32_t swap32(int32_t v)
{
    return (int32_t)(((uint32_t)v >> 24) | (((uint32_t)v >> 8) & 0xFF00u) |
                     (((uint32_t)v << 8) & 0xFF0000u) | ((uint32_t)v << 24));
}

ZoneInfo zone_info_swap_endianness(const ZoneInfo *z)
{
    ZoneInfo out = {0};
    if (!z) return out;
    out.zone_id = swap16(z->zone_id);
    out.floor_y = swap32(z->floor_y);
    out.roof_y = swap32(z->roof_y);
    out.upper_floor_y = swap32(z->upper_floor_y);
    out.upper_roof_y = swap32(z->upper_roof_y);
    out.water_y = swap32(z->water_y);
    out.brightness = swap16(z->brightness);
    out.upper_brightness = swap16(z->upper_brightness);
    out.tel_zone = swap16(z->tel_zone);
    out.tel_x = swap16(z->tel_x);
    out.tel_z = swap16(z->tel_z);
    return out;
}

void level_log_zones(const LevelState *level)
{
    if (!level->zone_adds || !level->data || level->num_zones <= 0)
        return;
    const uint8_t *ld = level->data;
    size_t data_len = level->data_byte_count;
    printf("[LEVEL] Zones: %d zones (offset table big-endian)\n", level->num_zones);
    for (int z = 0; z < level->num_zones; z++) {
        int32_t zoff = read_long(level->zone_adds + (size_t)z * 4u);
        if (zoff < 0 || (data_len != 0 && (size_t)zoff + 48u > data_len)) {
            printf("[LEVEL]   zone[%d] offset %ld - out of range (data_len=%zu)\n", z, (long)zoff, data_len);
            continue;
        }
        const uint8_t *zd = ld + zoff;
        int16_t zone_id = read_word(zd + 0);
        int32_t floor_y = read_long(zd + ZONE_OFF_FLOOR);
        int32_t roof_y = read_long(zd + ZONE_OFF_ROOF);
        int16_t bright_lo = read_word(zd + ZONE_OFF_BRIGHTNESS);
        int16_t bright_hi = read_word(zd + ZONE_OFF_UPPER_BRIGHT);
        printf("[LEVEL]   zone[%d] offset %ld id=%d floor=%ld roof=%ld bright=(%d,%d)\n",
               z, (long)zoff, (int)zone_id, (long)floor_y, (long)roof_y, (int)bright_lo, (int)bright_hi);
    }
}

int level_set_zone_roof(LevelState *level, int16_t zone_id, int32_t roof_y)
{
    uint8_t *zd = level_get_zone_data_ptr(level, zone_id);
    if (!zd) return -1;
    write_long_be(zd + ZONE_OFF_ROOF, roof_y);

    return 0;
}

int level_set_zone_floor(LevelState *level, int16_t zone_id, int32_t floor_y)
{
    uint8_t *zd = level_get_zone_data_ptr(level, zone_id);
    if (!zd) return -1;
    write_long_be(zd + ZONE_OFF_FLOOR, floor_y);

    return 0;
}
