# DMCSEEDITOR ( Mistress Revamp )

A from-scratch modding tool for **Devil May Cry 4: Special Edition** — browse the game's
archives, recolor effects and textures, swap textures and movesets, and preview the 3D
model and animations, all in one black/red window. No 3ds Max, no hex editing by hand,
no external batch scripts.

Built in C/C++ with Dear ImGui, compiled as a 32-bit Windows exe and run under CrossOver/Wine.

— **by MistressDMC**

## What it does

**Archive browser** — opens any MT Framework `.arc`, lists every entry, and labels what each
file actually is (e.g. *"Vergil coat MODEL"*, *"face texture (colour)"*, *"material refs"*)
using the character / file-ID map (pl030 = Vergil, etc.). Filter by type (efl / lmt / tex /
mod / mrl) or by name. Multi-select with checkboxes + shift-click ranges + "select all shown".

**Recolour** — pick a colour and apply it to *all* or *selected* entries in one click:
- **Effects** — each effect's per-texture colour code (the clean, crash-free method)
- **Textures** — `_BM` / `_MM` / `_NM` maps, colorised while keeping shading detail

**Hex Inspector** — raw byte view plus a per-emitter **effect colour-code picker** (find every
effect colour and set it visually).

**Pointers** — edit the texture/animation reference strings inside `.efl` effects and `.mrl`
materials — repoint a material to a different texture for texture/model swaps.

**Motion (LMT)** — the moveset swap: redirect a move to play another slot's animation
(safe & reversible — it only remaps the table, never the animation data).

**3D View** — loads the character model + rig, plays animations with a scrubber, steps motions
with the arrows, and does live move-swaps by number so you see the result before saving.

**Save** — every change is staged automatically; one button repacks the whole archive.

## Build

Requires the mingw-w64 cross toolchain (`i686-w64-mingw32-g++`):

```sh
./build.sh        # -> build/DMCSEEDITOR.exe
```

Run the exe under CrossOver/Wine (DirectX 9 backend).

## Supported formats

| Type   | What                            | Editable            |
|--------|---------------------------------|---------------------|
| `.arc` | MT Framework archive v7         | extract / repack    |
| `.efl` | Effect List (particles+colour)  | colours, path refs  |
| `.tex` | Texture (BC1/BC2/BC3/BC5)       | recolor             |
| `.mrl` | Material (texture refs)         | texture-ref swap    |
| `.lmt` | Motion list v67                 | moveset swap        |
| `.mod` | Model / skeleton                | 3D view + animation |

## Credits

DMCSEEDITOR is the work of **MistressDMC**. Uses [Dear ImGui](https://github.com/ocornut/imgui)
and [miniz](https://github.com/richgel999/miniz) (vendored in `third_party/`).
