// macOS modal dialog implementation of the layer picker.
//
// Written blind on Windows — needs validation on a real Mac. Author
// notes for whoever runs this on the Mac mini:
//
//  - This is Objective-C++. CMake compiles it with the Apple clang
//    Obj-C++ toolchain. Don't include this file in any .cpp's
//    translation unit.
//  - Cocoa.framework + CoreFoundation are linked in CMakeLists.txt.
//  - Uses NSAlert with an NSPopUpButton accessory view. Adequate for
//    the ~60-layer use case; if the team wants a scrolling list with
//    type-ahead later, swap to NSWindow+NSTableView.
//  - When the plugin is loaded into AE, NSApp is the AE process —
//    [NSAlert runModal] should integrate cleanly with AE's run loop.
//
// See exrdemux_dialog.h for the cross-platform interface.

#import <Cocoa/Cocoa.h>

#include "exrdemux_dialog.h"

#include <string>
#include <vector>

int ShowLayerPickerDialog(const std::vector<std::string>& layers) {
    if (layers.empty()) return -1;

    __block int picked = -1;
    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Pick EXR Layer"];
        [alert setInformativeText:@"Choose which channel/layer this EXRDemux instance should render."];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        // NSPopUpButton sized to be readable. Long lists scroll natively.
        NSPopUpButton* popup =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26)
                                       pullsDown:NO];
        for (const auto& name : layers) {
            NSString* title = [NSString stringWithUTF8String:name.c_str()];
            if (title) {
                [popup addItemWithTitle:title];
            } else {
                // Fall back if UTF-8 conversion fails for some reason.
                [popup addItemWithTitle:@"(invalid)"];
            }
        }
        [popup selectItemAtIndex:0];

        [alert setAccessoryView:popup];

        NSModalResponse resp = [alert runModal];
        if (resp == NSAlertFirstButtonReturn) {
            NSInteger idx = [popup indexOfSelectedItem];
            if (idx >= 0 && static_cast<size_t>(idx) < layers.size()) {
                picked = static_cast<int>(idx);
            }
        }
    }
    return picked;
}
