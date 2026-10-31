cmake_minimum_required(VERSION 3.20)

# The template mod (C tier): one DLL built against fnl_mod.h. Build it with
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# and install build\Release\TemplateMod.dll with mod.ini in Fuser\Binaries\Win64\mods\TemplateMod\.
project(TemplateMod LANGUAGES CXX)

# FNL_SDK: the folder holding fnl_mod.h (this folder's parent in the SDK)
set(FNL_SDK "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "The FUSERNativeLoader SDK folder")

add_library(TemplateMod SHARED template_mod.cpp)
target_include_directories(TemplateMod PRIVATE "${FNL_SDK}")
target_compile_features(TemplateMod PRIVATE cxx_std_17)
target_compile_options(TemplateMod PRIVATE /utf-8 /EHsc)
