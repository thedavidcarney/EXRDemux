// 01_inspect_effect.jsx
//
// Run via:  File > Scripts > Run Script File...
//
// PRE: select a layer in the active comp that has EXtractoR applied.
// POST: writes "exrdemux_inspect.txt" to your Desktop with every effect's
//       matchName, every property's matchName, type, and current value.
//       Recurses into property groups (which EXtractoR uses).

(function () {
    var item = app.project.activeItem;
    if (!item || !(item instanceof CompItem)) {
        alert("Open a comp first.");
        return;
    }
    var sel = item.selectedLayers;
    if (sel.length === 0) {
        alert("Select a layer that has EXtractoR applied.");
        return;
    }
    var layer = sel[0];
    var fx = layer.property("ADBE Effect Parade");
    if (!fx || fx.numProperties === 0) {
        alert("Selected layer has no effects on it.");
        return;
    }

    var lines = [];
    lines.push("=== Layer: " + layer.name + " ===");
    lines.push("Source: " + (layer.source ? layer.source.name : "<none>"));
    lines.push("Effect count: " + fx.numProperties);
    lines.push("");

    for (var i = 1; i <= fx.numProperties; i++) {
        var e = fx.property(i);
        lines.push("--- Effect " + i + " ---");
        dumpProperty(e, 0, lines);
        lines.push("");
    }

    var outFile = new File(Folder.desktop.fsName + "/exrdemux_inspect.txt");
    outFile.encoding = "UTF-8";
    outFile.open("w");
    outFile.write(lines.join("\n"));
    outFile.close();

    alert("Inspection report written to:\n" + outFile.fsName);
})();

function dumpProperty(p, depth, lines) {
    var pad = "";
    for (var s = 0; s < depth; s++) pad += "  ";

    var name = safeStr(function () { return p.name; });
    var match = safeStr(function () { return p.matchName; });
    lines.push(pad + "[" + name + "]   matchName: " + match);

    var isGroup = false;
    try {
        // PropertyType is INDEXED_GROUP, NAMED_GROUP, or PROPERTY (leaf)
        if (p.propertyType === PropertyType.INDEXED_GROUP ||
            p.propertyType === PropertyType.NAMED_GROUP) {
            isGroup = true;
        }
    } catch (err) {
        // Fall through; we'll detect via numProperties below
    }

    if (!isGroup) {
        // Some groups don't expose propertyType cleanly — fall back: if it has
        // numProperties > 0 and lacks a value, treat as group.
        try {
            if (typeof p.numProperties === "number" && p.numProperties > 0 &&
                (typeof p.value === "undefined")) {
                isGroup = true;
            }
        } catch (err) {
            // .value threw — likely a group
            try {
                if (typeof p.numProperties === "number" && p.numProperties > 0) {
                    isGroup = true;
                }
            } catch (err2) {}
        }
    }

    if (isGroup) {
        var n = 0;
        try { n = p.numProperties; } catch (err) {}
        lines.push(pad + "  (group, " + n + " children)");
        for (var k = 1; k <= n; k++) {
            try {
                dumpProperty(p.property(k), depth + 1, lines);
            } catch (err) {
                lines.push(pad + "  <error reading child " + k + ": " + safeErr(err) + ">");
            }
        }
        return;
    }

    // Leaf property
    var typeStr = safeStr(function () { return propTypeName(p.propertyValueType); });
    lines.push(pad + "  valueType : " + typeStr);

    var canExpr = safeStr(function () { return "" + p.canSetExpression; });
    lines.push(pad + "  canSetExpr: " + canExpr);

    var canVary = safeStr(function () { return "" + p.canVaryOverTime; });
    lines.push(pad + "  canVary   : " + canVary);

    var val = safeStr(function () { return "" + p.value; });
    lines.push(pad + "  value     : " + val);
}

function safeStr(fn) {
    try { return fn(); }
    catch (err) { return "<unreadable: " + safeErr(err) + ">"; }
}

function safeErr(err) {
    try {
        if (err && err.message) return err.message;
        if (err && err.description) return err.description;
        return "" + err;
    } catch (e) { return "<error stringifying error>"; }
}

function propTypeName(t) {
    var map = {};
    try { map[PropertyValueType.NO_VALUE]       = "NO_VALUE"; } catch (e) {}
    try { map[PropertyValueType.ThreeD_SPATIAL] = "ThreeD_SPATIAL"; } catch (e) {}
    try { map[PropertyValueType.ThreeD]         = "ThreeD"; } catch (e) {}
    try { map[PropertyValueType.TwoD_SPATIAL]   = "TwoD_SPATIAL"; } catch (e) {}
    try { map[PropertyValueType.TwoD]           = "TwoD"; } catch (e) {}
    try { map[PropertyValueType.OneD]           = "OneD"; } catch (e) {}
    try { map[PropertyValueType.COLOR]          = "COLOR"; } catch (e) {}
    try { map[PropertyValueType.CUSTOM_VALUE]   = "CUSTOM_VALUE"; } catch (e) {}
    try { map[PropertyValueType.MARKER]         = "MARKER"; } catch (e) {}
    try { map[PropertyValueType.LAYER_INDEX]    = "LAYER_INDEX"; } catch (e) {}
    try { map[PropertyValueType.MASK_INDEX]     = "MASK_INDEX"; } catch (e) {}
    try { map[PropertyValueType.SHAPE]          = "SHAPE"; } catch (e) {}
    try { map[PropertyValueType.TEXT_DOCUMENT]  = "TEXT_DOCUMENT"; } catch (e) {}
    return (typeof map[t] !== "undefined") ? map[t] : ("UNKNOWN(" + t + ")");
}
