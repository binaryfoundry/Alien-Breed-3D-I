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

/* Little-endian variants (some level data uses LE for door/switch tables) */
static int16_t read_word_le(const uint8_t *p)
{
    return (int16_t)((p[1] << 8) | p[0]);
}
static int32_t read_long_le(const uint8_t *p)
{
    return (int32_t)((p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0]);
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
    int32_t door_offset = read_long(lg + 0);
    int32_t lift_offset = read_long(lg + 4);
    int32_t switch_offset = read_long(lg + 8);
    int32_t zone_graph_offset = read_long(lg + 12);
    printf("[LEVEL] Graphics header: door=%ld lift=%ld switch=%ld zone_graph=%ld\n",
           (long)door_offset, (long)lift_offset, (long)switch_offset, (long)zone_graph_offset);
    level->door_data_owned = false;
    level->switch_data_owned = false;

    if (door_offset > 0) {
        const uint8_t *door_src = lg + door_offset;
        int16_t zone_id_be = read_word(door_src);
        int16_t type_be = read_word(door_src + 2);
        int16_t zone_id_le = read_word_le(door_src);
        int16_t type_le = read_word_le(door_src + 2);
        /* Alternate layout: zone at 4, type at 6 (4-byte header; bytes 0-3 padding) */
        int16_t zone_at4 = read_word(door_src + 4);
        int16_t type_at6 = read_word(door_src + 6);
        /* Alternate layout: 18-byte entries with zone at 2, type at 4 */
        int16_t zone_at2 = read_word(door_src + 2);
        int16_t type_at4 = read_word(door_src + 4);
        int num_zones = (int)read_word(ld + 16);  /* will be set below; use early for validation */

        if (zone_id_be < 0) {
            level->door_data = NULL;  /* empty list */
        } else if ((type_be >= 0 && type_be <= 5 && zone_id_be >= 0 && zone_id_be < num_zones) || num_zones <= 0) {
            level->door_data = (uint8_t *)(lg + door_offset);
        } else if (type_at6 >= 0 && type_at6 <= 5 && zone_at4 >= 0 && zone_at4 < num_zones) {
            /* Zone at 4, type at 6. Try both 20-byte and 16-byte stride; use whichever yields more doors (stops at zone < 0). */
            int nd20 = 0;
            const uint8_t *d = door_src;
            while (nd20 < 256 && read_word(d + 4) >= 0) { nd20++; d += 20; }
            int nd16 = 0;
            d = door_src;
            while (nd16 < 256 && read_word(d + 4) >= 0) { nd16++; d += 16; }
            int nd = (nd16 > nd20) ? nd16 : nd20;
            int stride = (nd16 > nd20) ? 16 : 20;
            uint8_t *buf = (uint8_t *)malloc((size_t)(nd + 1) * 16u);
            if (buf) {
                for (int i = 0; i < nd; i++) {
                    const uint8_t *s = door_src + i * (unsigned)stride;
                    uint8_t *t = buf + i * 16;
                    write_word_be(t + 0, read_word(s + 4));   /* zone */
                    write_word_be(t + 2, read_word(s + 6));   /* type */
                    write_long_be(t + 4, read_long(s + 8));   /* pos */
                    write_word_be(t + 8, read_word(s + 12));  /* vel */
                    write_word_be(t + 10, read_word(s + 14)); /* max */
                    if (stride >= 20) {
                        write_word_be(t + 12, read_word(s + 16)); /* timer */
                        write_word_be(t + 14, read_word(s + 18)); /* flags */
                    } else {
                        write_word_be(t + 12, 0);  /* timer default */
                        write_word_be(t + 14, 0); /* flags default */
                    }
                }
                write_word_be(buf + nd * 16, (int16_t)-1);
                level->door_data = buf;
                level->door_data_owned = true;
            } else {
                level->door_data = (uint8_t *)(lg + door_offset);
            }
        } else if (type_at4 >= 0 && type_at4 <= 5 && zone_at2 >= 0 && zone_at2 < num_zones) {
            /* 18-byte entries: bytes 0-1 padding, 2-3 zone, 4-5 type, 6-9 pos, 10-11 vel, 12-13 max, 14-15 timer, 16-17 flags */
            int nd = 0;
            const uint8_t *d = door_src;
            while (read_word(d + 2) >= 0) { nd++; d += 18; }
            uint8_t *buf = (uint8_t *)malloc((size_t)(nd + 1) * 16u);
            if (buf) {
                for (int i = 0; i < nd; i++) {
                    const uint8_t *s = door_src + i * 18;
                    uint8_t *t = buf + i * 16;
                    write_word_be(t + 0, read_word(s + 2));   /* zone */
                    write_word_be(t + 2, read_word(s + 4));   /* type */
                    write_long_be(t + 4, read_long(s + 6));   /* pos */
                    write_word_be(t + 8, read_word(s + 10)); /* vel */
                    write_word_be(t + 10, read_word(s + 12)); /* max */
                    write_word_be(t + 12, read_word(s + 14));/* timer */
                    write_word_be(t + 14, read_word(s + 16));/* flags */
                }
                write_word_be(buf + nd * 16, (int16_t)-1);
                level->door_data = buf;
                level->door_data_owned = true;
            } else {
                level->door_data = (uint8_t *)(lg + door_offset);
            }
        } else if (type_le >= 0 && type_le <= 5 && zone_id_le >= 0 && zone_id_le < num_zones) {
            /* 16-byte entries, little-endian */
            int nd = 0;
            const uint8_t *d = door_src;
            while (read_word_le(d) >= 0) { nd++; d += 16; }
            uint8_t *buf = (uint8_t *)malloc((size_t)(nd + 1) * 16u);
            if (buf) {
                for (int i = 0; i < nd; i++) {
                    const uint8_t *s = door_src + i * 16;
                    uint8_t *t = buf + i * 16;
                    write_word_be(t + 0, read_word_le(s + 0));
                    write_word_be(t + 2, read_word_le(s + 2));
                    write_long_be(t + 4, read_long_le(s + 4));
                    write_word_be(t + 8, read_word_le(s + 8));
                    write_word_be(t + 10, read_word_le(s + 10));
                    write_word_be(t + 12, read_word_le(s + 12));
                    write_word_be(t + 14, read_word_le(s + 14));
                }
                write_word_be(buf + nd * 16, (int16_t)-1);
                level->door_data = buf;
                level->door_data_owned = true;
            } else {
                level->door_data = (uint8_t *)(lg + door_offset);
            }
        } else {
            level->door_data = (uint8_t *)(lg + door_offset);
        }
    } else {
        level->door_data = NULL;
    }

    /* Long 4: Offset to lifts */
    level->lift_data = (lift_offset > 0) ? (lg + lift_offset) : NULL;

    /* Long 8: Offset to switches */
    if (switch_offset > 0) {
        const uint8_t *sw_src = lg + switch_offset;
        int num_zones_sw = (int)read_word(ld + 16);
        int16_t zone_id_be = read_word(sw_src);
        int16_t zone_id_le = read_word_le(sw_src);

        if (zone_id_be < 0) {
            level->switch_data = NULL;
        } else if ((zone_id_be >= 0 && zone_id_be < num_zones_sw) || num_zones_sw <= 0) {
            level->switch_data = (uint8_t *)(lg + switch_offset);
        } else if (zone_id_le >= 0 && zone_id_le < num_zones_sw) {
            int ns = 0;
            const uint8_t *s = sw_src;
            while (read_word_le(s) >= 0) { ns++; s += 14; }
            uint8_t *buf = (uint8_t *)malloc((size_t)(ns + 1) * 14u);
            if (buf) {
                for (int i = 0; i < ns; i++) {
                    const uint8_t *ent = sw_src + i * 14;
                    uint8_t *t = buf + i * 14;
                    write_word_be(t + 0, read_word_le(ent + 0));
                    write_word_be(t + 2, read_word_le(ent + 2));
                    write_word_be(t + 4, read_word_le(ent + 4));
                    write_long_be(t + 6, read_long_le(ent + 6));
                    write_word_be(t + 10, read_word_le(ent + 10));
                    write_word_be(t + 12, read_word_le(ent + 12));
                }
                write_word_be(buf + ns * 14, (int16_t)-1);
                level->switch_data = buf;
                level->switch_data_owned = true;
            } else {
                level->switch_data = (uint8_t *)(lg + switch_offset);
            }
        } else {
            level->switch_data = (uint8_t *)(lg + switch_offset);
        }
    } else {
        level->switch_data = NULL;
    }

    /* Long 12: Offset to zone graph adds */
    level->zone_graph_adds = lg + zone_graph_offset;

    /* Zone offset table starts at byte 16 of graphics data */
    level->zone_adds = lg + 16;
    level->zone_adds_owned = false;
    level->zone_brightness_le = false;

    /* ---- Level data header ---- */
    /* Byte 14: Number of points (word) */
    int16_t num_points = read_word(ld + 14);

    /* Byte 16: Number of zones (word) */
    level->num_zones = read_word(ld + 16);

    /* Detect little-endian zone offset table (same pattern as door/switch data). */
    if (level->num_zones > 0) {
        const uint8_t *za_src = level->zone_adds;
        int32_t first_off_be = read_long(za_src);
        int32_t first_off_le = read_long_le(za_src);
        /* Reasonable offset into level->data: 0 .. 16MB */
        int reasonable_be = (first_off_be >= 0 && first_off_be <= 0x00FFFFFF);
        int reasonable_le = (first_off_le >= 0 && first_off_le <= 0x00FFFFFF);
        if (!reasonable_be && reasonable_le) {
            size_t table_bytes = (size_t)(level->num_zones) * 4u;
            uint8_t *buf = (uint8_t *)malloc(table_bytes);
            if (buf) {
                for (int z = 0; z < level->num_zones; z++) {
                    write_long_be(buf + z * 4, read_long_le(za_src + z * 4));
                }
                level->zone_adds = buf;
                level->zone_adds_owned = true;
                printf("[LEVEL] Zone offset table converted from little-endian (%d zones)\n", level->num_zones);
            }
        }
    }

    /* Detect little-endian zone brightness words (causes wrong/flashing lights if read as BE). */
    if (level->num_zones > 0 && level->zone_adds && level->data) {
        int32_t first_off = read_long(level->zone_adds);
        if (first_off >= 0) {
            const uint8_t *zd = ld + first_off;
            int16_t bright_lo_be = (int16_t)((zd[ZONE_OFF_BRIGHTNESS  ] << 8) | zd[ZONE_OFF_BRIGHTNESS   + 1]);
            int16_t bright_lo_le = (int16_t)((zd[ZONE_OFF_BRIGHTNESS+1] << 8) | zd[ZONE_OFF_BRIGHTNESS  ]);
            /* Valid brightness 0-31; anim high byte 1-3 gives values like 0x0100, 0x0200, 0x0300 */
            int valid_be = (bright_lo_be >= 0 && bright_lo_be <= 31) || (bright_lo_be >= 0x0100 && bright_lo_be <= 0x0300);
            int valid_le = (bright_lo_le >= 0 && bright_lo_le <= 31) || (bright_lo_le >= 0x0100 && bright_lo_le <= 0x0300);
            if (!valid_be && valid_le) {
                level->zone_brightness_le = true;
                printf("[LEVEL] Zone brightness words detected as little-endian\n");
            }
        }
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

    /* Log zones with animated (flashing) brightness (anim 1=pulse, 2=flicker, 3=fire; high or low byte). Try BE then LE. */
    if (level->zone_adds && level->data && level->num_zones > 0) {
        int use_le = level->zone_brightness_le;
        int flashing_count = 0;
        /* Zone offsets point into level->data; use actual size when loaded from file */
        int32_t max_zoff = 0x00FFFFFF;
        if (level->data_byte_count > 32)
            max_zoff = (int32_t)(level->data_byte_count - 32);
        for (int try_le = 0; try_le < 2; try_le++) {
            int scan_le = (try_le == 0) ? use_le : !use_le;
            int count = 0;
            for (int z = 0; z < level->num_zones; z++) {
                int32_t zoff = read_long(level->zone_adds + z * 4);
                if (zoff < 0 || zoff > max_zoff) continue;
                const uint8_t *zd = ld + zoff;
                int16_t bright_lo = scan_le
                    ? (int16_t)((zd[ZONE_OFF_BRIGHTNESS  +1]<<8)|zd[ZONE_OFF_BRIGHTNESS  ])
                    : (int16_t)((zd[ZONE_OFF_BRIGHTNESS  ]<<8)|zd[ZONE_OFF_BRIGHTNESS  +1]);
                int16_t bright_hi = scan_le
                    ? (int16_t)((zd[ZONE_OFF_UPPER_BRIGHT+1]<<8)|zd[ZONE_OFF_UPPER_BRIGHT])
                    : (int16_t)((zd[ZONE_OFF_UPPER_BRIGHT]<<8)|zd[ZONE_OFF_UPPER_BRIGHT+1]);
                unsigned hi_lo = ((unsigned)((uint16_t)bright_lo >> 8) & 0xFFu), lo_lo = ((uint16_t)bright_lo & 0xFFu);
                unsigned hi_hi = ((unsigned)((uint16_t)bright_hi >> 8) & 0xFFu), lo_hi = ((uint16_t)bright_hi & 0xFFu);
                unsigned anim_lo = (hi_lo >= 1u && hi_lo <= 3u) ? hi_lo : (lo_lo >= 1u && lo_lo <= 3u) ? lo_lo : 0;
                unsigned anim_hi = (hi_hi >= 1u && hi_hi <= 3u) ? hi_hi : (lo_hi >= 1u && lo_hi <= 3u) ? lo_hi : 0;
                if (anim_lo != 0 || anim_hi != 0) {
                    if (try_le == 0) {
                        const char *name_lo = (anim_lo == 1) ? "pulse" : (anim_lo == 2) ? "flicker" : (anim_lo == 3) ? "fire" : "static";
                        const char *name_hi = (anim_hi == 1) ? "pulse" : (anim_hi == 2) ? "flicker" : (anim_hi == 3) ? "fire" : "static";
                        printf("[LEVEL] zone[%d] flashing: lower=%s (0x%04X) upper=%s (0x%04X)\n",
                               z, name_lo, (unsigned)(uint16_t)bright_lo, name_hi, (unsigned)(uint16_t)bright_hi);
                    }
                    count++;
                }
            }
            if (try_le == 0) {
                flashing_count = count;
                use_le = scan_le;
            } else if (count > 0 && flashing_count == 0) {
                level->zone_brightness_le = true;
                use_le = 1;
                printf("[LEVEL] Zone brightness words detected as little-endian (%d flashing zones found with LE)\n", count);
                for (int z = 0; z < level->num_zones; z++) {
                    int32_t zoff = read_long(level->zone_adds + z * 4);
                    if (zoff < 0 || zoff > max_zoff) continue;
                    const uint8_t *zd = ld + zoff;
                    int16_t bright_lo = (int16_t)((zd[ZONE_OFF_BRIGHTNESS  +1]<<8)|zd[ZONE_OFF_BRIGHTNESS  ]);
                    int16_t bright_hi = (int16_t)((zd[ZONE_OFF_UPPER_BRIGHT+1]<<8)|zd[ZONE_OFF_UPPER_BRIGHT]);
                    unsigned hi_lo = ((unsigned)((uint16_t)bright_lo >> 8) & 0xFFu), lo_lo = ((uint16_t)bright_lo & 0xFFu);
                    unsigned hi_hi = ((unsigned)((uint16_t)bright_hi >> 8) & 0xFFu), lo_hi = ((uint16_t)bright_hi & 0xFFu);
                    unsigned anim_lo = (hi_lo >= 1u && hi_lo <= 3u) ? hi_lo : (lo_lo >= 1u && lo_lo <= 3u) ? lo_lo : 0;
                    unsigned anim_hi = (hi_hi >= 1u && hi_hi <= 3u) ? hi_hi : (lo_hi >= 1u && lo_hi <= 3u) ? lo_hi : 0;
                    if (anim_lo != 0 || anim_hi != 0) {
                        const char *name_lo = (anim_lo == 1) ? "pulse" : (anim_lo == 2) ? "flicker" : (anim_lo == 3) ? "fire" : "static";
                        const char *name_hi = (anim_hi == 1) ? "pulse" : (anim_hi == 2) ? "flicker" : (anim_hi == 3) ? "fire" : "static";
                        printf("[LEVEL] zone[%d] flashing: lower=%s (0x%04X) upper=%s (0x%04X)\n",
                               z, name_lo, (unsigned)(uint16_t)bright_lo, name_hi, (unsigned)(uint16_t)bright_hi);
                    }
                }
                flashing_count = count;
                break;
            }
        }
        printf("[LEVEL] %d zone(s) with animated (flashing) brightness (checked %d zones, brightness %s)\n",
               flashing_count, level->num_zones, use_le ? "LE" : "BE");
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
