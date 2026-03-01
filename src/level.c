/*
 * Alien Breed 3D I - PC Port
 * level.c - Level data parsing
 *
 * Translated from: AB3DI.s blag: section (~line 722-848)
 */

#include "level.h"
#include "game_types.h"
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

/* 0=static, 1=pulse, 2=flicker, 3=fire. Amiga uses high byte only (lsr.w #8,d3; tst.b d3). */
static inline unsigned zone_bright_anim_type(int16_t word)
{
    unsigned hi = (unsigned)((uint16_t)word >> 8) & 0xFFu;
    if (hi >= 1u && hi <= 3u) return hi;
    return 0;
}
static const char *zone_anim_flag_name(unsigned t)
{
    if (t == 1) return "pulse";
    if (t == 2) return "flicker";
    if (t == 3) return "fire";
    return "static";
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

/* Forward declare for use in level_parse. */
static void log_broken_floor_line_connects(LevelState *level);

/* Map zone value from file (may be block ID at zd+0) to zone index (0..num_zones-1). */
static int zone_file_to_index(const uint8_t *ld, const uint8_t *zone_adds, int num_zones, int16_t zone_from_file)
{
    if (num_zones <= 0 || zone_from_file < 0) return -1;
    for (int z = 0; z < num_zones; z++) {
        int32_t zoff = read_long(zone_adds + (size_t)z * 4u);
        if (zoff < 0) continue;
        const uint8_t *zd = ld + zoff;
        if (read_word(zd + 0) == zone_from_file)
            return z;
    }
    if (zone_from_file < num_zones) return (int)zone_from_file;
    return -1;
}

/* Amiga door/lift format (Anims.s): 18-byte fixed header then variable wall list.
 * Header: Bottom(w), Top(w), curr(w), dir(w), Ptr(l), zone(w), conditions(w), 2 bytes at 16-17.
 * Wall list at 18: (wall_number(w), ptr(l), graphic(l)) until wall_number < 0, then +2.
 * wall_number = floor line index; ptr = offset into LEVELGRAPHICS for wall record (patch at +24 = door height).
 */
static const uint8_t *skip_amiga_wall_list(const uint8_t *p)
{
    while (read_word(p) >= 0)
        p += 2 + 4 + 4;
    return p + 2;
}

/* Parse one door's wall list; return number of wall entries, advance *p to next door.
 * If dst is non-NULL, write packed entries: 2 bytes fline (be) + 4 bytes gfx_off (be) per entry. */
static int parse_amiga_door_wall_list(const uint8_t **p, uint8_t *dst, int max_entries)
{
    const uint8_t *q = *p;
    int n = 0;
    while (n < max_entries) {
        int16_t w = read_word(q);
        if (w < 0) break;
        if (dst) {
            write_word_be(dst + n * 6, w);
            write_long_be(dst + n * 6 + 2, read_long(q + 2));
        }
        n++;
        q += 2 + 4 + 4;
    }
    if (read_word(q) < 0) q += 2;
    *p = q;
    return n;
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
     * Door table (at lg + door_offset). Entries 22 bytes each (same idea as lift: pos/top/bot), terminated by zone_id < 0.
     *   0-1:  zone_id (int16)   zone this door affects (roof height written here)
     *   2-3:  door_type (int16) 0=space/switch, 1=cond 0x900, 2=0x400, 3=0x200, 4=always open, 5=never
     *   4-7:  door_pos (int32)  current Y (*256)
     *   8-9:  door_vel (int16)  open/close speed
     *  10-13: door_top (int32) open position (*256), 14-17: door_bot (int32) closed position (*256)
     *  18-19: timer (int16)     close delay
     *  20-21: door_flags (uint16) for type 0: condition bit mask; 0 = any switch opens this door
     *
     * Switch table (at lg + switch_offset). Entries 14 bytes each, terminated by zone_id < 0.
     *   0-1:  zone_id (int16)   zone the switch is in
     *   2-3:  (reserved; byte 3 used as cooldown)
     *   4-5:  point_index (uint16) Amiga: index into Points for position; condition bit = 4 + (switch index in table), not from data
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
    level->door_wall_list = NULL;
    level->door_wall_list_offsets = NULL;
    level->door_wall_list_owned = false;
    level->num_doors = 0;
    if (door_offset <= 16) {
        level->door_data = NULL;
    } else {
        const uint8_t *door_src = lg + door_offset;
        int16_t first_w = read_word(door_src);
        if (first_w == 999) {
            level->door_data = NULL;
        } else {
            /* First pass: count doors and total wall entries */
            int nd = 0;
            int total_walls = 0;
            const uint8_t *d = door_src;
            while (read_word(d) != 999) {
                const uint8_t *wall_start = d + 18;
                int nw = parse_amiga_door_wall_list(&wall_start, NULL, 64);
                total_walls += nw;
                nd++;
                d = wall_start;
                if (nd > 256) break;
            }
            int16_t zone0 = read_word(door_src + 12);
            if (nd > 0 && nd <= 256 && zone0 >= 0 && zone0 < num_zones) {
                uint8_t *buf = (uint8_t *)malloc((size_t)(nd + 1) * 22u);
                uint8_t *wall_list = (total_walls > 0) ? (uint8_t *)malloc((size_t)total_walls * 6u) : NULL;
                uint32_t *wall_offsets = (nd > 0) ? (uint32_t *)malloc((size_t)(nd + 1) * sizeof(uint32_t)) : NULL;
                if (buf && (total_walls == 0 || (wall_list && wall_offsets))) {
                    const uint8_t *s = door_src;
                    int out_idx = 0;
                    uint32_t wall_index = 0;
                    for (int i = 0; i < nd; i++) {
                        int16_t bottom = read_word(s + 0);
                        int16_t top = read_word(s + 2);
                        int16_t curr = read_word(s + 4);
                        int16_t dir = read_word(s + 6);
                        int16_t zone = read_word(s + 12);
                        int16_t cond = read_word(s + 14);
                        const uint8_t *wall_start = s + 18;
                        uint32_t door_start = wall_index;
                        int nw = parse_amiga_door_wall_list(&wall_start, wall_list ? wall_list + wall_index * 6 : NULL, 64);
                        wall_index += nw;
                        s = wall_start;
                        if (zone >= 0 && zone < num_zones) {
                            if (wall_offsets) wall_offsets[out_idx] = door_start;
                            uint8_t *t = buf + out_idx * 22;
                            write_word_be(t + 0, zone);
                            write_word_be(t + 2, (int16_t)0);
                            /* Amiga: asr.w #2,d3 then muls #256,d3 → zone roof = curr*64, not curr*256 */
                            write_long_be(t + 4, (int32_t)curr * 64);
                            write_word_be(t + 8, dir);
                            write_long_be(t + 10, (int32_t)top * 64);
                            write_long_be(t + 14, (int32_t)bottom * 64);
                            write_word_be(t + 18, (int16_t)0);
                            write_word_be(t + 20, cond);
                            out_idx++;
                        }
                    }
                    if (wall_offsets) wall_offsets[out_idx] = wall_index;
                    write_word_be(buf + out_idx * 22, (int16_t)-1);
                    level->door_data = buf;
                    level->door_data_owned = true;
                    level->num_doors = out_idx;
                    level->door_wall_list = wall_list;
                    level->door_wall_list_offsets = wall_offsets;
                    level->door_wall_list_owned = true;
                } else {
                    free(wall_list);
                    free(wall_offsets);
                    if (buf) free(buf);
                    level->door_data = NULL;
                }
            } else {
                level->door_data = NULL;
            }
        }
    }

    /* Long 4: Offset to lifts - Amiga format (match standalone): 999 terminator, 18-byte header + variable wall list */
    level->lift_wall_list = NULL;
    level->lift_wall_list_offsets = NULL;
    level->num_lifts = 0;
    level->lift_wall_list_owned = false;
    if (lift_offset <= 16) {
        level->lift_data = NULL;
    } else {
        const uint8_t *lift_src = lg + lift_offset;
        int16_t first_w = read_word(lift_src);
        if (first_w == 999) {
            level->lift_data = NULL;
        } else {
            int nl = 0;
            int total_lift_walls = 0;
            const uint8_t *d = lift_src;
            while (read_word(d) != 999) {
                const uint8_t *wall_start = d + 18;
                int nw = parse_amiga_door_wall_list(&wall_start, NULL, 64);
                total_lift_walls += nw;
                nl++;
                d = wall_start;
                if (nl > 256) break;
            }
            /* Accept table if first lift has valid zone (by index or block ID), same idea as doors. */
            int16_t zone0 = read_word(lift_src + 12);
            int z0 = zone_file_to_index(ld, lg + 16, num_zones, zone0);
            if (z0 < 0 && zone0 >= 0 && zone0 < num_zones) z0 = (int)zone0;
            if (nl > 0 && nl <= 256 && z0 >= 0) {
                uint8_t *buf = (uint8_t *)malloc((size_t)(nl + 1) * 20u);
                uint8_t *wall_list = (total_lift_walls > 0) ? (uint8_t *)malloc((size_t)total_lift_walls * 6u) : NULL;
                uint32_t *wall_offsets = (nl > 0) ? (uint32_t *)malloc((size_t)(nl + 1) * sizeof(uint32_t)) : NULL;
                if (buf && (total_lift_walls == 0 || (wall_list && wall_offsets))) {
                    const uint8_t *s = lift_src;
                    int out_idx = 0;
                    uint32_t wall_index = 0;
                    for (int i = 0; i < nl; i++) {
                        int16_t bottom = read_word(s + 0);
                        int16_t top = read_word(s + 2);
                        int16_t curr = read_word(s + 4);
                        int16_t dir = read_word(s + 6);
                        int16_t zone = read_word(s + 12);
                        int16_t conditions = read_word(s + 14);
                        const uint8_t *wall_start = s + 18;
                        uint32_t lift_start = wall_index;
                        int nw = parse_amiga_door_wall_list(&wall_start, wall_list ? wall_list + wall_index * 6 : NULL, 64);
                        wall_index += nw;
                        s = wall_start;
                        int zidx = zone_file_to_index(ld, lg + 16, num_zones, zone);
                        if (zidx < 0 && zone >= 0 && zone < num_zones) zidx = (int)zone;
                        if (zidx >= 0) {
                            if (wall_offsets) wall_offsets[out_idx] = lift_start;
                            uint8_t *t = buf + out_idx * 20;
                            write_word_be(t + 0, (int16_t)zidx);
                            write_word_be(t + 2, (int16_t)0);  /* type 0 = space key */
                            write_long_be(t + 4, (int32_t)curr * 64);
                            write_word_be(t + 8, dir);
                            write_long_be(t + 10, (int32_t)top * 64);  /* lift_top = low position (×64, same as door) */
                            write_long_be(t + 14, (int32_t)bottom * 64);     /* lift_bot = high position (×64) */
                            write_word_be(t + 18, conditions);  /* Amiga conditions mask (same as door flags) */
                            out_idx++;
                        }
                    }
                    if (wall_offsets) wall_offsets[out_idx] = wall_index;
                    write_word_be(buf + out_idx * 20, (int16_t)-1);
                    level->lift_data = buf;
                    level->lift_data_owned = true;
                    level->num_lifts = out_idx;
                    level->lift_wall_list = wall_list;
                    level->lift_wall_list_offsets = wall_offsets;
                    level->lift_wall_list_owned = true;
                } else {
                    free(wall_list);
                    free(wall_offsets);
                    if (buf) free(buf);
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
            unsigned anim_lo = zone_bright_anim_type(bright_lo);
            unsigned anim_hi = zone_bright_anim_type(bright_hi);
            printf("[LEVEL]   zone[%d] offset %ld id=%d floor=%ld roof=%ld bright=(%d,%d) anim=(%s,%s)\n",
                   z, (long)zoff, (int)zone_id, (long)floor_y, (long)roof_y, (int)bright_lo, (int)bright_hi,
                   zone_anim_flag_name(anim_lo), zone_anim_flag_name(anim_hi));
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

    /* Number of floor lines: derived at runtime (do not trust header). Amiga indexes with *16 so 16 bytes per line.
     * 1) Layout: how many 16-byte slots fit between floor block and object data.
     * 2) Zone exit lists: max floor line index referenced; we need at least max_index+1.
     * Use the smaller of the two when both are valid so we don't read past real data. */
    int32_t layout_count = (floor_offset  - obj_offset) / 16;
    if (layout_count < 0) layout_count = 0;
    int32_t max_ref_index = -1;
    if (level->zone_adds && level->num_zones > 0) {
        for (int z = 0; z < level->num_zones; z++) {
            int32_t zoff = read_long(level->zone_adds + z * 4);
            const uint8_t *zd = ld + zoff;
            int16_t list_off = read_word(zd + 32);  /* ToExitList */
            const uint8_t *list = zd + list_off;
            for (int i = 0; i < 128; i++) {
                int16_t entry = read_word(list + i * 2);
                if (entry == -2) break;
                if (entry >= 0 && entry > max_ref_index) max_ref_index = (int32_t)entry;
            }
        }
    }
    if (max_ref_index >= 0 && layout_count > 0) {
        level->num_floor_lines = (max_ref_index + 1 <= layout_count) ? (max_ref_index + 1) : layout_count;
    } else {
        level->num_floor_lines = layout_count;
    }

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

    /* Log floor lines data (16 bytes each: x, z, xlen, zlen, connect at 0,2,4,6,8) */
    if (level->floor_lines && level->num_floor_lines > 0) {
        printf("[LEVEL] Floor lines: offset=%ld count=%d (x, z, xlen, zlen, connect)\n",
               (long)floor_offset, (int)level->num_floor_lines);
        int log_max = level->num_floor_lines > 24 ? 24 : (int)level->num_floor_lines;
        for (int fli = 0; fli < log_max; fli++) {
            const uint8_t *fl = level->floor_lines + (unsigned)fli * 16u;
            int16_t fx = read_word(fl + 0);
            int16_t fz = read_word(fl + 2);
            int16_t fxlen = read_word(fl + 4);
            int16_t fzlen = read_word(fl + 6);
            int16_t fconn = read_word(fl + 8);
            printf("[LEVEL]   fl[%d] x=%d z=%d xlen=%d zlen=%d connect=%d\n",
                   fli, (int)fx, (int)fz, (int)fxlen, (int)fzlen, (int)fconn);
        }
        if ((int)level->num_floor_lines > log_max)
            printf("[LEVEL]   ... and %d more\n", (int)level->num_floor_lines - log_max);
    }

    /* Debug: dump doors and switches (door_flags; switch condition bit = 4 + switch_index, not from data) */
    if (level->door_data) {
        const uint8_t *door = level->door_data;
        int di = 0;
        while (1) {
            int16_t zone_id = read_word(door);
            if (zone_id < 0) break;
            int16_t door_type = read_word(door + 2);
            int32_t door_pos = read_long(door + 4);
            int32_t door_top = read_long(door + 10);
            int32_t door_bot = read_long(door + 14);
            uint16_t door_flags = (uint16_t)read_word(door + 20);
            printf("[LEVEL] door[%d] zone=%d type=%d pos=%ld top=%ld bot=%ld flags=0x%04X (%u)\n",
                   di, (int)zone_id, (int)door_type, (long)door_pos, (long)door_top, (long)door_bot, door_flags, door_flags);
            door += 22;
            di++;
        }
    }
    if (level->switch_data) {
        const uint8_t *sw = level->switch_data;
        int si = 0;
        while (1) {
            int16_t zone_id = read_word(sw);
            if (zone_id < 0) break;
            uint16_t point_index = (uint16_t)read_word(sw + 4);
            unsigned int bit_num = 4 + (si % 8);
            uint16_t cond_bit = (uint16_t)(1u << bit_num);
            int16_t sw_x = read_word(sw + 10);
            int16_t sw_z = read_word(sw + 12);
            int door_zone = -1;
            if (level->door_data) {
                const uint8_t *door = level->door_data;
                while (1) {
                    int16_t dz = read_word(door);
                    if (dz < 0) break;
                    int16_t door_type = read_word(door + 2);
                    uint16_t door_flags = (uint16_t)read_word(door + 20);
                    if (door_type == 0 && (door_flags & cond_bit) != 0) {
                        door_zone = (int)dz;
                        break;
                    }
                    door += 22;
                }
            }
            if (door_zone >= 0)
                printf("[LEVEL] switch[%d] zone=%d point_index=%u cond_bit=0x%04X (bit %u) pos=(%d,%d) opens_door_zone=%d\n",
                       si, (int)zone_id, point_index, cond_bit, bit_num, (int)sw_x, (int)sw_z, door_zone);
            else
                printf("[LEVEL] switch[%d] zone=%d point_index=%u cond_bit=0x%04X (bit %u) pos=(%d,%d)\n",
                       si, (int)zone_id, point_index, cond_bit, bit_num, (int)sw_x, (int)sw_z);
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
    /* Debug: dump objects in level (64 bytes each; zone < 0 = inactive) */
    if (level->object_data) {
        const uint8_t *obj = level->object_data;
        int oi = 0;
        const int max_objects = 256;
        while (oi < max_objects) {
            int16_t cid = read_word(obj + 0);
            int16_t zone = read_word(obj + 12);
            int8_t number = (int8_t)obj[16];  /* object type */
            int8_t can_see = (int8_t)obj[17];
            const uint8_t *type_data = obj + 18;  /* 44 bytes */
            printf("[LEVEL] obj[%d] zone=%d cid=%d type=%d (%s)",
                   oi, (int)zone, (int)cid, (int)number,
                   number == OBJ_NBR_KEY ? "key" :
                   number == OBJ_NBR_MEDIKIT ? "medikit" :
                   number == OBJ_NBR_AMMO ? "ammo" :
                   number == OBJ_NBR_BIG_GUN ? "big_gun" :
                   number == OBJ_NBR_PLR1 ? "plr1" : number == OBJ_NBR_PLR2 ? "plr2" : "other");
            if (number == OBJ_NBR_KEY)
                printf(" key_byte17=%d", (int)(can_see & 0xFF));
            printf("\n");
            obj += OBJECT_SIZE;
            oi++;
        }
    }

    log_broken_floor_line_connects(level);

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

int level_connect_to_zone_index(const LevelState *level, int16_t connect)
{
    if (!level->zone_adds || !level->data || level->num_zones <= 0 || connect < 0)
        return -1;
    for (int z = 0; z < level->num_zones; z++) {
        int32_t zoff = read_long(level->zone_adds + (size_t)z * 4u);
        if (zoff < 0) continue;
        const uint8_t *zd = level->data + zoff;
        if (read_word(zd + 0) == (int16_t)connect)
            return z;
    }
    if (connect < level->num_zones)
        return (int)connect;
    return -1;
}

/* Floor line: 16 bytes; word at offset 8 = connect. */
/*
 * Check invariant: for each zone, for each exit in its exit list, the adjacent
 * zone (the zone fline connect points to) should also list that fline (pass back).
 * Log when it does not hold. Ignore one-sided boundaries (actual walls: fline in
 * only one zone's exit list).
 */
static void log_broken_floor_line_connects(LevelState *level)
{
    if (!level->floor_lines || !level->zone_adds || !level->data ||
        level->num_zones <= 0 || level->num_floor_lines <= 0)
        return;

    const int n = (int)level->num_floor_lines;
    int num_zones = level->num_zones;
    int *zone_a = (int *)malloc((size_t)n * sizeof(int));
    int *zone_b = (int *)malloc((size_t)n * sizeof(int));
    unsigned char *nz = (unsigned char *)calloc((size_t)n, 1);
    if (!zone_a || !zone_b || !nz) {
        free(zone_a);
        free(zone_b);
        free(nz);
        return;
    }
    for (int i = 0; i < n; i++) zone_a[i] = zone_b[i] = -1;

    /* Reverse map: which zones reference each floor line (from ToExitList). */
    for (int z = 0; z < num_zones; z++) {
        int32_t zoff = read_long(level->zone_adds + (size_t)z * 4u);
        if (zoff < 0) continue;
        const uint8_t *zd = level->data + zoff;
        int16_t list_off = read_word(zd + 32);
        const uint8_t *list = zd + list_off;
        for (int i = 0; i < 128; i++) {
            int16_t entry = read_word(list + i * 2);
            if (entry < 0) break;  /* -1 ends exit portion, -2 ends list (match movement.c) */
            if (entry < n) {
                nz[entry]++;
                if (nz[entry] == 1) zone_a[entry] = z;
                else if (nz[entry] == 2) zone_b[entry] = z;
            }
        }
    }

    const uint8_t *flines = level->floor_lines;
    int logged = 0;

    /* Pass 1: any fline with connect >= 0 that does not resolve is a broken connect. Log zone info and each exit fline info. */
    for (int f = 0; f < n; f++) {
        int16_t connect = read_word(flines + (size_t)f * 16u + 8);

        if (connect < 0) continue;
        if (level_connect_to_zone_index(level, connect) >= 0) continue;

        int correct_zone = 0; // TODO: Work this out

        /* Log only; do not patch the connect word. */
        /* Fline f has broken connect; log each zone that lists f: zone info then each exit fline info. */
        int z1 = zone_a[f];
        int z2 = (nz[f] >= 2) ? zone_b[f] : -1;
        for (int zi = 0; zi < 2; zi++) {
            int z = (zi == 0) ? z1 : z2;
            if (z < 0) continue;
            int32_t zoff = read_long(level->zone_adds + (size_t)z * 4u);
            if (zoff < 0) continue;
            const uint8_t *zd = level->data + zoff;
            printf("[LEVEL]   zone %d (fline %d broken connect %d, should be zone %d): id=%d floor=%ld roof=%ld upper_floor=%ld upper_roof=%ld water=%ld bright=%d upper_bright=%d tel_zone=%d tel_x=%d tel_z=%d\n",
                   z, f, (int)connect, correct_zone,
                   (int)read_word(zd + 0),
                   (long)read_long(zd + ZONE_OFF_FLOOR), (long)read_long(zd + ZONE_OFF_ROOF),
                   (long)read_long(zd + ZONE_OFF_UPPER_FLOOR), (long)read_long(zd + ZONE_OFF_UPPER_ROOF),
                   (long)read_long(zd + ZONE_OFF_WATER),
                   (int)read_word(zd + ZONE_OFF_BRIGHTNESS), (int)read_word(zd + ZONE_OFF_UPPER_BRIGHT),
                   (int)read_word(zd + ZONE_OFF_TEL_ZONE), (int)read_word(zd + ZONE_OFF_TEL_X), (int)read_word(zd + ZONE_OFF_TEL_Z));
            int16_t list_off = read_word(zd + 32);
            const uint8_t *list = zd + list_off;
            for (int i = 0; i < 128; i++) {
                int16_t entry = read_word(list + i * 2);
                if (entry < 0) break;
                if (entry >= n) continue;
                const uint8_t *fl = flines + (size_t)entry * 16u;
                int fl_connect = (int)read_word(fl + 8);
                if (entry == f)
                    printf("[LEVEL]     fline %d: x=%d z=%d xlen=%d zlen=%d connect=%d (should be zone %d) length=%d normal=%d away=%d\n",
                           entry,
                           (int)read_word(fl + 0), (int)read_word(fl + 2), (int)read_word(fl + 4), (int)read_word(fl + 6),
                           fl_connect, correct_zone,
                           (int)read_word(fl + 10), (int)read_word(fl + 12), (int)read_word(fl + 14));
                else
                    printf("[LEVEL]     fline %d: x=%d z=%d xlen=%d zlen=%d connect=%d length=%d normal=%d away=%d\n",
                           entry,
                           (int)read_word(fl + 0), (int)read_word(fl + 2), (int)read_word(fl + 4), (int)read_word(fl + 6),
                           fl_connect,
                           (int)read_word(fl + 10), (int)read_word(fl + 12), (int)read_word(fl + 14));
            }
            logged++;
        }
    }

    if (logged > 0)
        printf("[LEVEL] %d broken connect(s) or exit(s) where adjacent zone does not pass back\n", logged);

    free(zone_a);
    free(zone_b);
    free(nz);
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

/* Anim type 1=pulse, 2=flicker, 3=fire. Amiga uses high byte only. */
static inline unsigned zone_anim_type_from_word(int16_t word)
{
    unsigned hi = (unsigned)((uint16_t)word >> 8) & 0xFFu;
    if (hi >= 1u && hi <= 3u) return hi;
    return 0;
}

int16_t level_get_zone_brightness(const LevelState *level, int16_t zone_id, int use_upper)
{
    if (!level->zone_adds || !level->data || level->num_zones <= 0)
        return 0;
    if (zone_id < 0 || zone_id >= level->num_zones)
        return 0;

    int32_t zoff = read_long(level->zone_adds + (size_t)zone_id * 4u);
    size_t data_len = level->data_byte_count;
    if (zoff < 0 || (data_len != 0 && (size_t)zoff + 48u > data_len))
        return 0;
    const uint8_t *zd = level->data + zoff;
    int off = use_upper ? ZONE_OFF_UPPER_BRIGHT : ZONE_OFF_BRIGHTNESS;
    int16_t word;
    if (level->zone_brightness_le)
        word = (int16_t)((zd[off + 1] << 8) | zd[off]);
    else
        word = (int16_t)((zd[off] << 8) | zd[off + 1]);
    unsigned anim = zone_anim_type_from_word(word);
    if (anim == 0)
        return word;
    /* Amiga: animated zone brightness = table value only (no base, no clamp). add.w ZoneBright,d6 uses full range e.g. -10..10 */
    if (anim >= 1 && anim <= 3)
        return (int16_t)level->bright_anim_values[anim - 1];
    return word;
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
        unsigned anim_lo = zone_bright_anim_type(bright_lo);
        unsigned anim_hi = zone_bright_anim_type(bright_hi);
        printf("[LEVEL]   zone[%d] offset %ld id=%d floor=%ld roof=%ld bright=(%d,%d) anim=(%s,%s)\n",
               z, (long)zoff, (int)zone_id, (long)floor_y, (long)roof_y, (int)bright_lo, (int)bright_hi,
               zone_anim_flag_name(anim_lo), zone_anim_flag_name(anim_hi));
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

int level_set_zone_water(LevelState *level, int16_t zone_id, int32_t water_y)
{
    uint8_t *zd = level_get_zone_data_ptr(level, zone_id);
    if (!zd) return -1;
    write_long_be(zd + ZONE_OFF_WATER, water_y);

    return 0;
}
