# Feasibility Tests

Goal: prove or disprove that **EXtractoR** (the existing fnordware AE plugin
shipped with After Effects) can be driven from ExtendScript with a stable
channel mapping. If it can, the C++ plugin we're considering does not need
to exist — the Blender addon emits a `.jsx`, AE runs it, done.

## What we're testing, in order

| # | Test | Falsifies project if... |
|---|------|---------------------------|
| 1 | Manual: read EXtractoR's dropdown order in the UI | (informational baseline) |
| 2 | `01_inspect_effect.jsx` — what does ExtendScript see? | (informational; reveals matchNames) |
| 3 | `02_set_channels.jsx` — can JSX read/write the channel popup? | NO → project still needed (different reason) |
| 4 | `dump_exr_channels.py` — what's the EXR's intrinsic channel order? | (informational baseline) |
| 5 | Compare #1 with #4 — does AE's dropdown follow the EXR file order? | YES → project may not be needed |
| 6 | `03_extract_all_layers.jsx` — drive EXtractoR from JSX end-to-end | YES, output looks right → project not needed |
| 7 | Repeat #5 + #6 with a different EXR | YES, rule holds → project definitively not needed |

## How to run

### Prerequisites

- After Effects (you have it)
- Python 3 (any version, stdlib only — no pip installs)
- One real multilayer EXR from your Blender pipeline, somewhere accessible

### Step 1 — Manual reconnaissance (5 min)

Goal: find out what EXtractoR shows in its UI.

1. Open AE. New project. New comp.
2. File → Import → File → pick a multilayer EXR. Drag it into the comp.
3. Effect & Presets panel → search "EXtractoR" → drag onto the layer.
4. In the Effect Controls panel, click the "Red" channel popup. You should see
   a long list of channels (e.g. `R`, `G`, `B`, `A`, `lightgroup_chorus.R`, ...).
5. **Screenshot or transcribe the entire list, in order.** This is "the AE list."
6. Note the topmost entries that aren't real channels (e.g. AE may insert
   "None" or similar before the file's actual channels). The number of these
   "phantom" entries is the **index offset** we need to find later.

### Step 2 — ExtendScript inspection (`01_inspect_effect.jsx`)

Goal: confirm what the AE scripting API exposes about EXtractoR. We need
the effect's `matchName` and the property names for the channel popups.

1. With EXtractoR still applied to a selected layer, in AE go to
   **File → Scripts → Run Script File...** and pick `tests/jsx/01_inspect_effect.jsx`.
2. A report file `exrdemux_inspect.txt` will be written to your Desktop.
3. Open it. Look for the EXtractoR block — record:
   - The effect's `matchName` (likely `"fnord EXtractoR"` or `"ADBE EXtractoR"`)
   - The `matchName` of the Red/Green/Blue channel properties
   - Their `propertyValueType` (probably `OneD` — meaning numeric popup)
   - Their current numeric values
4. Edit the constants at the top of `02_set_channels.jsx` and
   `03_extract_all_layers.jsx` to match what you found.

### Step 3 — Read/write the channel popup (`02_set_channels.jsx`)

Goal: confirm `setValue` actually changes the channel.

1. Run `tests/jsx/02_set_channels.jsx`.
2. Watch the Effect Controls panel — the Red channel value should cycle
   through the integers it tries.
3. The dialog at the end shows the read-back values. Each "set N → read back N"
   line confirms scripting is round-tripping.
4. **If `setValue` throws or silently fails, the project IS needed.** Stop here.
5. If values round-trip cleanly, continue.

### Step 4 — Get the EXR's intrinsic channel order (`dump_exr_channels.py`)

Goal: ground truth of what's in the file, in the order libIlmImf returns them.

```bash
python tests/python/dump_exr_channels.py "path\to\your\file.exr"
```

This prints every channel with its index. The script is pure stdlib — no
dependencies needed.

### Step 5 — Compare AE list vs file order

Lay them side by side:

```
AE list (from Step 1):           File order (from Step 4):
  0: None                          0: A
  1: R                             1: B
  2: G                             2: G
  3: B                             3: R
  4: A                             4: lightgroup_chorus.A
  5: lightgroup_chorus.A           5: lightgroup_chorus.B
  ...                              ...
```

The interesting questions:

- Does AE's dropdown follow the file order, just shifted by some offset
  (the "phantom" entries before the real channels)?
- Or does AE re-sort (alphabetical, by datatype, etc.)?
- Are there extra synthetic entries AE inserts (e.g. luminance, hue)?

**If AE = file order + N**, the mapping rule is just `index = file_position + N`.
That's everything we need. Continue to Step 6.

**If AE re-sorts unpredictably or inserts synthetic entries throughout**,
we can't build a mapping from file metadata alone. Project is still needed
unless ExtendScript exposes the dropdown labels somehow (it almost certainly
does not — popup string lists aren't part of the AE scripting API).

### Step 6 — End-to-end (`03_extract_all_layers.jsx`)

Goal: prove the mapping rule by exploding the EXR into N layers automatically.

1. Open `tests/jsx/03_extract_all_layers.jsx`. Paste the channel list from
   Step 4 into the `CHANNEL_LIST` array. Set `INDEX_OFFSET` to whatever Step 5
   told you (probably 1 — most likely AE prepends a "None" entry).
2. In AE, have a project open with the EXR imported as a footage item.
3. Run the script. It will create a new comp `EXR_Extracted` with one layer
   per channel, each with EXtractoR set to a different channel index.
4. **Verify visually**: each layer should show the right channel's content.
   `R` should look red-channel-y, `lightgroup_chorus.R` should show only the
   chorus lights, etc.

### Step 7 — Repeat with a different EXR

Same procedure, different multilayer EXR — ideally one with a different
number of lightgroups. If the same `INDEX_OFFSET` and rule still produce
correct output, the rule is real and stable.

## Decision

After Step 7:

- **All passed** → C++ plugin is unnecessary. Pivot to writing a `.jsx`
  emitter inside the upstream Blender addon. This repo can be archived or
  repurposed as the JSX template library.
- **Step 3 failed** (setValue doesn't work) → project is needed; reasons are
  scripting limitations of EXtractoR specifically.
- **Step 5 failed** (no stable mapping) → project is needed; reasons are
  EXtractoR not exposing channel-name → index mapping to scripting.
- **Step 6 failed visually** → something subtle is wrong; investigate before
  declaring either way.
