// EXRDemux — scriptable multilayer EXR plugin for Adobe After Effects.
//
// Phase 1 of bring-up: minimal effect plugin entry point. Goal here is
// only to prove the toolchain compiles and links — AE SDK headers + OpenEXR
// linkage. No real plugin behavior yet.

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectUI.h"
#include "AE_GeneralPlug.h"
#include "entry.h"

#include <Imath/half.h>
#include <ImfMultiPartInputFile.h>

#define EXRDEMUX_MAJOR_VERSION 0
#define EXRDEMUX_MINOR_VERSION 1
#define EXRDEMUX_BUG_VERSION   0
#define EXRDEMUX_STAGE_VERSION PF_Stage_DEVELOP
#define EXRDEMUX_BUILD_VERSION 1

namespace {

void link_check_openexr() {
    Imath::half h(1.0f);
    (void)h;
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

    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "EXRDemux",
        "io.github.thedavidcarney.EXRDemux",
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
    link_check_openexr();

    PF_Err err = PF_Err_NONE;
    switch (cmd) {
        case PF_Cmd_GLOBAL_SETUP:
            out_data->my_version = PF_VERSION(
                EXRDEMUX_MAJOR_VERSION,
                EXRDEMUX_MINOR_VERSION,
                EXRDEMUX_BUG_VERSION,
                EXRDEMUX_STAGE_VERSION,
                EXRDEMUX_BUILD_VERSION);
            out_data->out_flags = 0;
            break;

        case PF_Cmd_PARAMS_SETUP:
        case PF_Cmd_RENDER:
        case PF_Cmd_ABOUT:
        default:
            break;
    }
    return err;
}
