// 02_set_channels.jsx
//
// Run via:  File > Scripts > Run Script File...
//
// PRE: select a layer with EXtractoR applied to a multilayer EXR.
// PURPOSE: confirm we can read AND write the channel popup property via JSX.
//
// CONFIG: edit the constants below to match what 01_inspect_effect.jsx
// revealed about your install. The defaults below are best guesses.

(function () {
    // ---- CONFIG ----------------------------------------------------------
    var EFFECT_MATCH_NAME = "fnord EXtractoR";  // verify via 01_inspect
    var CHANNEL_PROP_NAME = "Red";              // the popup we'll exercise
    var INDICES_TO_TRY    = [0, 1, 2, 3, 4, 5, 8, 12, 20];
    // ----------------------------------------------------------------------

    var item = app.project.activeItem;
    if (!item || !(item instanceof CompItem)) { alert("Open a comp."); return; }
    var sel = item.selectedLayers;
    if (sel.length === 0) { alert("Select a layer with EXtractoR applied."); return; }

    var layer = sel[0];
    var fx = layer.property("ADBE Effect Parade");

    var target = null;
    for (var i = 1; i <= fx.numProperties; i++) {
        if (fx.property(i).matchName === EFFECT_MATCH_NAME) {
            target = fx.property(i);
            break;
        }
    }
    if (!target) {
        alert("Effect not found with matchName: " + EFFECT_MATCH_NAME +
              "\nRun 01_inspect_effect.jsx and update the constant at the top.");
        return;
    }

    var prop = null;
    try { prop = target.property(CHANNEL_PROP_NAME); } catch (e) {}
    if (!prop) {
        // try by index, dump what's available
        var avail = [];
        for (var k = 1; k <= target.numProperties; k++) {
            avail.push("  " + k + ": " + target.property(k).name + "  (matchName: " + target.property(k).matchName + ")");
        }
        alert("Property '" + CHANNEL_PROP_NAME + "' not found.\nAvailable:\n" + avail.join("\n"));
        return;
    }

    var lines = [];
    lines.push("Effect      : " + target.name + "  (" + target.matchName + ")");
    lines.push("Property    : " + prop.name + "  (" + prop.matchName + ")");
    lines.push("Initial val : " + prop.value);
    lines.push("");
    lines.push("Round-trip test:");

    app.beginUndoGroup("EXtractoR setValue probe");
    for (var n = 0; n < INDICES_TO_TRY.length; n++) {
        var v = INDICES_TO_TRY[n];
        try {
            prop.setValue(v);
            lines.push("  set " + pad(v, 3) + " -> read back " + prop.value);
        } catch (err) {
            lines.push("  set " + pad(v, 3) + " -> ERROR: " + err);
        }
    }
    app.endUndoGroup();

    alert(lines.join("\n"));

    function pad(n, w) { var s = "" + n; while (s.length < w) s = " " + s; return s; }
})();
