cmake_minimum_required(VERSION 3.25)

# Enable Objective-C++ when building on macOS so .mm files compile.
if(APPLE)
    project(EXRDemux LANGUAGES C CXX OBJC OBJCXX VERSION 0.1.0)
else()
    project(EXRDemux LANGUAGES C CXX VERSION 0.1.0)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match vcpkg's runtime: dynamic CRT (/MD or /MDd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# macOS deployment target. arm64 minimum is 11.0 (Big Sur, first OS
# with Apple Silicon support); 10.15 is moot for our arm64-only build.
# Must match VCPKG_OSX_DEPLOYMENT_TARGET in cmake/overlay-triplets/
# arm64-osx.cmake or vcpkg's static libs will be built against a
# different (typically newer) SDK and reference symbols missing on
# older macOS.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0"
        CACHE STRING "Minimum macOS version" FORCE)
endif()

# ----- Dependencies -----
find_package(OpenEXR CONFIG REQUIRED)
find_package(Imath CONFIG REQUIRED)

# ----- AE SDK location (gitignored, dropped in by user per README) -----
set(AE_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/AfterEffectsSDK")
if(WIN32)
    set(AE_SDK_PLATFORM_DIR
        "${AE_SDK_ROOT}/Win/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK")
elseif(APPLE)
    # Mac SDK extraction layout TBD; CI will need to set this.
    set(AE_SDK_PLATFORM_DIR
        "${AE_SDK_ROOT}/Mac/AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK")
else()
    message(FATAL_ERROR "Unsupported platform; AE plugins are Windows + macOS only.")
endif()

set(AE_SDK_HEADERS   "${AE_SDK_PLATFORM_DIR}/Examples/Headers")
set(AE_SDK_UTIL      "${AE_SDK_PLATFORM_DIR}/Examples/Util")
set(AE_SDK_RESOURCES "${AE_SDK_PLATFORM_DIR}/Examples/Resources")

if(NOT EXISTS "${AE_SDK_HEADERS}/AE_Effect.h")
    message(FATAL_ERROR
        "After Effects SDK headers not found at:\n"
        "  ${AE_SDK_HEADERS}\n"
        "See README.md for SDK setup instructions.")
endif()

# ----- Plugin target -----
add_library(EXRDemux MODULE
    src/exrdemux.cpp
    "${AE_SDK_UTIL}/AEGP_SuiteHandler.cpp"
    "${AE_SDK_UTIL}/AEFX_SuiteHelper.c"
    "${AE_SDK_UTIL}/MissingSuiteError.cpp"
)

target_include_directories(EXRDemux PRIVATE
    "${AE_SDK_HEADERS}"
    "${AE_SDK_HEADERS}/SP"
    "${AE_SDK_RESOURCES}"
    "${AE_SDK_UTIL}"
    src
)

target_link_libraries(EXRDemux PRIVATE
    OpenEXR::OpenEXR
    Imath::Imath
)

if(WIN32)
    target_include_directories(EXRDemux PRIVATE "${AE_SDK_HEADERS}/Win")
    target_compile_definitions(EXRDemux PRIVATE MSWindows WIN32 _WINDOWS)
    set_target_properties(EXRDemux PROPERTIES
        SUFFIX ".aex"
        PREFIX ""
    )

    # ----- PiPL build pipeline (Windows) -----
    # Apple Resource (.r) -> .rr -> PiPLtool -> .rrc -> Windows .rc
    set(PIPL_INPUT  "${CMAKE_SOURCE_DIR}/src/exrdemux.r")
    set(PIPL_RR     "${CMAKE_BINARY_DIR}/exrdemux.rr")
    set(PIPL_RRC    "${CMAKE_BINARY_DIR}/exrdemux.rrc")
    set(PIPL_RC     "${CMAKE_BINARY_DIR}/exrdemux.rc")
    set(PIPLTOOL    "${AE_SDK_RESOURCES}/PiPLtool.exe")

    if(NOT EXISTS "${PIPLTOOL}")
        message(FATAL_ERROR "PiPLtool.exe not found at ${PIPLTOOL}")
    endif()

    # Need cmd.exe for shell redirect; CMake's command runner doesn't redirect.
    add_custom_command(
        OUTPUT "${PIPL_RC}"
        DEPENDS "${PIPL_INPUT}"
        COMMAND cmd.exe /c "cl.exe /nologo /I \"${AE_SDK_HEADERS}\" /EP \"${PIPL_INPUT}\" > \"${PIPL_RR}\""
        COMMAND "${PIPLTOOL}" "${PIPL_RR}" "${PIPL_RRC}"
        COMMAND cmd.exe /c "cl.exe /nologo /D MSWindows /EP \"${PIPL_RRC}\" > \"${PIPL_RC}\""
        COMMENT "Compiling PiPL"
    )

    target_sources(EXRDemux PRIVATE "${PIPL_RC}")

    # Win-side dialog: hand-authored Win32 dialog template + impl.
    target_sources(EXRDemux PRIVATE
        "${CMAKE_SOURCE_DIR}/src/exrdemux_dialog.rc"
        "${CMAKE_SOURCE_DIR}/src/exrdemux_dialog_win.cpp"
    )

elseif(APPLE)
    target_include_directories(EXRDemux PRIVATE "${AE_SDK_HEADERS}/Mac")
    target_compile_definitions(EXRDemux PRIVATE macintosh)

    # Mac-side dialog: AppKit modal in an Objective-C++ source file.
    target_sources(EXRDemux PRIVATE
        "${CMAKE_SOURCE_DIR}/src/exrdemux_dialog_mac.mm"
    )

    # Build a proper .plugin bundle (not a bare .dylib). AE looks for
    # CFBundlePackageType = "eFKT" inside the bundle's Info.plist.
    set_target_properties(EXRDemux PROPERTIES
        BUNDLE                    TRUE
        BUNDLE_EXTENSION          "plugin"
        OUTPUT_NAME               "EXRDemux"
        MACOSX_BUNDLE_INFO_PLIST  "${CMAKE_SOURCE_DIR}/cmake/MacInfoPlist.in"
        MACOSX_BUNDLE_BUNDLE_NAME            "EXRDemux"
        MACOSX_BUNDLE_GUI_IDENTIFIER         "io.github.thedavidcarney.EXRDemux"
        MACOSX_BUNDLE_BUNDLE_VERSION         "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING   "${PROJECT_VERSION}"
    )

    # Frameworks: AppKit (Cocoa) for the dialog, CoreFoundation for the
    # A_UTF16Char path conversion in GetSourceExrPath().
    target_link_libraries(EXRDemux PRIVATE
        "-framework Cocoa"
        "-framework CoreFoundation"
    )

    # ----- PiPL build pipeline (macOS) -----
    # Same source .r file as Windows; compiled by Apple's Rez to a
    # .rsrc that ends up at Contents/Resources/EXRDemux.rsrc inside
    # the bundle.
    set(PIPL_INPUT "${CMAKE_SOURCE_DIR}/src/exrdemux.r")
    set(PIPL_RSRC  "${CMAKE_BINARY_DIR}/EXRDemux.rsrc")

    find_program(REZ_EXECUTABLE
        NAMES Rez
        PATHS /usr/bin /Applications/Xcode.app/Contents/Developer/usr/bin
        DOC "Apple Rez resource compiler (ships with Xcode CLT)")
    if(NOT REZ_EXECUTABLE)
        message(FATAL_ERROR
            "Rez not found. Install Xcode Command Line Tools (xcode-select --install).")
    endif()

    # Rez doesn't auto-define AE_OS_MAC, so pass it explicitly. The
    # SDK's AE_General.r is the macOS-side Resource header with type
    # definitions; -i adds the include path.
    # -useDF: write the resource map into the data fork. Without it Rez
    # uses the legacy HFS resource fork, which file copies and the cmake
    # bundle install both strip — leaving an empty .rsrc in the bundle.
    add_custom_command(
        OUTPUT "${PIPL_RSRC}"
        DEPENDS "${PIPL_INPUT}"
        COMMAND "${REZ_EXECUTABLE}"
                -useDF
                -d "AE_OS_MAC"
                -d "ARCH_64"
                -i "${AE_SDK_HEADERS}"
                -i "${AE_SDK_RESOURCES}"
                -o "${PIPL_RSRC}"
                "${PIPL_INPUT}"
        COMMENT "Compiling PiPL via Rez"
        VERBATIM)

    set_source_files_properties("${PIPL_RSRC}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
        GENERATED TRUE)
    target_sources(EXRDemux PRIVATE "${PIPL_RSRC}")
endif()

# Quiet a few MSVC warnings the SDK headers trip but we can't fix.
if(MSVC)
    target_compile_options(EXRDemux PRIVATE /W3)
endif()
