/*
 * Alien Breed 3D I - PC Port
 * sprite_palettes.h - Embedded sprite palette tables (from pc/data/pal/*.pal)
 *
 * Data is generated at configure time by pc/tools/pal_to_header.py into
 * sprite_palettes_data.h. No runtime .pal loading.
 */
#ifndef SPRITE_PALETTES_H
#define SPRITE_PALETTES_H

#include "renderer.h"
#include <stddef.h>

extern const uint8_t *sprite_pal_embedded[MAX_SPRITE_TYPES];
extern const size_t sprite_pal_embedded_size[MAX_SPRITE_TYPES];

#endif
