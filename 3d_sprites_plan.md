---
name: 3D sprite support
overview: "The Amiga original draws two kinds of objects: 2D billboard sprites (BitMapObj) and 3D vector/polygon objects (PolygonObj). The PC port currently skips 3D objects (obj[6]==0xFF). Supporting them requires loading .vec data, implementing vertex transform and projection, depth-sorting parts, and drawing shaded polygons."
todos: []
isProject: false
---

# Support for Amiga 3D sprites (vector/polygon objects)

## Current state

- **Detection**: Objects with **byte at offset 6 == 0xFF** (`OBJ_3D_SPRITE`) are 3D; the port explicitly **skips** them in [src/renderer.c](src/renderer.c) (lines 1634–1635: `if ((uint8_t)obj[6] == OBJ_3D_SPRITE) continue`).
- **2D path**: All other objects use the existing billboard path (BitMapObj): single point from `obj_rotated[pt_num]`, WAD/PTR/PAL sprite, scale by z, draw with `renderer_draw_sprite`.
- **Amiga 3D path**: When byte 6 is 0xFF, the Amiga calls `PolygonObj` in [amiga/ObjDraw3.ChipRam.s](amiga/ObjDraw3.ChipRam.s), which uses **vector objects** (`.vec` files): 3D vertices, per-part polygon lists, and draws rotated, projected, depth-sorted polygons.

## Amiga 3D object and .vec format (from ObjDraw3 and Defs.i)

**64-byte object when 3D** (same blob; interpretation differs when obj[6]==0xFF):


| Offset | Use in 3D                                                                                             |
| ------ | ----------------------------------------------------------------------------------------------------- |
| 0      | Point number (object position in world; index into `object_points` / `obj_rotated`)                   |
| 2      | Brightness (objVectBright)                                                                            |
| 4      | Unknown (objVectUnknown4) – used in projection                                                        |
| 6      | 0xFF (marks 3D) **and** low byte = vector object index into POLYOBJECTS (ASM uses word at 6 as index) |
| 8      | objVectNumber (vector object number) – same index                                                     |
| 10     | objVectFrameNumber (animation frame)                                                                  |
| 30     | objVectFacing (angle in Amiga units, for rotating model)                                              |


**POLYOBJECTS** (in ObjDraw3.ChipRam.s): table of pointers to 10 vector objects – robot, medipac, exitsign, crate, terminal, blueind, Greenind, Redind, yellowind, gaspipe. Data comes from `incbin` `.vec` or `include` `.vec.s`.

**.vec layout** (from [amiga/vectorobjects/YellowInd.vec.s](amiga/vectorobjects/YellowInd.vec.s) and PolygonObj):

- Word 0: `num_points`
- Word 2: `num_frames`
- Word 4: offset to frame pointers (or start of part list)
- Then: **part list** – each entry (pointer to polygon data, sort point index), terminated by -1
- Then: **frame data** – for each frame, list of 3D points (x,y,z words = 6 bytes per point)
- **Polygon data** per part: “lines to draw”, “preholes”, vertex indices; polygons are drawn via `doapoly` (triangle/quad rasterization with Left/Right/objclipt/objclipb clipping and brightness)

So 3D sprites are **small 3D meshes** (vertices + polygons), rotated by object facing, positioned at the object’s point, projected to screen, parts sorted by depth, then each polygon drawn with distance-based brightness.

## What needs to be done

### 1. Vector object data (load or convert .vec)

- **Option A**: Parse Amiga `.vec` / `.vec.s` at runtime (binary + asm-style layout). Requires a full .vec format spec and robust parser.
- **Option B**: Add a small converter (script or tool) that reads `.vec` and emits a C-friendly format (e.g. structs: points, frames, part list, per-part polygon vertex indices). Load that at runtime from disk or embed in the build.
- **Option C**: Embed only the few needed .vec files (e.g. robot, medipac, exitsign, crate, terminal, indicators, gaspipe) as binary blobs and parse the known layout in C.

Recommendation: **Option B or C** – define a simple “poly object” format (points, parts, polygons) and either convert .vec once or parse the known .vec layout in C. Avoid reimplementing the full Amiga binary format in the main render path.

### 2. Renderer: stop skipping 3D objects and branch to a 3D path

In [src/renderer.c](src/renderer.c) inside `draw_zone_objects`:

- When `(uint8_t)obj[6] == OBJ_3D_SPRITE`, **do not** `continue`.
- Read: point number (offset 0), brightness (2), vector object index (word at 6 → use low byte or full word as index into a POLYOBJECTS-style table), facing (offset 30), frame (offset 10). Use existing `obj_rotated[pt_num]` for view-space position (x, z, depth).
- Call a new function, e.g. `draw_3d_vector_object(state, obj, orp, zone_roof, zone_floor)`, that implements the PolygonObj pipeline.

### 3. Implement PolygonObj pipeline in C

Implement in the same file (or a dedicated `renderer_3dobj.c`):

- **Load poly object**: Resolve vector index to a loaded poly object (vertices, frames, parts, polygons). If not loaded, skip drawing (or fallback to a placeholder).
- **Object-space → view-space**:
  - Rotate all vertices by `(objVectFacing - viewer_angle)` (same rotation convention as level/objects: use existing sin/cos tables or `r->sinval`/`r->cosval`).
  - Translate by object centre from `obj_rotated[pt_num]` (x_fine, z) and camera-relative Y (e.g. from zone floor/roof or object height at offset 4).
- **Project vertices**: Same projection as walls/sprites (e.g. `screen_x = x * scale / z + half_width`, `screen_y = y * scale / z + half_height`). Store in a “boxonscr” style array (screen x,y per vertex).
- **Per-vertex brightness**: Distance-based (e.g. z>>7 + objVectBright), clamped; store in “boxbrights” style array for shading polygons.
- **Depth-sort parts**: For each part, compute a sort key (e.g. max z or centroid z of its vertices). Sort parts back-to-front (painter’s algorithm).
- **Draw polygons** (`doapoly`): For each part, for each polygon (triangle or quad):
  - Clip to screen and to objclipt/objclipb (top/bottom) and Left/Right (from current zone/clip).
  - Rasterize (flat or per-vertex shaded) into the same framebuffer/depth buffer used by sprites. Use existing depth buffer so 3D objects occlude and are occluded correctly.

Reuse existing renderer globals where possible (e.g. clip bounds, depth buffer, RGB buffer).

### 4. Data and constants

- **POLYOBJECTS table**: In C, an array of pointers to poly object data (or indices into a single packed buffer). Populated when loading .vec (or converted) data.
- **Object layout**: Use [src/game_types.h](src/game_types.h) and Defs.i: obj[0]=point, obj[2]=bright, obj[6]=0xFF + vect index, obj[8]=vect number, obj[10]=frame, obj[30]=facing. Use existing `rd16`/accessors for big-endian.
- **Limits**: Amiga uses ~250 points (boxrot/boxonscr). Define e.g. `MAX_POLY_POINTS` and `MAX_POLY_PARTS` and clamp or reject oversized data.

### 5. Assets and loading

- **Where .vec files live**: [amiga/vectorobjects/](amiga/vectorobjects/) (robot.vec, medipac.vec, exitsign.vec, crate.vec, terminal.vec, blueind.vec, Greenind.vec, Redind.vec, yellowind.vec.s, gaspipe.vec). Either ship these and parse them, or ship a converted format next to existing level/sprite assets.
- **When to load**: Same time as other object assets (e.g. in [src/stub_io.c](src/stub_io.c) or wherever sprite WAD/PTR are loaded), or on first use. Ensure POLYOBJECTS is filled before any 3D object is drawn.

### 6. Testing and fallback

- **Fallback**: If vector index is out of range or poly data missing, skip drawing (current behaviour) or draw a simple 2D placeholder (e.g. a small quad or reuse a billboard).
- **Testing**: Use a level that places 3D objects (exitsign, crate, terminal, indicators, gas pipe, robot, medipac). Compare with Amiga in the same zone/camera.

## Suggested order of implementation

1. **Define a minimal “poly object” format** (points, parts, polygons) and either document .vec or write a one-off .vec → C struct (or binary) converter.
2. **Load at least one .vec** (e.g. exitsign or crate) and add a POLYOBJECTS-style table in the renderer.
3. **Implement the 3D path** in the renderer: no longer skip when obj[6]==0xFF; read object fields; call `draw_3d_vector_object` with position, angle, brightness, frame.
4. **Implement** vertex rotate + translate, project, brightness, part sort, and one polygon type (e.g. triangles) with flat shading and clipping.
5. **Extend** to all polygon types used by doapoly (quads, “preholes”) and per-vertex brightness if needed.
6. **Load all 10 vector objects** and hook them into POLYOBJECTS so all 3D object types draw.

## Files to touch

- [src/renderer.c](src/renderer.c): Remove 3D skip; add `draw_3d_vector_object` and PolygonObj/doapoly logic (or call into a new file).
- [src/renderer.h](src/renderer.h): Declare `draw_3d_vector_object` and any poly-object types/limits.
- New (optional): `src/renderer_3dobj.c` / `.h` for poly load + draw to keep renderer.c smaller.
- [src/game_types.h](src/game_types.h): Already has `OBJ_3D_SPRITE`; add any shared 3D object field accessors if needed.
- Asset loading (e.g. [src/stub_io.c](src/stub_io.c) or new loader): Load .vec or converted poly data and register in POLYOBJECTS.
- [amiga/ObjDraw3.ChipRam.s](amiga/ObjDraw3.ChipRam.s) + [amiga/vectorobjects/](amiga/vectorobjects/): Reference for .vec layout and doapoly behaviour (no code changes required).

## Summary

Supporting 3D sprites means: **(1)** loading or converting the Amiga .vec vector objects into a usable format, **(2)** no longer skipping objects with obj[6]==0xFF and instead reading their 3D-specific fields, **(3)** implementing the PolygonObj pipeline (rotate vertices, project, brightness, depth-sort parts, draw polygons with clipping and depth). The bulk of the work is implementing the .vec format (or a derived format) and the polygon rasterization path to match the Amiga’s behaviour.
