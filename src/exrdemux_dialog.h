// Layer picker dialog — platform-abstracted interface.
//
// Implementations live in:
//   src/exrdemux_dialog_win.cpp  (Win32 modal dialog from the .rc template)
//   src/exrdemux_dialog_mac.mm   (AppKit NSAlert + NSPopUpButton)
//
// Returns the 0-based index of the picked layer in `layers`, or -1 if
// the user cancelled / closed the dialog.

#pragma once

#include <string>
#include <vector>

int ShowLayerPickerDialog(const std::vector<std::string>& layers);
