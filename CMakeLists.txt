cmake_minimum_required(VERSION 3.20)

# FUSERNativeLoader: the FUSER mod loader (winhttp.dll proxy) with the fue reflection core and the FuserNative
# features built in. build.ps1 drives this and also packs FUSERNativeMods_P.pak; see README.md.
project(FUSERNativeLoader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FNL_DEBUG: the diagnostic build - a console with the loader's tracing, the F6-F10 debug keys and the probes.
option(FNL_DEBUG "Diagnostic build of winhttp.dll (console, debug keys, probes)" OFF)

add_library(FUSERNativeLoader SHARED
    src/dllmain.cpp
    src/winhttp_proxy.cpp
    src/fnl_entry.cpp
    src/unlocks.cpp
    src/data_table.cpp
    src/custom_content.cpp
    src/content/content_types.cpp
    src/content/content_unlocks.cpp
    src/content/content_projector.cpp
    src/content/content_outfit.cpp
    src/fue/fue.cpp
    src/native/fusernative.cpp
    src/debug/unlock_probe.cpp
)
set_target_properties(FUSERNativeLoader PROPERTIES OUTPUT_NAME "winhttp")
set_target_properties(FUSERNativeLoader PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(FUSERNativeLoader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_link_libraries(FUSERNativeLoader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/funchook.lib")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FNL_VERSION LIMIT_COUNT 1)
target_compile_definitions(FUSERNativeLoader PRIVATE UNICODE FNL_DEBUG=$<BOOL:${FNL_DEBUG}> FNL_VERSION="${FNL_VERSION}")
target_compile_options(FUSERNativeLoader PRIVATE /utf-8 /EHsc /Zi) # /Zi: a PDB for crash and stall stacks
target_link_options(FUSERNativeLoader PRIVATE /DEBUG /OPT:REF /OPT:ICF /IGNORE:4099) # 4099: the prebuilt funchook.lib has no PDB

# fue and the code on it are C++20 (std::format); the original loader source (dllmain.cpp) stays C++17.
set_source_files_properties(src/fnl_entry.cpp src/unlocks.cpp src/data_table.cpp src/custom_content.cpp src/content/content_types.cpp
                            src/content/content_unlocks.cpp src/content/content_projector.cpp src/content/content_outfit.cpp src/fue/fue.cpp src/native/fusernative.cpp src/debug/unlock_probe.cpp
                            PROPERTIES COMPILE_OPTIONS "/std:c++20")

# Optional multiplayer extension. It uses the core's fue hook dispatcher and reflection API,
# so there is one owner for UObject::ProcessEvent and UGameEngine::Tick in each process.
add_library(FUSERMultiplayer SHARED
    src/multiplayer/plugin_entry.cpp
    src/multiplayer/host_hook.cpp
    src/multiplayer/p2p_tunnel.cpp
    src/multiplayer/pak_check.cpp
    src/multiplayer/community_fetch.cpp
    src/multiplayer/server_content.cpp
)
set_target_properties(FUSERMultiplayer PROPERTIES OUTPUT_NAME "FUSERMultiplayer")
target_include_directories(FUSERMultiplayer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/lib")
# libarchive + zlib (static, tools/build_libarchive.ps1): extracting community song archives
target_link_libraries(FUSERMultiplayer PRIVATE FUSERNativeLoader "${CMAKE_CURRENT_SOURCE_DIR}/lib/funchook.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libarchive/archive.lib" "${CMAKE_CURRENT_SOURCE_DIR}/lib/libarchive/zlib.lib" Bcrypt)
# FNL_DEBUG: trace hooks and debug keys. FNL_VERSION: reported to fuserd (the stock / modded split).
target_compile_definitions(FUSERMultiplayer PRIVATE UNICODE FNL_DEBUG=$<BOOL:${FNL_DEBUG}> FNL_VERSION="${FNL_VERSION}")
target_compile_options(FUSERMultiplayer PRIVATE /utf-8 /EHsc /Zi /std:c++20)
target_link_options(FUSERMultiplayer PRIVATE /DEBUG /OPT:REF /OPT:ICF /IGNORE:4099)
