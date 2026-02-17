/*
 * Alien Breed 3D I - PC Port
 * control_loop.c - Outer control loop
 *
 * Translated from: ControlLoop.s, AB3DI.s (PlayTheGame)
 *
 * Flow (from ControlLoop.s comments):
 *   1. Load Title Music
 *   2. Load title screen
 *   3. Fade up title screen
 *   4. Add 'loading' message
 *   5. Load samples and walls
 *   6. LOOP START
 *   7. Option select screens
 *   8. Free music mem, allocate level mem
 *   9. Load level
 *  10. Play level with options selected
 *  11. Reload title music
 *  12. Reload title screen
 *  13. goto 6
 */

#include "control_loop.h"
#include "game_loop.h"
#include "game_data.h"
#include "level.h"
#include "player.h"
#include "stub_display.h"
#include "stub_input.h"
#include "stub_audio.h"
#include "stub_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Password system
 *
 * Translated from ControlLoop.s CalcPassword / PassLineToGame / GetStats.
 *
 * The password encodes: energy, max_level, which guns are visible,
 * ammo for each gun. It uses parity bits and a checksum for validation,
 * then interleaves/mixes the bits and converts to A-P characters.
 * ----------------------------------------------------------------------- */

/* GetParity: set bit 7 as parity of bits 0-6 */
static uint8_t get_parity(uint8_t val)
{
    uint8_t result = val;
    for (int i = 6; i >= 0; i--) {
        if (result & (1 << i)) {
            result ^= 0x80;
        }
    }
    return result;
}

/* CheckParity: returns true if parity is valid */
static bool check_parity(uint8_t val)
{
    uint8_t computed = 0;
    for (int i = 6; i >= 0; i--) {
        if (val & (1 << i)) {
            computed ^= 0x80;
        }
    }
    return (val & 0x80) == (computed & 0x80);
}

/* Mix two bytes by interleaving their bits */
static uint16_t mix_bytes(uint8_t a, uint8_t b)
{
    uint16_t result = 0;
    b = ~b; /* NOT b before mixing */
    for (int i = 0; i < 8; i++) {
        result <<= 1;
        if (a & 1) result |= 1;
        a >>= 1;
        result <<= 1;
        if (b & 1) result |= 1;
        b >>= 1;
    }
    return result;
}

/* Unmix a word into two bytes */
static void unmix_word(uint16_t word, uint8_t *a, uint8_t *b)
{
    uint8_t ra = 0, rb = 0;
    for (int i = 0; i < 8; i++) {
        rb |= (uint8_t)((word & 1) << i);
        word >>= 1;
        ra |= (uint8_t)((word & 1) << i);
        word >>= 1;
    }
    *a = ra;
    *b = (uint8_t)(~rb); /* invert back */
}

void calc_password(GameState *state)
{
    uint8_t passbuf[8];
    memset(passbuf, 0, sizeof(passbuf));

    /* Byte 0: energy with parity */
    passbuf[0] = get_parity((uint8_t)(state->plr1.energy & 0x7F));

    /* Byte 1: gun visibility flags (bits 7-4) + max_level (bits 3-0) */
    uint8_t guns = 0;
    if (state->plr1.gun_data[1].visible) guns |= 0x80;
    if (state->plr1.gun_data[2].visible) guns |= 0x40;
    if (state->plr1.gun_data[4].visible) guns |= 0x20;
    if (state->plr1.gun_data[7].visible) guns |= 0x10;
    passbuf[1] = (uint8_t)(guns | (state->max_level & 0x0F));

    /* Byte 7: checksum of byte 1 */
    passbuf[7] = (uint8_t)((uint8_t)(passbuf[1] ^ 0xB5) * (uint8_t)(-1) + 50);

    /* Bytes 2-6: ammo for guns 0,1,2,4,7 with parity */
    passbuf[2] = get_parity((uint8_t)((state->plr1.gun_data[0].ammo >> 3) & 0x7F));
    passbuf[3] = get_parity((uint8_t)((state->plr1.gun_data[1].ammo >> 3) & 0x7F));
    passbuf[4] = get_parity((uint8_t)((state->plr1.gun_data[2].ammo >> 3) & 0x7F));
    passbuf[5] = get_parity((uint8_t)((state->plr1.gun_data[4].ammo >> 3) & 0x7F));
    passbuf[6] = get_parity((uint8_t)((state->plr1.gun_data[7].ammo >> 3) & 0x7F));

    /* Mix bytes: interleave passbuf[0..3] with passbuf[7..4] */
    uint16_t pass[4];
    pass[0] = mix_bytes(passbuf[0], passbuf[7]);
    pass[1] = mix_bytes(passbuf[1], passbuf[6]);
    pass[2] = mix_bytes(passbuf[2], passbuf[5]);
    pass[3] = mix_bytes(passbuf[3], passbuf[4]);

    /* Convert to A-P characters (4 bits -> 'A'+nibble) */
    char password_str[17];
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *pb = (uint8_t*)&pass[i];
        password_str[pos++] = 'A' + (pb[1] & 0x0F);
        password_str[pos++] = 'A' + ((pb[1] >> 4) & 0x0F);
        password_str[pos++] = 'A' + (pb[0] & 0x0F);
        password_str[pos++] = 'A' + ((pb[0] >> 4) & 0x0F);
    }
    password_str[16] = '\0';

    /* Store in password storage */
    if (state->max_level >= 0 && state->max_level < MAX_LEVELS) {
        memcpy(state->password_storage + state->max_level * (PASSWORD_LENGTH + 1),
               password_str, PASSWORD_LENGTH + 1);
    }

    printf("[PASSWORD] Generated: %s (level %d)\n", password_str, state->max_level);
}

int pass_line_to_game(GameState *state, const char *password)
{
    /* Convert A-P characters back to 4 mixed words */
    uint16_t pass[4];
    for (int i = 0; i < 4; i++) {
        uint8_t lo = (uint8_t)((password[i*4+1] - 'A') << 4) | (password[i*4] - 'A');
        uint8_t hi = (uint8_t)((password[i*4+3] - 'A') << 4) | (password[i*4+2] - 'A');
        pass[i] = (uint16_t)((hi << 8) | lo);
    }

    /* Unmix */
    uint8_t passbuf[8];
    unmix_word(pass[0], &passbuf[0], &passbuf[7]);
    unmix_word(pass[1], &passbuf[1], &passbuf[6]);
    unmix_word(pass[2], &passbuf[2], &passbuf[5]);
    unmix_word(pass[3], &passbuf[3], &passbuf[4]);

    /* Validate parity on bytes 0,2,3,4,5,6 */
    if (!check_parity(passbuf[0])) return -1;
    if (!check_parity(passbuf[2])) return -1;
    if (!check_parity(passbuf[3])) return -1;
    if (!check_parity(passbuf[4])) return -1;
    if (!check_parity(passbuf[5])) return -1;
    if (!check_parity(passbuf[6])) return -1;

    /* Validate checksum */
    uint8_t expected = (uint8_t)((uint8_t)(passbuf[1] ^ 0xB5) * (uint8_t)(-1) + 50);
    if (expected != passbuf[7]) return -1;

    /* Decode into game state (GetStats) */
    state->plr1.energy = (int16_t)(passbuf[0] & 0x7F);
    state->max_level = (int16_t)(passbuf[1] & 0x0F);
    state->plr1.gun_data[1].visible = (passbuf[1] & 0x80) ? -1 : 0;
    state->plr1.gun_data[2].visible = (passbuf[1] & 0x40) ? -1 : 0;
    state->plr1.gun_data[4].visible = (passbuf[1] & 0x20) ? -1 : 0;
    state->plr1.gun_data[7].visible = (passbuf[1] & 0x10) ? -1 : 0;
    state->plr1.gun_data[0].ammo = (int16_t)((passbuf[2] & 0x7F) << 3);
    state->plr1.gun_data[1].ammo = (int16_t)((passbuf[3] & 0x7F) << 3);
    state->plr1.gun_data[2].ammo = (int16_t)((passbuf[4] & 0x7F) << 3);
    state->plr1.gun_data[4].ammo = (int16_t)((passbuf[5] & 0x7F) << 3);
    state->plr1.gun_data[7].ammo = (int16_t)((passbuf[6] & 0x7F) << 3);

    printf("[PASSWORD] Decoded: energy=%d level=%d\n",
           state->plr1.energy, state->max_level);
    return 0;
}

/*
 * read_main_menu - Wait for menu selection
 *
 * Original (ControlLoop.s ReadMainMenu):
 *   Draws option screen, loops on CheckMenu until selection.
 *   Returns option number or special codes ($ff=ESC, $fe=TAB).
 *
 * Stubbed: immediately returns "play game" (option 1).
 */
int read_main_menu(GameState *state)
{
    printf("[MENU] Main menu (stub - auto-selecting 'Play Game')\n");
    (void)state;
    return 1; /* play game selected */
}

/*
 * play_the_game - Runs a single level from start to death/completion
 *
 * Translated from AB3DI.s PlayTheGame (~line 484 onwards).
 *
 * Original flow:
 *   - Clear/draw text screen
 *   - Allocate copper screen memory
 *   - Allocate level memory
 *   - Setup players (send level number in MP)
 *   - Load level data, graphics, clips (via dos.library + UnLHA)
 *   - Parse level data (blag:) - resolve pointers, assign clips
 *   - Setup control mode from prefs
 *   - Initialize player positions
 *   - Setup audio channels
 *   - Build scale table
 *   - Set copper/DMA/interrupt registers
 *   - Enter main game loop (mainLoop)
 */
void play_the_game(GameState *state)
{
    printf("[GAME] === PlayTheGame: level %d ===\n", state->current_level);

    /* ---- Text screen ---- */
    display_clear_text_screen();

    /* ---- Allocate screen memory ---- */
    display_alloc_copper_screen();
    display_init_copper_screen();

    /* ---- Load level data ---- */
    io_load_level_data(&state->level, state->current_level);
    io_load_level_graphics(&state->level, state->current_level);
    io_load_level_clips(&state->level, state->current_level);

    /* ---- Parse level data (blag:) ----
     * Original resolves offsets in level data to absolute pointers:
     *   DoorData, LiftData, SwitchData, ZoneGraphAdds, zoneAdds,
     *   Points, FloorLines, ObjectData, PlayerShotData, NastyShotData,
     *   ObjectPoints, PLR1_Obj, PLR2_Obj
     * And assigns clip data to zone graph lists.
     */
    if (state->level.data && state->level.graphics && !state->level.points) {
        /* Pointers not yet set by IO loader - parse from raw data */
        level_parse(&state->level);

        /* Assign clip data to zone graph lists */
        if (state->level.clips && state->level.num_zones > 0) {
            level_assign_clips(&state->level, state->level.num_zones);
        }

        /* ListOfGraphRooms is now derived per-frame from the player's
         * current zone data (at offset 48 = ToListOfGraph).  It is set
         * in player.c (player_init_from_level and player_physics_and_collision).
         * No global allocation needed here. */

        /* Allocate workspace (zone visibility bitmask) */
        if (state->level.num_zones > 0 && !state->level.workspace) {
            state->level.workspace = (uint8_t *)calloc(1,
                (size_t)(state->level.num_zones + 1));
        }

        /* Allocate zone brightness table (lower + upper = 2 * num_zones) */
        if (state->level.num_zones > 0 && !state->level.zone_bright_table) {
            state->level.zone_bright_table = (uint8_t *)calloc(1,
                (size_t)(state->level.num_zones * 2 + 1));
            /* Initialize to 0 so dark areas match Amiga; level can set brighter zones */
            memset(state->level.zone_bright_table, 0,
                   (size_t)(state->level.num_zones * 2));
        }

        printf("[GAME] Level parsed: %d zones\n", state->level.num_zones);
    } else if (state->level.points) {
        printf("[GAME] Level pointers already resolved by loader\n");
    } else {
        printf("[GAME] No level data loaded\n");
    }

    /* ---- Setup control mode from prefs ---- */
    /* Original checks Prefsfile[0] for 'k','m','n','j','p' */
    printf("[GAME] Control mode: mouse+kbd (default)\n");

    /* ---- Init player positions ---- */
    player_init_from_level(state);

    /* ---- Audio setup ---- */
    audio_mt_init();

    /* ---- Clear keyboard ---- */
    input_clear_keyboard(state->key_map);

    /* ---- Set initial state ---- */
    state->hitcol = 0;
    state->hitcol2 = 0;
    state->master_quitting = false;
    state->slave_quitting = (state->mode == MODE_SINGLE);
    state->do_anything = true;

    if (state->mode != MODE_SINGLE) {
        state->plr1.energy = PLAYER_MAX_ENERGY;
    }
    state->plr2.energy = PLAYER_MAX_ENERGY;

    printf("[GAME] Entering main loop...\n");

    /* ---- Main game loop ---- */
    game_loop(state);

    printf("[GAME] === Level ended ===\n");

    /* ---- quitGame equivalent (AB3DI.s line ~4628-4731) ---- */
    {
        /* Update energy bar one last time */
        int16_t final_energy;
        if (state->mode == MODE_SLAVE) {
            final_energy = state->plr2.energy;
        } else {
            final_energy = state->plr1.energy;
        }
        state->energy = final_energy;

        /* Stop background music */
        audio_mt_end();

        /* Check won or lost */
        if (final_energy > 0) {
            /* Won! */
            printf("[GAME] Level completed successfully!\n");

            if (state->mode == MODE_SINGLE) {
                if (state->max_level < MAX_LEVELS) {
                    state->max_level = state->current_level + 1;
                }
                state->finished_level = 1;
            }

            /* Check for end of game (level 16) */
            if (state->max_level >= MAX_LEVELS) {
                printf("[GAME] Final level complete! %s\n", end_game_text);
                state->max_level = MAX_LEVELS - 1;
                /* EndGameScroll would go here */
            }
        } else {
            /* Lost */
            printf("[GAME] Player died.\n");
            state->finished_level = 0;
        }
    }

    /* ---- Cleanup for main menu (AB3DI.s CleanupForMainMenu ~4774) ---- */
    audio_mt_end();
    display_release_copper_screen();

    state->master_pause = false;
    state->slave_pause = false;
    state->master_quitting = false;
    state->slave_quitting = false;
    state->do_anything = false;
}

/*
 * play_game - The outermost game loop
 *
 * Translated from ControlLoop.s PlayGame (~line 142).
 */
void play_game(GameState *state)
{
    state->mode = MODE_SINGLE;

    printf("[CONTROL] PlayGame starting\n");

    /* ---- Load shared assets ---- */
    io_load_walls();
    io_load_floor();
    io_load_gun_graphics();
    io_dump_textures();  /* Debug: write textures/*.bmp for viewing */
    io_load_objects();
    io_load_sfx();

    /* ---- Setup default game ---- */
    game_state_setup_default(state);

    /* ---- Give player starting weapons ----
     * The original game gives the pistol (gun 0) by default.
     * In a real level load, weapons come from save/password data. */
    state->plr1.gun_data[0].visible = -1;  /* Pistol acquired */
    state->plr1.gun_data[0].ammo = 999 * 8;  /* Starting pistol ammo (999 display units) */
    state->plr1.gun_selected = 0;
    state->plr2.gun_data[0].visible = -1;
    state->plr2.gun_data[0].ammo = 999 * 8;
    state->plr2.gun_selected = 0;

    /* ---- Bypass menu: go straight to level 1 ---- */
    state->current_level = 0;  /* Level index 0 = Level 1 */
    state->max_level = 0;
    state->finished_level = 0;
    state->nasty = true;
    state->plr1.angpos = 0;
    state->plr2.angpos = 0;

    printf("[CONTROL] Bypassing menu - starting level 1 directly\n");

    io_load_panel();

    play_the_game(state);

    display_release_panel_memory();

    if (state->finished_level) {
        state->max_level++;
        if (state->max_level >= 16) state->max_level = 15;
        printf("[CONTROL] Level completed! Level %d -> %d\n",
               state->current_level, state->max_level);
    }

    printf("[CONTROL] PlayGame finished\n");
}
