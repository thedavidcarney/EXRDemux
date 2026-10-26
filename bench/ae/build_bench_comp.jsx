// Builds the EXRDemux many-layers benchmark comp and saves it.
// Run in AE (File > Scripts > Run Script File, or AfterFX -r) with
//   $.global.EXRDEMUX_BENCH = { exr: "...", aep: "...", layers: 120, passes: 30, frames: 10 };
// set first, or edit the defaults below. Every layer is a full-frame
// EXRDemux of one pass of the same multipart EXR, stacked with Add, the
// way a chase comp is built.
(function () {
    var cfg = $.global.EXRDEMUX_BENCH || {};
    var exrPath = cfg.exr, aepPath = cfg.aep;
    var nLayers = cfg.layers || 120, nPasses = cfg.passes || 30, nFrames = cfg.frames || 10;

    function fnv1a(s) {               // must match HashLayerName in exrdemux.cpp
        var h = 0x811c9dc5;
        for (var i = 0; i < s.length; i++) {
            h ^= s.charCodeAt(i) & 0xff;
            h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
        }
        return h >>> 0;
    }

    app.newProject();
    app.project.bitsPerChannel = 32;
    var item = app.project.importFile(new ImportOptions(new File(exrPath)));
    var fps = 30;
    var comp = app.project.items.addComp("EXRDemux bench", item.width, item.height, 1,
                                         nFrames / fps, fps);
    for (var i = 0; i < nLayers; i++) {
        var pass = "Light_" + ("0" + (i % nPasses)).slice(-2);
        var hash = fnv1a(pass);
        var L = comp.layers.add(item);
        L.blendingMode = BlendingMode.ADD;
        var fx = L.property("ADBE Effect Parade").addProperty("tdcarney EXRDemux");
        fx.property("Layer Hash Hi").setValue(Math.floor(hash / 65536));
        fx.property("Layer Hash Lo").setValue(hash % 65536);
        L.name = pass + " #" + i;
    }
    // 32-bit float EXR output so before/after frames can be compared bit
    // for bit. setSettings needs AE 2023+.
    var rq = app.project.renderQueue.items.add(comp);
    var om = rq.outputModule(1);
    om.setSettings({ "Format": "OpenEXR" });
    om.setSettings({ "Video Output": { "Depth": "Floating Point", "Channels": "RGB + Alpha" } });
    if (cfg.out) om.setSettings({ "Output File Info": { "Full Flat Path": cfg.out } });
    app.project.save(new File(aepPath));
    return comp.numLayers;
})();
