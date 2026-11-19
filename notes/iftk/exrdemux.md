# EXRDemux

Pulls one named pass out of a multilayer EXR, like EXtractoR, but it remembers the pass by name, so re-rendering from Blender with added or reordered passes keeps the right one. Reach for it on every light pass of a multilayer render.

**Find it:** Effect > EXR > EXRDemux

## Quick start

1. Apply it to the multilayer EXR layer.
2. Click **Pick Layer...** and choose the pass. The button then reads "Layer: <name>".
3. Duplicate the layer and pick the next pass for each one you need.

## Settings

| Setting | What it does |
|---|---|
| Layer | Pick Layer... opens the list of passes in the file. Shows "Layer: (missing)" if the pass is gone from the file. |
| Black Point | Value that maps to black. Default 0. |
| White Point | Value that maps to white. Default 1. |
| UnMult | Divides colour by alpha, for premultiplied passes. Default off. |
| EXRDemux | Shows the installed version. |

## Tips

- After re-rendering the EXR to the same file name, use Reload Footage. It renders correctly either way, but faster after a reload.
- Values pass through untouched. Use a 32 bpc project to keep values above 1.
- Passes without alpha (Blender 5.2 light groups), UV and single-channel layers (shown grey) are read from disk, which is a little slower.
- A renamed pass is a different pass: pick it again after renaming it in Blender.
