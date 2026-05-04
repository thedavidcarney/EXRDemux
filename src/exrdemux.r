// EXRDemux PiPL — plugin metadata that After Effects reads on startup.
//
// This file is in Apple Resource format (.r). The build pipeline is:
//   1. cl /EP preprocesses this as C source into a .rr
//   2. PiPLtool.exe converts the .rr into a .rrc
//   3. cl /EP /D MSWindows preprocesses the .rrc into a Windows .rc
//   4. rc.exe compiles the .rc into a binary resource embedded in EXRDemux.aex
//
// Don't edit this file like normal C — the resource grammar is sensitive to
// braces, commas, and the embedded "Kind", "Name", etc. property keywords.

#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include <AE_General.r>
#endif

resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            "EXRDemux"
        },
        Category {
            "EXR"
        },

#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
        CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
        CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
        CodeMacIntel64 {"EffectMain"},
        CodeMacARM64 {"EffectMain"},
#endif

        AE_PiPL_Version {
            2,
            0
        },

        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },

        // Encoded via PF_VERSION layout: (major<<19) | (minor<<15) | (bug<<11)
        // | (stage<<9) | build.  Stage: DEVELOP=0, ALPHA=1, BETA=2, RELEASE=3.
        // Must match the runtime PF_VERSION(...) call in exrdemux.cpp or AE
        // refuses to load with "code/PiPL version mismatch".
        // Current: 0.1.0 develop build 1  =>  (1<<15) | 1  =>  0x8001 = 32769.
        AE_Effect_Version {
            0x8001
        },

        AE_Effect_Info_Flags {
            0
        },

        AE_Effect_Global_OutFlags {
            0x00000000
        },

        AE_Effect_Global_OutFlags_2 {
            0x00000000
        },

        AE_Effect_Match_Name {
            "io.github.thedavidcarney.EXRDemux"
        },

        AE_Reserved_Info {
            0
        },

        AE_Effect_Support_URL {
            "https://github.com/thedavidcarney/EXRDemux"
        }
    }
};
