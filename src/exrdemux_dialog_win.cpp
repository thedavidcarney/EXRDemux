// Win32 modal dialog implementation of the layer picker.
// Backed by the dialog template in exrdemux_dialog.rc.
//
// See exrdemux_dialog.h for the cross-platform interface.

#include "exrdemux_dialog.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace {

// Must match the IDs in src/exrdemux_dialog.rc.
constexpr int kIdDialog   = 101;
constexpr int kIdLayerList = 1001;

struct LayerDialogContext {
    const std::vector<std::string>* layers;
    int                              result;  // out: picked index or -1
};

INT_PTR CALLBACK LayerPickerDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static LayerDialogContext* ctx = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<LayerDialogContext*>(lp);
            HWND lb = GetDlgItem(hwnd, kIdLayerList);
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
            if (id == IDOK || (id == kIdLayerList && code == LBN_DBLCLK)) {
                HWND lb = GetDlgItem(hwnd, kIdLayerList);
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

}  // namespace

int ShowLayerPickerDialog(const std::vector<std::string>& layers) {
    if (layers.empty()) return -1;

    // Find the module that holds the dialog resource — this DLL.
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
        MAKEINTRESOURCEA(kIdDialog),
        GetActiveWindow(),
        LayerPickerDlgProc,
        reinterpret_cast<LPARAM>(&ctx));

    if (rc == -1) return -1;  // dialog creation failed
    return ctx.result;
}
