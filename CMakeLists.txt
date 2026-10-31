cmake_minimum_required(VERSION 3.25)

project(DSRUpscaler
    VERSION 0.1.0
    DESCRIPTION "DLSS / FSR / XeSS upscaling, frame generation and FPS unlock for Dark Souls Remastered"
    LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DSRUpscaler must be built with MSVC for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)  # _Static_assert in src/hooks/vtable_check.c
# Static CRT: the mod DLL must not depend on a VC++ redist being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The context hooks run thousands of times per frame on the game's render threads: full inlining.
foreach(lang C CXX)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_${lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}")
endforeach()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(DSRU_GAME_DIR "" CACHE PATH
    "DARK SOULS REMASTERED folder. If set, dxgi.dll (+pdb) is copied there after each build.")
# Vendor SDKs: ./third_party, or the Elden Ring mod's next to this folder (same SDKs).
set(_sdk_root "${CMAKE_SOURCE_DIR}/third_party")
if(NOT EXISTS "${_sdk_root}/streamline/include/sl.h" AND EXISTS "${CMAKE_SOURCE_DIR}/../EldenRingUpscaler/third_party")
    set(_sdk_root "${CMAKE_SOURCE_DIR}/../EldenRingUpscaler/third_party")
endif()
set(DSRU_STREAMLINE_DIR "${_sdk_root}/streamline" CACHE PATH "NVIDIA Streamline SDK root (expects include/sl.h)")
set(DSRU_FIDELITYFX_DIR "${_sdk_root}/fidelityfx" CACHE PATH
    "AMD FidelityFX SDK 2.x 'Kits/FidelityFX' folder (expects api/include/ffx_api.h)")
set(DSRU_XESS_DIR "${_sdk_root}/xess" CACHE PATH "Intel XeSS SDK root (expects inc/xess/xess_d3d12.h)")
set(DSRU_NGX_DIR "${_sdk_root}/ngx" CACHE PATH "NVIDIA DLSS (NGX) SDK root, github.com/NVIDIA/DLSS (expects include/nvsdk_ngx.h)")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# MinHook, Dear ImGui and stb are built from their sources directly (SOURCE_SUBDIR points nowhere so
# their own build files are not used) to keep full control over CRT / flags.
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake)
FetchContent_MakeAvailable(minhook imgui stb)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# Code shared by the mod and the probe
# ---------------------------------------------------------------------------
add_library(dsru_common STATIC
    src/core/log.cpp
    src/core/paths.cpp
    src/core/crash_handler.cpp
    src/core/perf.cpp
    src/hooks/hook_manager.cpp
    src/hooks/vtable_check.c
    src/game/pattern_scan.cpp
    src/game/shader_db.cpp
    src/game/rtti.cpp
    src/game/flipper.cpp
    src/game/frame_timer.cpp)
target_include_directories(dsru_common PUBLIC src)
target_compile_definitions(dsru_common PUBLIC
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE DSRU_VERSION="${PROJECT_VERSION}")
target_compile_options(dsru_common PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj>)
target_link_libraries(dsru_common PUBLIC minhook)

# ---------------------------------------------------------------------------
# DSRProbe: diagnostic dxgi.dll (build/<preset>/probe/dxgi.dll)
# ---------------------------------------------------------------------------
add_library(DSRProbe SHARED
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi.def
    src/probe/probe_main.cpp
    src/probe/probe_d3d11.cpp
    src/probe/probe_trace.cpp
    src/probe/probe_dump.cpp
    src/probe/probe_timing.cpp
    src/probe/probe_memory.cpp)
set_target_properties(DSRProbe PROPERTIES
    OUTPUT_NAME dxgi
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/probe"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/probe"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/probe/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/probe")
target_include_directories(DSRProbe PRIVATE ${stb_SOURCE_DIR})
target_link_options(DSRProbe PRIVATE /MAP $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>)
# Never link dxgi.lib: we *are* dxgi.dll. d3d11.lib only for its import stubs' types (no calls are made
# through it: the game's device is hooked, not created).
target_link_libraries(DSRProbe PRIVATE dsru_common version psapi dxguid)
target_compile_definitions(DSRProbe PRIVATE _CRT_SECURE_NO_WARNINGS)
add_custom_command(TARGET DSRProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/dist/DSRProbe.ini" "$<TARGET_FILE_DIR:DSRProbe>")

# ---------------------------------------------------------------------------
# DSRUpscaler: the mod (build/<preset>/dxgi.dll + DSRUpscaler/ runtime folder)
# ---------------------------------------------------------------------------
# HLSL compiled at build time into headers (Windows SDK fxc: shader model 5.0 for D3D11).
find_program(DSRU_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64" REQUIRED)
set(DSRU_SHADER_DIR "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${DSRU_SHADER_DIR}")
set(DSRU_SHADER_HEADERS)
function(dsru_shader source entry profile name)
    set(out "${DSRU_SHADER_DIR}/${name}.h")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${DSRU_FXC}" /nologo /O3 /T ${profile} /E ${entry} /Vn g_${name} /Fh "${out}" "${CMAKE_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${source}"
        COMMENT "fxc ${source} (${entry})"
        VERBATIM)
    set(DSRU_SHADER_HEADERS ${DSRU_SHADER_HEADERS} "${out}" PARENT_SCOPE)
endfunction()
dsru_shader(src/shaders/motion_vectors.hlsl main cs_5_0 motion_vectors_cs)
dsru_shader(src/shaders/blit.hlsl vs_main vs_5_0 blit_vs)
dsru_shader(src/shaders/blit.hlsl ps_main ps_5_0 blit_ps)
dsru_shader(src/shaders/sharpen.hlsl main cs_5_0 sharpen_cs)

add_library(DSRUpscaler SHARED
    src/dllmain.cpp
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi.def
    src/core/bootstrap.cpp
    src/core/config.cpp
    src/game/dsr.cpp
    src/game/fps_unlock_mod.cpp
    src/game/high_fps.cpp
    src/hooks/d3d11_hooks.cpp
    src/hooks/dxgi_hooks.cpp
    src/render/bridge.cpp
    src/render/d3d11_util.cpp
    src/render/presenter.cpp
    src/render/sharpen.cpp
    src/render/upscale.cpp
    src/framegen/framegen.cpp
    src/overlay/overlay.cpp
    src/upscaler/upscaler_types.cpp
    src/upscaler/upscaler.cpp
    src/upscaler/signature.cpp
    src/upscaler/streamline/streamline.cpp
    src/upscaler/fsr/ffx_runtime.cpp
    src/upscaler/fsr/fsr.cpp
    src/upscaler/fsr/fsr_fg.cpp
    src/upscaler/xess/xess.cpp
    src/upscaler/xess/xess_fg.cpp
    src/upscaler/dlss_nr/dlss_nr.cpp
    ${DSRU_SHADER_HEADERS})
set_target_properties(DSRUpscaler PROPERTIES OUTPUT_NAME dxgi)
target_include_directories(DSRUpscaler PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(DSRUpscaler PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_options(DSRUpscaler PRIVATE /MAP $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>)
# Never link dxgi.lib: we *are* dxgi.dll. d3d12.dll is loaded when the bridge is created.
# wintrust / crypt32: signature checks of the vendor runtimes before loading them.
target_link_libraries(DSRUpscaler PRIVATE dsru_common imgui version wintrust crypt32 dxguid)

set(DSRU_RUNTIME_DIR "$<TARGET_FILE_DIR:DSRUpscaler>/DSRUpscaler")
if(EXISTS "${DSRU_STREAMLINE_DIR}/include/sl.h")
    message(STATUS "Streamline SDK: ${DSRU_STREAMLINE_DIR}")
    target_include_directories(DSRUpscaler PRIVATE "${DSRU_STREAMLINE_DIR}/include")
    target_compile_definitions(DSRUpscaler PRIVATE DSRU_HAS_STREAMLINE=1)
    set(DSRU_SL_RUNTIME sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_g.dll nvngx_dlssg.dll
        sl.reflex.dll sl.pcl.dll sl.nis.dll)
    list(TRANSFORM DSRU_SL_RUNTIME PREPEND "${DSRU_STREAMLINE_DIR}/bin/x64/")
    add_custom_command(TARGET DSRUpscaler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DSRU_RUNTIME_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DSRU_SL_RUNTIME} "${DSRU_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_STREAMLINE_DIR}/license.txt"
            "${DSRU_RUNTIME_DIR}/licenses/Streamline-license.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_STREAMLINE_DIR}/bin/x64/nvngx_dlss.license.txt"
            "${DSRU_RUNTIME_DIR}/licenses/NVIDIA-DLSS-license.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_STREAMLINE_DIR}/bin/x64/reflex.license.txt"
            "${DSRU_RUNTIME_DIR}/licenses/NVIDIA-Reflex-license.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_STREAMLINE_DIR}/bin/x64/nis.license.txt"
            "${DSRU_RUNTIME_DIR}/licenses/NVIDIA-NIS-license.txt")
else()
    message(STATUS "Streamline SDK not found - DLSS will be unavailable")
endif()
if(EXISTS "${DSRU_FIDELITYFX_DIR}/api/include/ffx_api.h")
    message(STATUS "FidelityFX SDK: ${DSRU_FIDELITYFX_DIR}")
    target_include_directories(DSRUpscaler PRIVATE
        "${DSRU_FIDELITYFX_DIR}/api/include" "${DSRU_FIDELITYFX_DIR}/upscalers/include"
        "${DSRU_FIDELITYFX_DIR}/framegeneration/include")
    target_compile_definitions(DSRUpscaler PRIVATE DSRU_HAS_FIDELITYFX=1)
    add_custom_command(TARGET DSRUpscaler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DSRU_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DSRU_FIDELITYFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll"
            "${DSRU_FIDELITYFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
            "${DSRU_FIDELITYFX_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll"
            "${DSRU_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DSRU_RUNTIME_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_FIDELITYFX_DIR}/license.md"
            "${DSRU_RUNTIME_DIR}/licenses/AMD-FidelityFX-license.md")
else()
    message(STATUS "FidelityFX SDK not found - FSR will be unavailable")
endif()
if(EXISTS "${DSRU_XESS_DIR}/inc/xess/xess_d3d12.h")
    message(STATUS "XeSS SDK: ${DSRU_XESS_DIR}")
    target_include_directories(DSRUpscaler PRIVATE "${DSRU_XESS_DIR}/inc")
    target_compile_definitions(DSRUpscaler PRIVATE DSRU_HAS_XESS=1)
    add_custom_command(TARGET DSRUpscaler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DSRU_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_XESS_DIR}/bin/libxess.dll"
            "${DSRU_XESS_DIR}/bin/libxess_fg.dll" "${DSRU_XESS_DIR}/bin/libxell.dll" "${DSRU_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DSRU_RUNTIME_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_XESS_DIR}/LICENSE.txt"
            "${DSRU_RUNTIME_DIR}/licenses/Intel-XeSS-license.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DSRU_XESS_DIR}/third-party-programs.txt"
            "${DSRU_RUNTIME_DIR}/licenses/Intel-XeSS-third-party-programs.txt")
else()
    message(STATUS "XeSS SDK not found - XeSS will be unavailable")
endif()

# DLSS 5 neural rendering (upscaler/dlss_nr): NGX headers + static library. The runtime
# (nvngx_dlssnr.dll) is supplied by the user; dsru_nvngx.dll relays its NGX calls (nvngx_forwarder.cpp).
if(EXISTS "${DSRU_NGX_DIR}/include/nvsdk_ngx.h")
    message(STATUS "NGX SDK: ${DSRU_NGX_DIR}")
    target_include_directories(DSRUpscaler PRIVATE "${DSRU_NGX_DIR}/include")
    target_compile_definitions(DSRUpscaler PRIVATE DSRU_HAS_NGX=1)
    target_link_libraries(DSRUpscaler PRIVATE "${DSRU_NGX_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
    add_library(dsru_nvngx SHARED src/upscaler/dlss_nr/nvngx_forwarder.cpp)
    target_include_directories(dsru_nvngx PRIVATE "${DSRU_NGX_DIR}/include")
    target_compile_definitions(dsru_nvngx PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(DSRUpscaler dsru_nvngx)
    add_custom_command(TARGET dsru_nvngx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dsru_nvngx>/DSRUpscaler"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dsru_nvngx>" "$<TARGET_FILE_DIR:dsru_nvngx>/DSRUpscaler")
else()
    message(STATUS "NGX SDK not found - DLSS 5 will be unavailable")
endif()

# License notices of the code compiled into dxgi.dll (shipped by package.ps1): MinHook, Dear ImGui, and
# AMD's MIT license for RCAS (from FSR 1, not covered by the FidelityFX SDK's binary license).
add_custom_command(TARGET DSRUpscaler POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DSRU_RUNTIME_DIR}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${minhook_SOURCE_DIR}/LICENSE.txt"
        "${DSRU_RUNTIME_DIR}/licenses/MinHook-license.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${imgui_SOURCE_DIR}/LICENSE.txt"
        "${DSRU_RUNTIME_DIR}/licenses/DearImGui-license.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/dist/licenses/AMD-FSR1-RCAS-license.txt"
        "${DSRU_RUNTIME_DIR}/licenses/AMD-FSR1-RCAS-license.txt")

add_custom_command(TARGET DSRUpscaler POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/dist/DSRUpscaler.ini" "$<TARGET_FILE_DIR:DSRUpscaler>")
if(DSRU_GAME_DIR)
    add_custom_command(TARGET DSRUpscaler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:DSRUpscaler>" "$<TARGET_PDB_FILE:DSRUpscaler>" "${DSRU_GAME_DIR}"
        COMMENT "Deploying dxgi.dll to ${DSRU_GAME_DIR}")
endif()
