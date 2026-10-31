cmake_minimum_required(VERSION 3.20)

# FUSERNativeLoader: the FUSER mod loader (winhttp.dll proxy) with the fue reflection core, the Mods tab and custom
# content, and the bundled mods it loads from mods/ (sdk/fnl_mod.h). build.ps1 drives this and also packs the mods'
# paks; see README.md.
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
    src/debug/unlock_probe.cpp
    src/pak_diag.cpp
    src/loader/code_patch.cpp
    src/loader/mod_manager.cpp
    src/loader/settings.cpp
)
set_target_properties(FUSERNativeLoader PROPERTIES OUTPUT_NAME "winhttp")
target_include_directories(FUSERNativeLoader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_link_libraries(FUSERNativeLoader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/funchook.lib")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FNL_VERSION LIMIT_COUNT 1)
# FNL_LOADER: the sdk/ headers export their FNL_API declarations (the C++ tier) instead of importing them
target_compile_definitions(FUSERNativeLoader PRIVATE UNICODE FNL_LOADER FNL_DEBUG=$<BOOL:${FNL_DEBUG}> FNL_VERSION="${FNL_VERSION}")
target_compile_options(FUSERNativeLoader PRIVATE /utf-8 /EHsc /Zi) # /Zi: a PDB for crash and stall stacks
target_link_options(FUSERNativeLoader PRIVATE /DEBUG /OPT:REF /OPT:ICF /IGNORE:4099) # 4099: the prebuilt funchook.lib has no PDB

# fue and the code on it are C++20 (std::format); the original loader source (dllmain.cpp) stays C++17.
set_source_files_properties(src/fnl_entry.cpp src/unlocks.cpp src/data_table.cpp src/custom_content.cpp src/content/content_types.cpp
                            src/content/content_unlocks.cpp src/content/content_projector.cpp src/content/content_outfit.cpp src/fue/fue.cpp src/debug/unlock_probe.cpp
                            src/pak_diag.cpp src/loader/code_patch.cpp src/loader/mod_manager.cpp src/loader/settings.cpp PROPERTIES COMPILE_OPTIONS "/std:c++20")

# ---- bundled mods (mods/<Name>/, each with the mod.ini in its source folder; build.ps1 lays them out)
# C++ tier mods use the core's fue hook dispatcher and reflection API (linked from winhttp.dll), so there is one owner
# for UObject::ProcessEvent and UGameEngine::Tick in each process. FNL_DEBUG: trace hooks and debug keys.
# FNL_VERSION: the bundle's version (and reported to fuserd: the stock / modded split).
function(fnl_mod target)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${target}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_CURRENT_SOURCE_DIR}/lib")
    target_compile_definitions(${target} PRIVATE UNICODE FNL_DEBUG=$<BOOL:${FNL_DEBUG}> FNL_VERSION="${FNL_VERSION}")
    target_compile_options(${target} PRIVATE /utf-8 /EHsc /Zi /std:c++20)
    target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF /IGNORE:4099)
endfunction()

# The multiplayer client (C++ tier)
add_library(FUSERMultiplayer SHARED
    src/multiplayer/plugin_entry.cpp
    src/multiplayer/host_hook.cpp
    src/multiplayer/p2p_tunnel.cpp
    src/multiplayer/pak_check.cpp
    src/multiplayer/community_fetch.cpp
    src/multiplayer/server_content.cpp
)
fnl_mod(FUSERMultiplayer)
# libarchive + zlib (static, tools/build_libarchive.ps1): extracting community song archives
target_link_libraries(FUSERMultiplayer PRIVATE FUSERNativeLoader "${CMAKE_CURRENT_SOURCE_DIR}/lib/funchook.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libarchive/archive.lib" "${CMAKE_CURRENT_SOURCE_DIR}/lib/libarchive/zlib.lib" Bcrypt)

# Key/BPM, song tabs, search, sorting, crate-edit tabs, new-crate slots, genre 9 requests (C++ tier)
add_library(FUSERQualityOfLifeImprovements SHARED src/qol/plugin_entry.cpp src/qol/fusernative.cpp)
fnl_mod(FUSERQualityOfLifeImprovements)
target_link_libraries(FUSERQualityOfLifeImprovements PRIVATE FUSERNativeLoader)

# The fifth deck and Hot Clip Volume Normalisation (C++ tier; its hooks are prepared early and installed later, on its
# own funchook handle)
add_library(FUSERFifthDeck SHARED src/fifth_deck/plugin_entry.cpp src/fifth_deck/fifth_deck.cpp)
fnl_mod(FUSERFifthDeck)
target_link_libraries(FUSERFifthDeck PRIVATE FUSERNativeLoader "${CMAKE_CURRENT_SOURCE_DIR}/lib/funchook.lib")

# The BPM range (C tier, early) and the engine / SDK fixes (C tier)
add_library(FUSERBpmRange SHARED src/bpm_range/bpm_range.cpp)
fnl_mod(FUSERBpmRange)
add_library(FUSERFixes SHARED src/fixes/fixes.cpp)
fnl_mod(FUSERFixes)
