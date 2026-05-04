cmake_minimum_required(VERSION 3.25)
project(EXRDemux LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match vcpkg's runtime: dynamic CRT (/MD or /MDd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

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
elseif(APPLE)
    target_include_directories(EXRDemux PRIVATE "${AE_SDK_HEADERS}/Mac")
    set_target_properties(EXRDemux PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
    )
endif()

# Quiet a few MSVC warnings the SDK headers trip but we can't fix.
if(MSVC)
    target_compile_options(EXRDemux PRIVATE /W3)
endif()
