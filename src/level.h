/*
 * Alien Breed 3D I - PC Port
 * level.h - Level data parsing and initialization
 *
 * Translated from: AB3DI.s blag: section (~line 722-848),
 *                  LevelData2.s (InitPlayer, data pointers)
 *
 * Level data comes in three files:
 *   disk/levels/level_X/twolev.bin        - Level geometry & objects
 *   disk/levels/level_X/twolev.graph.bin  - Zone graphics data
 *   disk/levels/level_X/twolev.clips      - Clip/visibility data
 *
 * All three are LHA-compressed in the original. The raw data has a
 * specific header format with offsets to sub-structures.
 */

#ifndef LEVEL_H
#define LEVEL_H

#include "game_state.h"

/*
 * Zone info returned by level_get_zone_info (big-endian reads from level data).
 */
typedef struct {
    int16_t zone_id;
    int32_t floor_y;
    int32_t roof_y;
    int32_t upper_floor_y;
    int32_t upper_roof_y;
    int32_t water_y;
    int16_t brightness;
    int16_t upper_brightness;
    int16_t tel_zone;
    int16_t tel_x;
    int16_t tel_z;
} ZoneInfo;

/*
 * Return a copy of *z with byte order of all multi-byte fields swapped. Use when
 * a ZoneInfo was filled from raw big-endian bytes and you need host order, or vice versa.
 * If z is NULL, returns a zeroed ZoneInfo.
 */
ZoneInfo zone_info_swap_endianness(const ZoneInfo *z);

/*
 * Fill *out with zone data for the given zone_id. Returns 0 on success, -1 if
 * zone_id is out of range or level has no zone_adds/data.
 */
int level_get_zone_info(const LevelState *level, int16_t zone_id, ZoneInfo *out);

/*
 * Return a pointer to the zone data block for zone_id (writable), or NULL if
 * invalid. Use this when updating zone data (e.g. door roof) so the address
 * comes from the same lookup as validation.
 */
uint8_t *level_get_zone_data_ptr(LevelState *level, int16_t zone_id);

/*
 * Set the zone roof Y (big-endian long at ZONE_OFF_ROOF). Returns 0 on success, -1 if invalid.
 */
int level_set_zone_roof(LevelState *level, int16_t zone_id, int32_t roof_y);

/*
 * Set the zone floor Y (big-endian long at ZONE_OFF_FLOOR). Returns 0 on success, -1 if invalid.
 */
int level_set_zone_floor(LevelState *level, int16_t zone_id, int32_t floor_y);

/*
 * Log each zone's offset, id, floor, roof, brightness to stdout (for periodic debug output).
 */
void level_log_zones(const LevelState *level);

/*
 * Parse loaded level data and resolve all internal pointers.
 *
 * Translated from AB3DI.s blag: section.
 *
 * Level data header (twolev.bin):
 *   Word  0: PLR1 start X
 *   Word  1: PLR1 start Z
 *   Word  2: PLR1 start zone
 *   Word  3: PLR2 start X
 *   Word  4: PLR2 start Z
 *   Word  5: PLR2 start zone
 *   Word  6: unused
 *   Word  7: Number of points
 *   Word  8: unused
 *   Word  9: unused
 *   Word 10: Number of object points
 *   Long 11: Offset to points
 *   Long 13: Offset to floor lines
 *   Long 15: Offset to object data
 *   Long 17: Offset to player shot data
 *   Long 19: Offset to nasty shot data
 *   Long 21: Offset to object points
 *   Long 23: Offset to player 1 object
 *   Long 25: Offset to player 2 object
 *   Word 16: Number of zones (in graphics data)
 *
 * Graphics data header (twolev.graph.bin):
 *   Long  0: Offset to door data
 *   Long  1: Offset to lift data
 *   Long  2: Offset to switch data
 *   Long  3: Offset to zone graph adds
 *   Then: zone offset table (one long per zone)
 */
int level_parse(LevelState *level);

/*
 * Assign clip data to zone graph lists.
 * Translated from AB3DI.s assignclips loop (~line 812-843).
 */
void level_assign_clips(LevelState *level, int16_t num_zones);

#endif /* LEVEL_H */
