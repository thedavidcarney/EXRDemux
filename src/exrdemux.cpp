// EXRDemux — scriptable multilayer EXR plugin for Adobe After Effects.
//
// Architecture note (learned during Phase 2):
//
// AE's plugin lifecycle does NOT make the source layer's footage path
// available at PF_Cmd_PARAMS_SETUP. AEGP_GetEffectLayer returns null
// there. The SDK samples that use it (PathMaster, Resizer, SmartyPants)
// only call it during render-time selectors. AE's popup string list is
// also locked once PARAMS_SETUP returns — there is no API to update it.
//
// So the popup necessarily uses generic slot labels ("Layer 1", "Layer 2",
// ...). The user picks a slot; the plugin's "real" persistence is the
// hidden Layer Hash (Phase 3): when the popup changes, plugin captures the
// channel name at that slot from the current EXR, hashes it, and stores
// the hash. At render time we resolve hash -> name -> current channel
// index, so re-renders with shifted channels still hit the right pixels.
//
// Param layout:
//   [0] PARAM_INPUT  - implicit input layer (counted but not added)
//   [1] PARAM_LAYER  - popup of generic slot labels (locked at PARAMS_SETUP)
//   [2] PARAM_HASH   - hidden float slider; stores 32-bit hash of channel
//                      name (set in Phase 3, used at render in Phase 4)
//   [3] PARAM_BLACK  - Black Point (rescale floor)
//   [4] PARAM_WHITE  - White Point (rescale ceiling)
//   [5] PARAM_UNMULT - UnMult premultiplied alpha
//
// Param layout:
//   [0] PARAM_INPUT  - implicit input layer (counted but not added)
//   [1] PARAM_LAYER  - popup of layer names from the source EXR
//   [2] PARAM_HASH   - hidden float slider; stores 32-bit hash of channel name
//                      so selection survives EXR re-renders with shifted indices
//   [3] PARAM_BLACK  - Black Point (rescale floor)
//   [4] PARAM_WHITE  - White Point (rescale ceiling)
//   [5] PARAM_UNMULT - UnMult premultiplied alpha

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectUI.h"
#include "AE_GeneralPlug.h"
#include "Param_Utils.h"
#include "entry.h"
#include "AEFX_SuiteHandlerTemplate.h"
#include "AEGP_SuiteHandler.h"

#include <Imath/half.h>
#include <ImfMultiPartInputFile.h>
#include <ImfChannelList.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <set>

#ifdef AE_OS_WIN
    #include <Windows.h>
#endif

#define EXRDEMUX_MAJOR_VERSION 0
#define EXRDEMUX_MINOR_VERSION 1
#define EXRDEMUX_BUG_VERSION   0
#define EXRDEMUX_STAGE_VERSION PF_Stage_DEVELOP
#define EXRDEMUX_BUILD_VERSION 1

namespace {

enum {
    PARAM_INPUT = 0,
    PARAM_LAYER,
    PARAM_PICK_BUTTON,
    PARAM_HASH_HI,
    PARAM_HASH_LO,
    PARAM_BLACK,
    PARAM_WHITE,
    PARAM_UNMULT,
    PARAM_COUNT
};

// Param IDs (must be unique and stable across plugin versions for
// project file compatibility). Adding new params must use a fresh ID.
//
// The hash is split into two 16-bit halves because PF_Param_FLOAT_SLIDER
// round-trips through 32-bit float somewhere in AE's persistence path,
// and integers above ~16M lose precision. 16-bit halves stay exact.
enum {
    ID_LAYER       = 1,
    ID_BLACK       = 3,
    ID_WHITE       = 4,
    ID_UNMULT      = 5,
    ID_PICK_BUTTON = 6,
    ID_HASH_HI     = 7,
    ID_HASH_LO     = 8
    // ID 2 retired (was a single 32-bit hash, lost precision).
};

// Win32 dialog template constants (must match exrdemux_dialog.rc).
#ifdef AE_OS_WIN
    #define IDD_LAYER_PICKER 101
    #define IDC_LAYER_LIST   1001
#endif

// Static popup slot count. AE locks the popup's choice list at
// PARAMS_SETUP and provides no API to mutate it later, so the visible
// labels are generic ("Layer 1"..."Layer 256"). The Pick Layer button
// shows real names via a native modal; the popup is the scriptable
// backing store.
constexpr int kLayerSlotCount = 256;

// Build the pipe-separated label list once. 256 slots, "Layer 1".."Layer 256".
// C++11 magic-static init is thread-safe; the buffer's lifetime is the
// process so the pointer we hand AE stays valid forever.
const char* StaticSlotLabels() {
    static const std::string buf = []{
        std::string s;
        s.reserve(kLayerSlotCount * 12);
        for (int i = 1; i <= kLayerSlotCount; ++i) {
            if (i > 1) s += '|';
            s += "Layer ";
            s += std::to_string(i);
        }
        return s;
    }();
    return buf.c_str();
}

void link_check_openexr() {
    Imath::half h(1.0f);
    (void)h;
}

// FNV-1a 32-bit hash. Used to persist channel-name selection in a
// scriptable numeric param so the choice survives re-renders that shift
// the EXR's channel ordering (the original user-pain motivating this
// project). Must match the JS implementation in
// tests/jsx/exrdemux_helpers.jsx so JSX automation can compute the
// same hash without going through the dialog.
uint32_t HashLayerName(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// Get the file path of the EXR backing the layer this effect is applied to.
// Returns empty string on any failure. Writes a human-readable diagnostic
// describing which step failed to *diag_out (so the popup can surface it
// during early development).
std::string GetSourceExrPath(PF_InData* in_data, std::string* diag_out) {
    auto fail = [&](const char* msg) -> std::string {
        if (diag_out) *diag_out = msg;
        return "";
    };

    if (!in_data)                     return fail("no in_data");
    if (!in_data->effect_ref)         return fail("no effect_ref");
    if (!in_data->pica_basicP)        return fail("no pica_basicP");

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    AEGP_LayerH layer = nullptr;
    A_Err err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
        in_data->effect_ref, &layer);
    if (err)    return fail("AEGP_GetEffectLayer error");
    if (!layer) return fail("AEGP_GetEffectLayer null");

    AEGP_ItemH item = nullptr;
    err = suites.LayerSuite7()->AEGP_GetLayerSourceItem(layer, &item);
    if (err)   return fail("AEGP_GetLayerSourceItem error");
    if (!item) return fail("AEGP_GetLayerSourceItem null");

    AEGP_FootageH footage = nullptr;
    err = suites.FootageSuite5()->AEGP_GetMainFootageFromItem(item, &footage);
    if (err)      return fail("AEGP_GetMainFootageFromItem error");
    if (!footage) return fail("AEGP_GetMainFootageFromItem null");

    AEGP_MemHandle path_handle = nullptr;
    err = suites.FootageSuite5()->AEGP_GetFootagePath(
        footage, 0, AEGP_FOOTAGE_MAIN_FILE_INDEX, &path_handle);
    if (err)          return fail("AEGP_GetFootagePath error");
    if (!path_handle) return fail("AEGP_GetFootagePath null");

    A_UTF16Char* utf16 = nullptr;
    suites.MemorySuite1()->AEGP_LockMemHandle(
        path_handle, reinterpret_cast<void**>(&utf16));

    std::string result;
    if (utf16) {
#ifdef AE_OS_WIN
        const wchar_t* wide = reinterpret_cast<const wchar_t*>(utf16);
        int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            result.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], n, nullptr, nullptr);
        }
#else
        (void)utf16;  // TODO(mac)
#endif
    }

    suites.MemorySuite1()->AEGP_UnlockMemHandle(path_handle);
    suites.MemorySuite1()->AEGP_FreeMemHandle(path_handle);

    if (result.empty() && diag_out) *diag_out = "path empty after lock";
    return result;
}

// Group the EXR's channels into "layers". Iterates all parts (Blender's
// newer multilayer output may be multipart). For each channel name, the
// "layer" is everything before the last dot. Bare channels (no dot, e.g.
// plain "R") collapse into "(default)". Optional all_channels output
// captures every raw channel name for diagnostics.
std::vector<std::string> EnumerateExrLayers(
    const std::string& path,
    std::vector<std::string>* all_channels = nullptr,
    int* part_count = nullptr)
{
    std::vector<std::string> layers;
    if (path.empty()) return layers;

    try {
        Imf::MultiPartInputFile file(path.c_str());
        if (part_count) *part_count = file.parts();
        if (file.parts() == 0) return layers;

        std::set<std::string> seen;
        bool has_default = false;

        for (int p = 0; p < file.parts(); ++p) {
            const Imf::ChannelList& channels = file.header(p).channels();
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                std::string name = it.name();
                if (all_channels) all_channels->push_back(name);

                size_t dot = name.find_last_of('.');
                if (dot == std::string::npos) {
                    has_default = true;
                } else {
                    std::string layer = name.substr(0, dot);
                    if (seen.insert(layer).second) {
                        layers.push_back(layer);
                    }
                }
            }
        }

        if (has_default) {
            layers.insert(layers.begin(), "(default)");
        }
    } catch (const std::exception&) {
        layers.clear();
    }
    return layers;
}

// ===== Modal "Pick Layer" dialog (Windows) ======================
//
// Shows a centered modal listbox of all layer names. Returns the picked
// 0-based index, or -1 if cancelled. Win32-specific for now; Mac path
// will use NSPanel/NSAlert later.

#ifdef AE_OS_WIN

struct LayerDialogContext {
    const std::vector<std::string>* layers;
    int                              result;  // out
};

INT_PTR CALLBACK LayerPickerDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static LayerDialogContext* ctx = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<LayerDialogContext*>(lp);
            HWND lb = GetDlgItem(hwnd, IDC_LAYER_LIST);
            for (const auto& name : *ctx->layers) {
                SendMessageA(lb, LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(name.c_str()));
            }
            SendMessage(lb, LB_SETCURSEL, 0, 0);
            return TRUE;
        }
        case WM_COMMAND: {
            int id   = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IDOK || (id == IDC_LAYER_LIST && code == LBN_DBLCLK)) {
                HWND lb = GetDlgItem(hwnd, IDC_LAYER_LIST);
                LRESULT sel = SendMessage(lb, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR && ctx) ctx->result = static_cast<int>(sel);
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                if (ctx) ctx->result = -1;
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            if (ctx) ctx->result = -1;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

int ShowLayerPickerDialog(const std::vector<std::string>& layers) {
    if (layers.empty()) return -1;

    // Find the module that contains our dialog resource (this DLL).
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ShowLayerPickerDialog),
        &module);

    LayerDialogContext ctx;
    ctx.layers = &layers;
    ctx.result = -1;

    INT_PTR rc = DialogBoxParamA(
        module,
        MAKEINTRESOURCEA(IDD_LAYER_PICKER),
        GetActiveWindow(),
        LayerPickerDlgProc,
        reinterpret_cast<LPARAM>(&ctx));

    if (rc == -1) return -1;  // dialog creation failed
    return ctx.result;
}

#else  // AE_OS_WIN

int ShowLayerPickerDialog(const std::vector<std::string>& /*layers*/) {
    return -1;  // TODO(mac)
}

#endif

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data,
                   PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    out_data->my_version = PF_VERSION(
        EXRDEMUX_MAJOR_VERSION,
        EXRDEMUX_MINOR_VERSION,
        EXRDEMUX_BUG_VERSION,
        EXRDEMUX_STAGE_VERSION,
        EXRDEMUX_BUILD_VERSION);

    // 16bpc + 32bpc float, Smart Render for the float path, MFR-safe.
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE
                         | PF_OutFlag_PIX_INDEPENDENT;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE
                         | PF_OutFlag2_SUPPORTS_SMART_RENDER
                         | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data,
                   PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    PF_Err     err = PF_Err_NONE;
    PF_ParamDef def;

    // [1] Layer popup. Hidden from the Effect Controls panel — its only
    // job is to be the JSX-scriptable backing store for the slot index.
    // (AE's popup string list is locked at PARAMS_SETUP and can't be
    // updated to real layer names; we surface real names via the button
    // below + dynamic display-name updates.)
    AEFX_CLR_STRUCT(def);
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_POPUP("Layer",
                 kLayerSlotCount,
                 1,                       // 1-based default
                 StaticSlotLabels(),
                 ID_LAYER);

    // [2] Pick Layer button — opens a native modal listbox of real names.
    // SUPERVISE flag tells AE to send PF_Cmd_USER_CHANGED_PARAM on click.
    // The param's display name (left of the button) is updated dynamically
    // to "Layer: <selected layer>" after each pick.
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_BUTTON;
    def.flags      = PF_ParamFlag_SUPERVISE;
    PF_STRNNCPY(def.PF_DEF_NAME, "Layer", sizeof(def.PF_DEF_NAME));
    def.u.button_d.u.PF_DEF_NAMESPTR = "Pick Layer...";
    def.uu.id = ID_PICK_BUTTON;
    if (PF_Err pe = PF_ADD_PARAM(in_data, -1, &def)) return pe;

    // [3,4] Layer-name hash split into hi/lo 16-bit halves. Stored as two
    // float sliders so they survive AE's float32 persistence quantization
    // (a single 32-bit hash slider rounds to the nearest representable
    // float and corrupts integer hashes above ~16M). Hidden from UI;
    // populated on Pick Layer click and read at render time.
    AEFX_CLR_STRUCT(def);
    def.flags    = PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP;
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_FLOAT_SLIDERX("Layer Hash Hi",
                         0, 65535, 0, 65535, 0.0, 0,
                         PF_ValueDisplayFlag_NONE,
                         PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP,
                         ID_HASH_HI);

    AEFX_CLR_STRUCT(def);
    def.flags    = PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP;
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_FLOAT_SLIDERX("Layer Hash Lo",
                         0, 65535, 0, 65535, 0.0, 0,
                         PF_ValueDisplayFlag_NONE,
                         PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP,
                         ID_HASH_LO);

    // [3] Black Point — same intent as EXtractoR's Black Point.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Black Point",
                         -1e10, 1e10,
                         -1.0, 1.0,
                         0.0,
                         4,
                         PF_ValueDisplayFlag_NONE,
                         0,
                         ID_BLACK);

    // [4] White Point — same intent as EXtractoR's White Point.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("White Point",
                         -1e10, 1e10,
                         -1.0, 1.0,
                         1.0,
                         4,
                         PF_ValueDisplayFlag_NONE,
                         0,
                         ID_WHITE);

    // [5] UnMult — same intent as EXtractoR's UnMult.
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("UnMult", "", FALSE, 0, ID_UNMULT);

    out_data->num_params = PARAM_COUNT;
    return err;
}

// Legacy 8/16bpc render path. Plain passthrough for now.
PF_Err Render(PF_InData* in_data, PF_OutData* /*out_data*/,
              PF_ParamDef* params[], PF_LayerDef* output) {
    link_check_openexr();
    PF_LayerDef* input = &params[PARAM_INPUT]->u.ld;
    return PF_COPY(input, output, NULL, NULL);
}

// Called when the user clicks our "Pick Layer..." button (or interacts
// with any SUPERVISE-flagged param). We open a modal layer picker, and
// on a confirmed selection we update the Layer popup's display name to
// show the chosen layer name plus stash the index back into the popup.
PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* /*out_data*/,
                        PF_ParamDef* params[],
                        const PF_UserChangedParamExtra* which) {
    if (!which || which->param_index != PARAM_PICK_BUTTON) {
        return PF_Err_NONE;
    }

    // Read the source EXR fresh on each click. PF_ProgPtr isn't stable
    // across selectors so we can't reuse the SEQUENCE_SETUP cache key;
    // header parse is microseconds and only runs on user interaction.
    std::string diag;
    std::string path = GetSourceExrPath(in_data, &diag);
    std::vector<std::string> layers;
    if (!path.empty()) layers = EnumerateExrLayers(path);
    if (layers.empty()) return PF_Err_NONE;

    int picked = ShowLayerPickerDialog(layers);
    if (picked < 0 || picked >= static_cast<int>(layers.size())) {
        return PF_Err_NONE;  // user cancelled
    }

    const std::string& name = layers[picked];
    uint32_t hash = HashLayerName(name);

    // Persist:
    //   - PARAM_HASH_HI / PARAM_HASH_LO (FNV-1a 32-bit name hash, split
    //     across two float sliders to survive AE's float32 persistence)
    //   - PARAM_LAYER (1-based popup index, scriptable convenience)
    if (params && params[PARAM_LAYER]) {
        params[PARAM_LAYER]->u.pd.value = static_cast<short>(picked + 1);
        params[PARAM_LAYER]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }
    if (params && params[PARAM_HASH_HI]) {
        params[PARAM_HASH_HI]->u.fs_d.value =
            static_cast<double>((hash >> 16) & 0xFFFFu);
        params[PARAM_HASH_HI]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }
    if (params && params[PARAM_HASH_LO]) {
        params[PARAM_HASH_LO]->u.fs_d.value =
            static_cast<double>(hash & 0xFFFFu);
        params[PARAM_HASH_LO]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }

    // Update the button's left-side display name to "Layer: <selected>"
    // so the user can see the current selection at a glance without
    // re-opening the dialog. The button's face text ("Pick Layer...")
    // must be preserved explicitly — AEFX_CLR_STRUCT zeroes the union
    // and AE reads the (now-null) namesptr otherwise, blanking the button.
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_BUTTON;
    def.u.button_d.u.PF_DEF_NAMESPTR = "Pick Layer...";
    std::string new_name = "Layer: " + name;
    PF_STRNNCPY(def.PF_DEF_NAME, new_name.c_str(), sizeof(def.PF_DEF_NAME));
    suites.ParamUtilsSuite3()->PF_UpdateParamUI(
        in_data->effect_ref, PARAM_PICK_BUTTON, &def);

    return PF_Err_NONE;
}

// SEQUENCE_SETUP runs once per effect instance. We don't strictly need
// it any more — UserChangedParam reads the EXR fresh on each click —
// but keeping it as a no-op stub means future phases (cache, sequence
// state for the hash-resolution render path) have a hook to extend.
PF_Err SequenceSetup(PF_InData* /*in_data*/, PF_OutData* /*out_data*/) {
    return PF_Err_NONE;
}

// Smart Render: required for 32bpc float color (PF_OutFlag2_FLOAT_COLOR_AWARE).
// AE calls PreRender first to declare needs, then SmartRender to do the work.
PF_Err PreRender(PF_InData* in_data, PF_OutData* /*out_data*/,
                 PF_PreRenderExtra* extra) {
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    PF_Err err = extra->cb->checkout_layer(
        in_data->effect_ref,
        PARAM_INPUT,
        PARAM_INPUT,
        &req,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &in_result);
    if (err) return err;

    extra->output->result_rect     = in_result.result_rect;
    extra->output->max_result_rect = in_result.max_result_rect;
    return PF_Err_NONE;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* /*out_data*/,
                   PF_SmartRenderExtra* extra) {
    link_check_openexr();

    PF_EffectWorld* input_worldP  = nullptr;
    PF_EffectWorld* output_worldP = nullptr;

    PF_Err err = extra->cb->checkout_layer_pixels(
        in_data->effect_ref, PARAM_INPUT, &input_worldP);
    if (err) return err;

    err = extra->cb->checkout_output(in_data->effect_ref, &output_worldP);
    if (err) return err;

    if (!input_worldP || !output_worldP) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    // Phase 1 passthrough: copy input world to output world.
    return (*in_data->utils->copy)(
        in_data->effect_ref, input_worldP, output_worldP, nullptr, nullptr);
}

}  // namespace

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;
    // matchName: AE caps at 31 chars (PF_MAX_EFFECT_NAME_LEN). Must match
    // the AE_Effect_Match_Name in exrdemux.r exactly. Stable across releases.
    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "EXRDemux",
        "tdcarney EXRDemux",
        "EXR",
        AE_RESERVED_INFO,
        "EffectMain",
        "https://github.com/thedavidcarney/EXRDemux");
    return result;
}

extern "C" DllExport
PF_Err EffectMain(
    PF_Cmd cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra)
{
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_SETUP:
                err = SequenceSetup(in_data, out_data);
                break;
            case PF_Cmd_USER_CHANGED_PARAM:
                err = UserChangedParam(in_data, out_data, params,
                    reinterpret_cast<const PF_UserChangedParamExtra*>(extra));
                break;
            case PF_Cmd_RENDER:
                err = Render(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data,
                                reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data,
                                  reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
            default:
                break;
        }
    } catch (const PF_Err& thrown) {
        err = thrown;
    }
    return err;
}
