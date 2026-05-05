// 05_hash_persistence.jsx
//
// Run via: File > Scripts > Run Script File...
//
// PRE: Apply EXRDemux to your multilayer EXR layer, then click
//      "Pick Layer..." and pick "CurtainUplight_001". Then select that
//      AE layer.
//
// This script confirms:
//   1. The plugin stored a non-zero hash in "Layer Hash"
//   2. The hash matches the JSX-side FNV-1a of "CurtainUplight_001"
//   3. setValue on Layer Hash from JSX round-trips
//
// If both hashes match, the C++ and JSX implementations agree and we
// can drive selections from JSX without going through the dialog.

#include "exrdemux_helpers.jsx"

(function () {
    var EXPECTED_NAME = "CurtainUplight_001";

    var item = app.project.activeItem;
    if (!item || !(item instanceof CompItem)) { alert("Open a comp."); return; }
    var sel = item.selectedLayers;
    if (sel.length === 0) { alert("Select the EXRDemux'd layer."); return; }

    var fx = sel[0].property("ADBE Effect Parade")
        .property("tdcarney EXRDemux");
    if (!fx) { alert("EXRDemux not found on layer."); return; }

    var hi  = fx.property("Layer Hash Hi").value;
    var lo  = fx.property("Layer Hash Lo").value;
    var stored  = ((hi << 16) >>> 0) | lo;
    var jsxHash = exrdemuxHashLayerName(EXPECTED_NAME);

    var lines = [];
    lines.push("Hi:  " + hi);
    lines.push("Lo:  " + lo);
    lines.push("Combined hash: " + stored);
    lines.push("JSX hash of \"" + EXPECTED_NAME + "\":  " + jsxHash);
    lines.push("Match: " + (stored === jsxHash ? "YES" : "NO"));
    lines.push("");

    // Round-trip test via the helper: set a different layer name, read back.
    exrdemuxSetLayer(fx, "Top Light Down");
    var hi2 = fx.property("Layer Hash Hi").value;
    var lo2 = fx.property("Layer Hash Lo").value;
    var roundTripped = ((hi2 << 16) >>> 0) | lo2;
    var expectedTLD  = exrdemuxHashLayerName("Top Light Down");
    lines.push("After setLayer(\"Top Light Down\"): hi=" + hi2 + ", lo=" + lo2);
    lines.push("  combined: " + roundTripped + ", expected: " + expectedTLD);
    lines.push("  Match: " + (roundTripped === expectedTLD ? "YES" : "NO"));

    // Restore
    exrdemuxSetLayer(fx, EXPECTED_NAME);

    alert(lines.join("\n"));
})();
