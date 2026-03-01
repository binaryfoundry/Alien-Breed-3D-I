/*
 * Alien Breed 3D I - PC Port
 * game_state.h - Global game state (translated from AB3DI.s / LevelData2.s globals)
 *
 * In the original Amiga code, all game state lives in global variables
 * scattered across multiple .s files. We consolidate them here into
 * structured groups while preserving the original naming for traceability.
 */

#ifndef GAME_STATE_H
#define GAME_STATE_H

#include "game_types.h"

/* -----------------------------------------------------------------------
 * Player state
 *
 * The original code has two sets of player variables:
 *   PLR1_xxx / PLR2_xxx  - "actual" positions used by rendering/collision
 *   PLR1s_xxx / PLR2s_xxx - "simulation" positions used by player control
 *   p1_xxx / p2_xxx      - "per-frame" snapshot copied before control
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Rendering / actual position (PLRn_xxx) */
    int16_t  energy;
    int16_t  gun_selected;     /* index into GunData */
    int16_t  cosval;
    int16_t  sinval;
    int16_t  angpos;
    int16_t  angspd;
    int32_t  xoff;
    int32_t  yoff;
    int32_t  yvel;
    int32_t  zoff;
    int32_t  tyoff;            /* target y offset */
    int32_t  xspdval;
    int32_t  zspdval;
    int16_t  zone;
    int32_t  roompt;           /* offset into level data (was pointer) */
    int32_t  old_roompt;
    int32_t  no_transition_back_roompt;  /* if >= 0: next frame don't transition to this room (stairs) */
    int32_t  points_to_rotate_ptr;
    int32_t  list_of_graph_rooms;
    int32_t  oldxoff;
    int32_t  oldzoff;
    int8_t   stood_in_top;
    int32_t  height;

    /* Simulation position (PLRns_xxx) */
    int16_t  s_cosval;
    int16_t  s_sinval;
    int16_t  s_angpos;
    int16_t  s_angspd;
    int32_t  s_xoff;
    int32_t  s_yoff;
    int32_t  s_yvel;
    int32_t  s_zoff;
    int32_t  s_tyoff;
    int32_t  s_xspdval;
    int32_t  s_zspdval;
    int16_t  s_zone;
    int32_t  s_roompt;
    int32_t  s_old_roompt;
    int32_t  s_points_to_rotate_ptr;
    int32_t  s_list_of_graph_rooms;
    int32_t  s_oldxoff;
    int32_t  s_oldzoff;
    int32_t  s_height;
    int32_t  s_targheight;

    /* Per-frame snapshot (pn_xxx) */
    int32_t  p_xoff;
    int32_t  p_zoff;
    int32_t  p_yoff;
    int32_t  p_height;
    int16_t  p_angpos;
    int16_t  p_bobble;
    int8_t   p_clicked;
    int8_t   p_spctap;
    int8_t   p_ducked;
    int8_t   p_gunselected;
    int8_t   p_fire;
    int16_t  p_holddown;

    /* Misc */
    int16_t  bobble;
    int8_t   clicked;
    int8_t   fire;
    int8_t   spctap;
    int8_t   ducked;

    /* Gun data (8 guns x 32 bytes each) */
    GunEntry gun_data[MAX_GUNS];

    /* Push values (from ObjectMove.s) */
    int32_t  pushx;
    int32_t  pushz;
    int32_t  opushx;
    int32_t  opushz;

    /* Shooting */
    int16_t  time_to_shoot;
    int8_t   stood_on_lift;

    /* Object distances */
    int16_t  obj_dists[250];

    /* Animation */
    int16_t  bob_frame;          /* walk bobble animation frame */
    int16_t  gun_frame;          /* gun animation countdown (counts down each frame) */
} PlayerState;

/* -----------------------------------------------------------------------
 * Level data pointers (from LevelData2.s)
 * In the original code these are absolute Amiga memory pointers.
 * Here they are byte offsets or C pointers into allocated buffers.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;               /* LEVELDATA - raw level data buffer */
    size_t   data_byte_count;    /* size of data buffer when loaded from file; 0 = unknown */
    uint8_t *graphics;           /* LEVELGRAPHICS */
    uint8_t *clips;              /* LEVELCLIPS */

    /* Parsed pointers (offsets resolved at level load time) */
    uint8_t *door_data;
    uint8_t *lift_data;
    uint8_t *switch_data;
    uint8_t *zone_graph_adds;
    uint8_t *zone_adds;
    uint8_t *points;
    uint8_t *point_brights;
    uint8_t *floor_lines;
    uint8_t *object_data;
    uint8_t *player_shot_data;
    uint8_t *nasty_shot_data;
    uint8_t *other_nasty_data;
    uint8_t *object_points;
    uint8_t *plr1_obj;           /* pointer to player 1 object in object data */
    uint8_t *plr2_obj;           /* pointer to player 2 object in object data */
    uint8_t *connect_table;
    uint8_t *water_list;          /* WaterList - water zones + oscillation data */
    int16_t bright_anim_values[3];    /* Amiga brightAnimTable: current value for anim 1,2,3 (pulse,flicker,fire) */
    unsigned int bright_anim_indices[3]; /* Current index into pulse_anim / flicker_anim / fire_flicker_anim */
    uint8_t *workspace;           /* WorkSpace bitmask for zone visibility */
    uint8_t *list_of_graph_rooms; /* ListOfGraphRooms - rooms visible from current */
    uint8_t *floor_tile;          /* floortile - 256x256 floor texture sheet */

    /* When true, door_data / switch_data / lift_data / zone_adds were allocated by level_parse and must be freed */
    bool door_data_owned;
    bool switch_data_owned;
    bool lift_data_owned;
    bool zone_adds_owned;
    bool door_wall_list_owned;

    /* Amiga door wall list: per-door (floor_line_index, gfx_offset) for patching walls. When non-NULL, door_routine
     * writes floor line 14 and graphics at gfx_offset+24. door_wall_list_offsets[i] = first entry index for door i. */
    uint8_t         *door_wall_list;       /* packed: 2 bytes fline (be16) + 4 bytes gfx_off (be32) per entry */
    uint32_t        *door_wall_list_offsets; /* [num_doors+1]: start index per door (offsets[i+1]-offsets[i] = count) */
    int              num_doors;            /* number of door entries in door_data */

    /* Amiga lift wall list: same layout as door (fline + gfx_off). lift_routine patches floor line 14 and graphics. */
    uint8_t         *lift_wall_list;
    uint32_t        *lift_wall_list_offsets; /* [num_lifts+1] */
    int              num_lifts;
    bool             lift_wall_list_owned;

    /* When true, zone data words (e.g. brightness at ZONE_OFF_BRIGHTNESS) are little-endian in level->data */
    bool zone_brightness_le;

    int16_t  num_object_points;
    int16_t  num_zones;           /* Number of zones in the level */
    int32_t  num_floor_lines;    /* Number of floor/wall line segments (for brute-force collision) */

} LevelState;

/* -----------------------------------------------------------------------
 * Full game state
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Game mode */
    GameMode        mode;               /* mors: 'n','m','s','q' */
    int16_t         mp_mode;            /* 0=versus, 1=coop */

    /* Players */
    PlayerState     plr1;
    PlayerState     plr2;

    /* Control modes */
    ControlMode     plr1_control;
    ControlMode     plr2_control;

    /* Level */
    LevelState      level;
    int16_t         current_level;      /* PLOPT */
    int16_t         max_level;          /* MAXLEVEL */
    int8_t          finished_level;     /* FINISHEDLEVEL */

    /* Frame timing */
    int16_t         frames_to_draw;     /* FramesToDraw */
    int16_t         temp_frames;        /* TempFrames */

    /* Game flags */
    bool            do_anything;        /* doAnything */
    bool            nasty;              /* NASTY - enemies active */
    bool            read_controls;      /* READCONTROLS */

    /* Multiplayer quit flags */
    bool            master_quitting;
    bool            slave_quitting;
    bool            master_pause;
    bool            slave_pause;

    /* Hit flash */
    int16_t         hitcol;
    int16_t         hitcol2;

    /* Energy/ammo display */
    int16_t         energy;
    int16_t         ammo;

    /* Zone draw order (output of order_zones, used by worry flags) */
    int16_t         zone_order_zones[256];
    int             zone_order_count;

    /* List used this frame for ordering; renderer uses it for portal clip lookup.
     * Set from viewer's current zone (zone_data + 48) or level list. */
    const uint8_t  *view_list_of_graph_rooms;

    /* End zones per level */
    int16_t         end_zones[16];

    /* Keyboard map (128 keys) */
    uint8_t         key_map[128];
    uint8_t         last_pressed;

    /* Prefs file content */
    char            prefs_file[50];

    /* Running flag */
    bool            running;

    /* Position deltas (for enemy lead prediction) */
    int16_t         xdiff1, zdiff1;
    int16_t         xdiff2, zdiff2;

    /* Cheat */
    int16_t         cheat_num;

    /* Password storage (16 levels * 17 bytes each) */
    char            password_storage[PASSWORD_STORAGE_SIZE];

    /* Current time in ms (set each frame by game loop; used for delayed splash damage) */
    uint32_t current_ticks_ms;

#define MAX_EXPLOSIONS 16
    /* Active explosion animations (barrel, rocket/grenade impact, explode_into_bits). */
    struct {
        int16_t  x, z;
        int16_t  zone;
        int32_t  y_floor;   /* world Y for sprite placement (same scale as obj_floor) */
        int8_t   frame;     /* animation frame 0..8 */
        int8_t   start_delay; /* ticks before animation starts (variation) */
        int8_t   size_scale;  /* 100 = normal; barrel uses larger (e.g. 150) */
        int8_t   anim_rate;   /* 100 = normal; 75 = 25% slower */
        int8_t   frame_frac;  /* fractional frame accumulator (0-99) for anim_rate */
    } explosions[MAX_EXPLOSIONS];
    int num_explosions;

#define MAX_PENDING_BLASTS 8
    /* Delayed blast damage (e.g. barrel: short delay before splash). Frame-rate independent. */
    struct {
        int32_t  x, z, y;
        int16_t  radius;
        int16_t  power;
        uint32_t trigger_time_ms;
    } pending_blasts[MAX_PENDING_BLASTS];
    int num_pending_blasts;

} GameState;

/* -----------------------------------------------------------------------
 * Global game state instance
 * ----------------------------------------------------------------------- */
extern GameState g_state;

/* -----------------------------------------------------------------------
 * Initialization
 * ----------------------------------------------------------------------- */
void game_state_init(GameState *state);
void game_state_init_player(PlayerState *plr);
void game_state_setup_default(GameState *state);
void game_state_setup_two_player(GameState *state);

#endif /* GAME_STATE_H */
