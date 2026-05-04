// 04_exrdemux_scripting.jsx
//
// Run via:  File > Scripts > Run Script File...
//
// PRE: apply the EXRDemux effect to a layer, then select that layer.
// POST: writes "exrdemux_scripting.txt" to your Desktop with the results
//       of inspecting and round-tripping every parameter.
//
// PURPOSE: confirm that the new plugin's params (especially the Layer popup
// that EXtractoR couldn't expose) are readable AND writable from JSX. This
// is the milestone we couldn't reach with EXtractoR.

(function () {
    var EFFECT_MATCH = "tdcarney EXRDemux";
    var POPUP_VALUES_TO_TRY = [1, 2, 5, 10, 16];
    var BLACK_POINT_VALUES  = [0.0, -0.5, 1.5];
    var WHITE_POINT_VALUES  = [1.0, 0.5, 2.0];

    var item = app.project.activeItem;
    if (!item || !(item instanceof CompItem)) { fail("Open a comp first."); return; }
    var sel = item.selectedLayers;
    if (sel.length === 0) { fail("Select a layer with EXRDemux applied."); return; }

    var layer = sel[0];
    var fx = layer.property("ADBE Effect Parade");
    if (!fx || fx.numProperties === 0) { fail("Selected layer has no effects."); return; }

    var target = null;
    for (var i = 1; i <= fx.numProperties; i++) {
        if (fx.property(i).matchName === EFFECT_MATCH) { target = fx.property(i); break; }
    }
    if (!target) {
        fail("EXRDemux effect not found.\nLooking for matchName: " + EFFECT_MATCH +
             "\nFound effects:\n" + listEffects(fx));
        return;
    }

    var lines = [];
    lines.push("=== EXRDemux scripting smoke test ===");
    lines.push("Layer: " + layer.name);
    lines.push("Effect found: " + target.name + "  (" + target.matchName + ")");
    lines.push("Param count: " + target.numProperties);
    lines.push("");

    // ----- 1. Inspect every param -----
    lines.push("--- Initial param state ---");
    for (var p = 1; p <= target.numProperties; p++) {
        var prop = target.property(p);
        lines.push("  [" + p + "] " + safe(function(){ return prop.name; }) +
                   "  matchName=" + safe(function(){ return prop.matchName; }) +
                   "  value=" + safe(function(){ return "" + prop.value; }));
    }
    lines.push("");

    app.beginUndoGroup("EXRDemux scripting test");

    // ----- 2. Round-trip the Layer popup (the headline test) -----
    lines.push("--- Layer popup round-trip (the EXtractoR-couldn't-do-this test) ---");
    var popup = findProp(target, "Layer");
    if (popup) {
        var initialPopup = safe(function(){ return popup.value; });
        lines.push("  initial value: " + initialPopup);
        for (var n = 0; n < POPUP_VALUES_TO_TRY.length; n++) {
            var want = POPUP_VALUES_TO_TRY[n];
            try {
                popup.setValue(want);
                var got = popup.value;
                lines.push("  set " + want + " -> read back " + got +
                           (got === want ? "  OK" : "  *** MISMATCH ***"));
            } catch (err) {
                lines.push("  set " + want + " -> ERROR: " + errStr(err));
            }
        }
    } else {
        lines.push("  Layer property not found — listing what's available:");
        for (var q = 1; q <= target.numProperties; q++) {
            lines.push("    " + target.property(q).name);
        }
    }
    lines.push("");

    // ----- 3. Round-trip Black Point -----
    lines.push("--- Black Point round-trip ---");
    roundTripFloat(target, "Black Point", BLACK_POINT_VALUES, lines);
    lines.push("");

    // ----- 4. Round-trip White Point -----
    lines.push("--- White Point round-trip ---");
    roundTripFloat(target, "White Point", WHITE_POINT_VALUES, lines);
    lines.push("");

    // ----- 5. Toggle UnMult checkbox -----
    lines.push("--- UnMult toggle ---");
    var unmult = findProp(target, "UnMult");
    if (unmult) {
        try {
            var orig = unmult.value;
            unmult.setValue(orig ? 0 : 1);
            lines.push("  toggled " + orig + " -> " + unmult.value);
            unmult.setValue(orig);  // restore
            lines.push("  restored to " + unmult.value);
        } catch (err) {
            lines.push("  ERROR: " + errStr(err));
        }
    } else {
        lines.push("  UnMult not found.");
    }
    lines.push("");

    // ----- 6. Try to read the hidden Layer Hash -----
    // (it's marked PF_PUI_INVISIBLE so JSX may or may not see it)
    lines.push("--- Hidden Layer Hash ---");
    var hash = findProp(target, "Layer Hash");
    if (hash) {
        lines.push("  visible to JSX, value = " + safe(function(){ return hash.value; }));
    } else {
        lines.push("  not exposed (expected — invisible param)");
    }
    lines.push("");

    app.endUndoGroup();

    // Restore Layer popup to its initial value
    try { popup.setValue(initialPopup); } catch (e) {}

    var outFile = new File(Folder.desktop.fsName + "/exrdemux_scripting.txt");
    outFile.encoding = "UTF-8";
    outFile.open("w");
    outFile.write(lines.join("\n"));
    outFile.close();
    alert("Scripting smoke test complete.\nReport:\n" + outFile.fsName);

    // ---- helpers ----
    function findProp(eff, name) {
        try { return eff.property(name); } catch (e) { return null; }
    }
    function roundTripFloat(eff, name, values, out) {
        var prop = findProp(eff, name);
        if (!prop) { out.push("  " + name + " not found"); return; }
        var orig = safe(function(){ return prop.value; });
        out.push("  initial: " + orig);
        for (var i = 0; i < values.length; i++) {
            try {
                prop.setValue(values[i]);
                out.push("  set " + values[i] + " -> read back " + prop.value);
            } catch (err) {
                out.push("  set " + values[i] + " -> ERROR: " + errStr(err));
            }
        }
        try { prop.setValue(orig); } catch (e) {}
    }
    function listEffects(parade) {
        var s = [];
        for (var i = 1; i <= parade.numProperties; i++) {
            s.push("  " + parade.property(i).name + "  (" + parade.property(i).matchName + ")");
        }
        return s.join("\n");
    }
    function safe(fn) { try { return fn(); } catch (e) { return "<unreadable: " + errStr(e) + ">"; } }
    function errStr(e) { try { return (e && (e.message || e.description)) || ("" + e); } catch (z) { return "<error>"; } }
    function fail(msg) { alert(msg); }
})();
