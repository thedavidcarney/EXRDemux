// SplitAndSortPassesToPrecomps.jsx
//
// For the selected EXR footage item, build a folder structure of precomps:
// one precomp per layer (Demux applied + the layer's name hash baked in),
// plus a master comp containing all of them. Layers in the master are
// label-colored by shared name prefix.
//
// At startup, asks how to order the regular-light block in the master
// comp. Currently only "Layers In Order" is enabled; the spatial / random
// modes will arrive when Lightpass Studio ships its luminosity sidecar
// (see memory: project_lightpass_studio.md). The JSON contract is fixed
// — drop a `<exr_path>.luminosity.json` next to the EXR and a future
// version of this script will pick it up.
//
// Run from After Effects: File > Scripts > Run Script File... and pick this.

(function () {
    var STILL_DURATION_SEC = 30.0;
    var EXR_MAGIC = [0x76, 0x2f, 0x31, 0x01];
    var FLAG_MULTIPART = 0x1000;

    // ---- Math.imul polyfill (ExtendScript is ES3-ish, no Math.imul) ----
    var imul = Math.imul || function (a, b) {
        var aHi = (a >>> 16) & 0xFFFF;
        var aLo = a & 0xFFFF;
        var bHi = (b >>> 16) & 0xFFFF;
        var bLo = b & 0xFFFF;
        return ((aLo * bLo) + (((aHi * bLo + aLo * bHi) << 16) >>> 0)) | 0;
    };

    // FNV-1a 32-bit. Must match HashLayerName() in src/exrdemux.cpp byte-for-byte.
    function fnv1a32(s) {
        var h = 0x811c9dc5;
        for (var i = 0; i < s.length; i++) {
            h = imul(h ^ (s.charCodeAt(i) & 0xFF), 0x01000193);
        }
        return h >>> 0;
    }

    // ---- EXR header reader (streaming, binary) ----
    function ExrReader(path) {
        var f = new File(path);
        f.encoding = "BINARY";
        if (!f.open("r")) throw new Error("Cannot open EXR file:\n" + path);
        this.file = f;
        this.buf = "";
        this.pos = 0;
    }
    ExrReader.prototype._fill = function (n) {
        while (this.buf.length - this.pos < n) {
            if (this.file.eof) {
                throw new Error("Unexpected EOF while parsing EXR header");
            }
            var chunk = this.file.read(65536);
            if (chunk === null || chunk.length === 0) {
                throw new Error("EXR read failure at pos " + this.file.tell());
            }
            this.buf += chunk;
        }
    };
    ExrReader.prototype.byte = function () {
        this._fill(1);
        var b = this.buf.charCodeAt(this.pos) & 0xFF;
        this.pos++;
        return b;
    };
    ExrReader.prototype.peek = function () {
        this._fill(1);
        return this.buf.charCodeAt(this.pos) & 0xFF;
    };
    ExrReader.prototype.u32 = function () {
        this._fill(4);
        var b0 = this.buf.charCodeAt(this.pos)     & 0xFF;
        var b1 = this.buf.charCodeAt(this.pos + 1) & 0xFF;
        var b2 = this.buf.charCodeAt(this.pos + 2) & 0xFF;
        var b3 = this.buf.charCodeAt(this.pos + 3) & 0xFF;
        this.pos += 4;
        // top byte multiplied (not shifted) to stay unsigned in JS Number
        return (b0 | (b1 << 8) | (b2 << 16)) + (b3 * 0x1000000);
    };
    ExrReader.prototype.skip = function (n) {
        this._fill(n);
        this.pos += n;
    };
    ExrReader.prototype.nullStr = function () {
        var s = "";
        while (true) {
            this._fill(1);
            var c = this.buf.charCodeAt(this.pos) & 0xFF;
            this.pos++;
            if (c === 0) break;
            s += String.fromCharCode(c);
        }
        return s;
    };
    ExrReader.prototype.close = function () { try { this.file.close(); } catch (_) {} };

    // Parse the EXR file's header(s) and return all raw channel names from
    // every part. Robust to multipart files (Blender 4.x+ may emit them;
    // the Wall_Curtains test scene from Blender 5.x is 58-part multipart
    // with 4 channels per part).
    function parseExrChannels(path) {
        var r = new ExrReader(path);
        try {
            for (var i = 0; i < 4; i++) {
                if (r.byte() !== EXR_MAGIC[i]) {
                    throw new Error("Not an EXR file (bad magic):\n" + path);
                }
            }
            var version = r.u32();
            var multipart = (version & FLAG_MULTIPART) !== 0;

            var channels = [];
            while (true) {
                while (true) {
                    var attrName = r.nullStr();
                    if (attrName === "") break;
                    var attrType = r.nullStr();
                    var attrSize = r.u32();
                    if (attrName === "channels" && attrType === "chlist") {
                        var endPos = r.pos + attrSize;
                        while (r.pos < endPos) {
                            if (r.peek() === 0) { r.byte(); break; }
                            var chName = r.nullStr();
                            r.skip(16);  // pixelType+pLinear+reserved+xSampling+ySampling
                            channels.push(chName);
                        }
                        if (r.pos !== endPos) r.pos = endPos;
                    } else {
                        r.skip(attrSize);
                    }
                }
                if (!multipart) break;
                if (r.peek() === 0) { r.byte(); break; }
            }
            return channels;
        } finally {
            r.close();
        }
    }

    // From raw channel names, derive the user-facing layer display names that
    // the plugin uses (and hashes). Mirrors the plugin's logic:
    //   - everything before the last '.'  is the "layer key"
    //   - if the layer key has the form "X.X", collapse to "X"
    //   - bare channels (no dot) collapse into a single "(default)" entry
    function extractDisplayNames(channels) {
        var seenKeys = {};
        var keys = [];
        var hasDefault = false;
        for (var i = 0; i < channels.length; i++) {
            var ch = channels[i];
            var dot = ch.lastIndexOf(".");
            if (dot < 0) { hasDefault = true; continue; }
            var key = ch.substring(0, dot);
            if (!seenKeys[key]) {
                seenKeys[key] = true;
                keys.push(key);
            }
        }
        var displays = [];
        if (hasDefault) displays.push("(default)");
        for (var j = 0; j < keys.length; j++) {
            var k = keys[j];
            var mid = k.lastIndexOf(".");
            if (mid >= 0) {
                var left = k.substring(0, mid);
                var right = k.substring(mid + 1);
                if (left === right) { displays.push(left); continue; }
            }
            displays.push(k);
        }
        return displays;
    }

    // ---- Color-grouping by shared prefix --------------------------------
    function groupKey(name) {
        var m = name.match(/^(.*?)[_\-. ]?\d+$/);
        if (!m) return name;
        var prefix = m[1];
        if (prefix === "") return name;
        return prefix;
    }

    function makeLabeler() {
        var assigned = {};
        var counter = 0;
        return function (displayName) {
            var key = groupKey(displayName);
            if (!(key in assigned)) {
                counter = (counter % 16) + 1;
                assigned[key] = counter;
            }
            return assigned[key];
        };
    }

    // ---- Sort-mode dialog -----------------------------------------------
    // Returns one of "ordered", "auto-lr", "auto-tb", "front-back", "random"
    // or null if cancelled. Today only "ordered" is selectable; the other
    // modes are wired but greyed pending Lightpass Studio's sidecar.
    function chooseSortMode() {
        var dlg = new Window("dialog", "Split and Sort Passes to Precomps");
        dlg.alignChildren = "fill";
        dlg.spacing = 10;
        dlg.margins = 16;

        dlg.add("statictext", undefined,
            "Order of regular light passes in the Master Comp:");

        var radioGroup = dlg.add("panel", undefined, "");
        radioGroup.alignChildren = "left";
        radioGroup.margins = 12;
        radioGroup.spacing = 6;

        var rOrdered    = radioGroup.add("radiobutton", undefined, "Layers In Order");
        var rAutoLR     = radioGroup.add("radiobutton", undefined, "Auto Left to Right   (Lightpass Studio)");
        var rAutoTB     = radioGroup.add("radiobutton", undefined, "Auto Top to Bottom   (Lightpass Studio)");
        var rFrontBack  = radioGroup.add("radiobutton", undefined, "Front to Back   (Lightpass Studio)");
        var rRandom     = radioGroup.add("radiobutton", undefined, "Random   (Lightpass Studio)");

        rOrdered.value = true;
        rAutoLR.enabled = false;
        rAutoTB.enabled = false;
        rFrontBack.enabled = false;
        rRandom.enabled = false;

        var note = dlg.add("statictext", undefined,
            "Auto modes will read a luminosity sidecar emitted by Lightpass Studio.",
            { multiline: true });
        note.preferredSize.width = 380;

        var btns = dlg.add("group");
        btns.alignment = "right";
        var btnCancel = btns.add("button", undefined, "Cancel", { name: "cancel" });
        var btnRun    = btns.add("button", undefined, "Run",    { name: "ok"     });

        var result = null;
        btnRun.onClick = function () {
            if      (rOrdered.value)   result = "ordered";
            else if (rAutoLR.value)    result = "auto-lr";
            else if (rAutoTB.value)    result = "auto-tb";
            else if (rFrontBack.value) result = "front-back";
            else if (rRandom.value)    result = "random";
            dlg.close(1);
        };
        btnCancel.onClick = function () { dlg.close(0); };

        var ok = dlg.show();
        if (ok !== 1) return null;
        return result;
    }

    // ---- Selection validation -------------------------------------------
    function getSelectedExrFootage() {
        if (!app.project) {
            throw new Error("No project open.");
        }
        var sel = app.project.selection;
        if (!sel || sel.length === 0) {
            throw new Error("Select an EXR footage item in the Project panel before running this script.");
        }
        if (sel.length > 1) {
            throw new Error("Select exactly one footage item. Multi-selection isn't supported yet.");
        }
        var item = sel[0];
        if (!(item instanceof FootageItem)) {
            throw new Error("Selection must be a footage item (got: " + item.typeName + ").");
        }
        var src = item.mainSource;
        if (!(src instanceof FileSource)) {
            throw new Error("Footage must be backed by a file (FileSource).");
        }
        var f = src.file;
        if (!f) {
            throw new Error("Footage has no file path.");
        }
        var path = f.fsName;
        if (!/\.exr$/i.test(path)) {
            throw new Error("Selected file is not an EXR:\n" + path);
        }
        return { item: item, source: src, path: path };
    }

    // Sort the regular-light array in place per the chosen mode. Today
    // only "ordered" is reachable from the dialog; the auto modes are
    // here as scaffolding for when the luminosity sidecar exists.
    function sortRegular(regularLayers, sortMode, sidecar) {
        if (sortMode === "ordered" || !sidecar) return;
        // Centroid-driven modes — once Lightpass Studio is shipping
        // sidecars, fill in. Leaving as a no-op until then.
    }

    // ---- Main flow ------------------------------------------------------
    function main(sortMode) {
        var sel = getSelectedExrFootage();
        var footage = sel.item;
        var src = sel.source;
        var path = sel.path;

        var rawChannels = parseExrChannels(path);
        if (rawChannels.length === 0) {
            throw new Error("No channels found in EXR header:\n" + path);
        }
        var displays = extractDisplayNames(rawChannels);
        if (displays.length === 0) {
            throw new Error("Could not extract any layer names from:\n" + path);
        }

        var width = footage.width;
        var height = footage.height;
        var par = footage.pixelAspect;
        var fps = footage.frameRate || 24.0;
        var isStill = !!src.isStill;
        var compDuration = isStill ? STILL_DURATION_SEC : footage.duration;
        if (!compDuration || compDuration <= 0) compDuration = STILL_DURATION_SEC;

        var proj = app.project;
        var siblingFolder = footage.parentFolder;

        var compsFolder = proj.items.addFolder(footage.name + " Comps");
        compsFolder.parentFolder = siblingFolder;

        var precompsFolder = proj.items.addFolder("Layers Precomps");
        precompsFolder.parentFolder = compsFolder;

        var master = proj.items.addComp(
            footage.name + " Master Comp",
            width, height, par, compDuration, fps);
        master.parentFolder = compsFolder;

        var labeler = makeLabeler();
        var precompsByDisplay = [];

        for (var i = 0; i < displays.length; i++) {
            var displayName = displays[i];
            var precomp = proj.items.addComp(
                displayName, width, height, par, compDuration, fps);
            precomp.parentFolder = precompsFolder;
            precomp.label = labeler(displayName);

            var srcLayer = precomp.layers.add(footage);
            try {
                srcLayer.startTime = 0;
                if (precomp.duration > 0) {
                    srcLayer.outPoint = precomp.duration;
                }
            } catch (_) {}

            var fx = srcLayer.property("ADBE Effect Parade").addProperty("tdcarney EXRDemux");
            if (!fx) {
                throw new Error(
                    "Could not apply 'tdcarney EXRDemux' effect. Is the plugin installed?");
            }

            var hash = fnv1a32(displayName);
            var hi = (hash >>> 16) & 0xFFFF;
            var lo = hash & 0xFFFF;
            fx.property("Layer Hash Hi").setValue(hi);
            fx.property("Layer Hash Lo").setValue(lo);

            precompsByDisplay.push({ comp: precomp, name: displayName });
        }

        // 5. Categorize for the master comp. Final stack (top -> bottom):
        //      regular lights (sortMode-dependent ordering)
        //      World / Ambient / HDRI
        //      Image
        //      Alpha
        //    Cryptos: skipped from master, still in Layers Precomps folder.
        //    Image/Alpha/World/Ambient/HDRI: added but disabled.
        //    All layers get Lighten + a label color.
        var skippedCryptos = [];
        var bgLayers     = [];
        var ambientLayers = [];
        var regularLayers = [];

        for (var ci = 0; ci < precompsByDisplay.length; ci++) {
            var entry = precompsByDisplay[ci];
            if (/crypto/i.test(entry.name)) {
                skippedCryptos.push(entry.name);
            } else if (/^alpha$/i.test(entry.name)) {
                bgLayers.unshift(entry);     // alpha goes to the very bottom
            } else if (/^image$/i.test(entry.name)) {
                bgLayers.push(entry);        // image just above alpha
            } else if (/world|ambient|hdri/i.test(entry.name)) {
                ambientLayers.push(entry);
            } else {
                regularLayers.push(entry);
            }
        }

        sortRegular(regularLayers, sortMode, /* sidecar */ null);

        function addToMaster(entry, disable) {
            var lyr = master.layers.add(entry.comp);
            lyr.label = labeler(entry.name);
            lyr.blendingMode = BlendingMode.LIGHTEN;
            if (disable) lyr.enabled = false;
        }

        // layers.add() inserts at index 1, so add bottom-up.
        for (var b = 0; b < bgLayers.length; b++) addToMaster(bgLayers[b], true);
        for (var a = ambientLayers.length - 1; a >= 0; a--) addToMaster(ambientLayers[a], true);
        for (var r = regularLayers.length - 1; r >= 0; r--) addToMaster(regularLayers[r], false);

        master.openInViewer();

        if (skippedCryptos.length > 0) {
            skippedCryptos.reverse();
            var noun = skippedCryptos.length === 1 ? "layer was" : "layers were";
            alert(
                "EXRDemux: " + skippedCryptos.length + " " + noun +
                " excluded from the Master Comp because the name contains \"Crypto\":\n\n  • " +
                skippedCryptos.join("\n  • ") +
                "\n\nThe precomps still exist in the \"Layers Precomps\" folder. " +
                "If any of these are actually light passes (not cryptomattes), " +
                "drag them into the Master Comp manually.");
        }

        var msg = "EXRDemux: split " + displays.length + " layer" +
            (displays.length === 1 ? "" : "s") + " from\n" +
            footage.name + "\n" +
            (isStill ? "(still — " + STILL_DURATION_SEC + "s comps)"
                     : "(sequence — " + compDuration.toFixed(2) + "s comps)") +
            "\nSort mode: " + sortMode;
        $.writeln(msg);
    }

    var sortMode = chooseSortMode();
    if (sortMode === null) return;   // user cancelled; no undo group, no-op

    app.beginUndoGroup("EXRDemux: Split and Sort Passes to Precomps");
    try {
        main(sortMode);
    } catch (e) {
        alert("EXRDemux split failed:\n\n" + (e && e.message ? e.message : e));
    } finally {
        app.endUndoGroup();
    }
})();
