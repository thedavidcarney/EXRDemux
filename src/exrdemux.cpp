// EXRDemux — scriptable multilayer EXR plugin for Adobe After Effects.
//
// Phase 1: real out_flags + parameter structure (popup, hash, Black, White,
// UnMult). Render is a passthrough — Phase 2 wires up EXR reading and
// dynamic popup contents.
//
// Param layout:
//   [0] PARAM_INPUT  - implicit input layer (counted but not added)
//   [1] PARAM_LAYER  - popup of channel/layer choices (placeholder slots in v1)
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

#include <Imath/half.h>
#include <ImfMultiPartInputFile.h>

#define EXRDEMUX_MAJOR_VERSION 0
#define EXRDEMUX_MINOR_VERSION 1
#define EXRDEMUX_BUG_VERSION   0
#define EXRDEMUX_STAGE_VERSION PF_Stage_DEVELOP
#define EXRDEMUX_BUILD_VERSION 1

namespace {

enum {
    PARAM_INPUT = 0,
    PARAM_LAYER,
    PARAM_HASH,
    PARAM_BLACK,
    PARAM_WHITE,
    PARAM_UNMULT,
    PARAM_COUNT
};

// Param IDs (must be unique and stable across plugin versions for
// project file compatibility).
enum {
    ID_LAYER  = 1,
    ID_HASH   = 2,
    ID_BLACK  = 3,
    ID_WHITE  = 4,
    ID_UNMULT = 5
};

// Phase 1 placeholder popup contents. Phase 2 will replace this at
// PARAMS_SETUP time with the actual EXR's channel names.
constexpr const char* kPlaceholderChoices =
    "Channel 1|Channel 2|Channel 3|Channel 4|Channel 5|Channel 6|Channel 7|Channel 8|"
    "Channel 9|Channel 10|Channel 11|Channel 12|Channel 13|Channel 14|Channel 15|Channel 16";
constexpr int kPlaceholderNumChoices = 16;

void link_check_openexr() {
    Imath::half h(1.0f);
    (void)h;
}

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

    // [1] Layer popup — Phase 1 has placeholder labels. Phase 2 will
    // open the source EXR and populate real channel names here.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Layer",
                 kPlaceholderNumChoices,
                 1,                       // 1-based default
                 kPlaceholderChoices,
                 ID_LAYER);

    // [2] Hidden 32-bit hash of the selected channel's name. This is the
    // *real* persistence — the popup index is just for UI navigation.
    // INVISIBLE so the user never sees it; CANNOT_TIME_VARY so AE doesn't
    // try to keyframe it.
    AEFX_CLR_STRUCT(def);
    def.flags    = PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP;
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_FLOAT_SLIDERX("Layer Hash",
                         -1e10, 1e10,        // valid range
                         -1e10, 1e10,        // slider range (irrelevant; hidden)
                         0.0,                // default: 0 = unset
                         4,                  // precision
                         PF_ValueDisplayFlag_NONE,
                         PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP,
                         ID_HASH);

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
