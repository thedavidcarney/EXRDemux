// 03_extract_all_layers.jsx
//
// Run via:  File > Scripts > Run Script File...
//
// PRE: have your multilayer EXR imported as a footage item in the project.
//
// PURPOSE: end-to-end proof. Build a comp where each layer is one channel
// of the EXR, extracted via EXtractoR, set programmatically.
//
// If the visual output is correct, the C++ plugin is unnecessary.

(function () {
    // ---- CONFIG ----------------------------------------------------------

    // Paste the channel list from `python dump_exr_channels.py file.exr`.
    // ORDER MATTERS — the index in this array maps to the EXtractoR popup
    // index (after applying INDEX_OFFSET).
    var CHANNEL_LIST = [
        "R",
        "G",
        "B",
        "A"
        // ... paste yours here
    ];

    // How many phantom entries does AE prepend to the file's channel list?
    // From your manual UI recon (Step 1 in README): if the dropdown shows
    // "None" then "R" then "G"..., INDEX_OFFSET = 1 (the "None" eats index 0).
    var INDEX_OFFSET = 1;

    var EFFECT_MATCH_NAME = "fnord EXtractoR";   // verify via 01_inspect
    var RED_PROP_NAME     = "Red";               // verify via 01_inspect
    var GREEN_PROP_NAME   = "Green";
    var BLUE_PROP_NAME    = "Blue";

    // Which footage to use: first item whose name contains this substring.
    var FOOTAGE_NAME_HINT = ".exr";

    // ----------------------------------------------------------------------

    var footage = null;
    for (var i = 1; i <= app.project.numItems; i++) {
        var it = app.project.item(i);
        if (it instanceof FootageItem && it.name.toLowerCase().indexOf(FOOTAGE_NAME_HINT.toLowerCase()) >= 0) {
            footage = it; break;
        }
    }
    if (!footage) {
        alert("No footage item with '" + FOOTAGE_NAME_HINT + "' in the name was found.");
        return;
    }

    var dur = footage.duration > 0 ? footage.duration : 5;
    var fps = footage.frameRate > 0 ? footage.frameRate : 24;
    var comp = app.project.items.addComp(
        "EXR_Extracted_" + footage.name,
        footage.width, footage.height, footage.pixelAspect, dur, fps);

    app.beginUndoGroup("Extract all EXR layers");

    var failures = [];
    for (var c = 0; c < CHANNEL_LIST.length; c++) {
        var chName = CHANNEL_LIST[c];
        var chIndex = c + INDEX_OFFSET;

        var layer = comp.layers.add(footage);
        layer.name = chName;

        var ex;
        try {
            ex = layer.property("ADBE Effect Parade").addProperty(EFFECT_MATCH_NAME);
        } catch (err) {
            failures.push(chName + ": addProperty failed (" + err + ")");
            continue;
        }

        try {
            ex.property(RED_PROP_NAME).setValue(chIndex);
            ex.property(GREEN_PROP_NAME).setValue(chIndex);
            ex.property(BLUE_PROP_NAME).setValue(chIndex);
        } catch (err) {
            failures.push(chName + " (idx " + chIndex + "): " + err);
        }
    }

    app.endUndoGroup();
    comp.openInViewer();

    var msg = "Created " + CHANNEL_LIST.length + " layers in '" + comp.name + "'.";
    if (failures.length > 0) {
        msg += "\n\nFailures:\n" + failures.join("\n");
    } else {
        msg += "\n\nAll channels set without error. Verify visually:";
        msg += "\n- Each layer should show only the named channel's content.";
        msg += "\n- If the wrong channel shows up, adjust INDEX_OFFSET and re-run.";
    }
    alert(msg);
})();
