# Game sources taken verbatim from Dusklight's files.cmake, so the Vita build
# follows upstream's file lists instead of keeping a copy of them.
include_guard(GLOBAL)

set(DUSK_ROOT "${CMAKE_SOURCE_DIR}/external/dusklight")

# files.cmake uses paths relative to Dusklight's root and creates the JSystem
# static libraries itself. Make the lists absolute and re-point the JSystem
# targets' SOURCES (CMake would otherwise resolve them against this directory).
include("${DUSK_ROOT}/files.cmake")
foreach (_list DOLZEL_FILES Z2AUDIOLIB_FILES SSYSTEM_FILES REL_FILES DOLPHIN_FILES DUSK_FILES)
    list(TRANSFORM ${_list} PREPEND "${DUSK_ROOT}/")
endforeach ()
foreach (_lib IN LISTS JSYSTEM_LIBRARIES)
    get_target_property(_srcs ${_lib} SOURCES)
    list(TRANSFORM _srcs PREPEND "${DUSK_ROOT}/")
    set_property(TARGET ${_lib} PROPERTY SOURCES ${_srcs})
endforeach ()

# Dusklight's cmake/GameABIConfig.cmake, minus the mod SDK targets. TARGET_VITA
# is ours; upstream code never sees it unless one of our patches adds it.
set(TWV_GAME_DEFS TARGET_PC=1 WIDESCREEN_SUPPORT=1 AVOID_UB=1 VERSION=0 MTX_USE_PS=1 PARTIAL_DEBUG=1
        DUSK_BUILDING_GAME=1 DUSK_HAS_FUNCHOOK=0 DUSK_HAS_PREPATCH=0 TARGET_VITA=1
        # aurora_core's PUBLIC defines, which upstream game TUs inherit through GAME_LIBS.
        # WEBGPU_DAWN is left out: there is no Dawn on the Vita (no game file tests it).
        AURORA AURORA_ENABLE_GX AURORA_ENABLE_RMLUI)
set(TWV_GAME_INCLUDES
        "${DUSK_ROOT}/include"
        "${DUSK_ROOT}/assets/GZ2E01"
        "${DUSK_ROOT}/libs/JSystem/include"
        "${DUSK_ROOT}/extern/aurora/include/dolphin"
        "${DUSK_ROOT}/extern/aurora/include"
        "${DUSK_ROOT}/sdk/include"
        "${DUSK_ROOT}/src"
        "${DUSK_ROOT}/extern"
        "${DUSK_ROOT}/extern/borealis/include"
        "${DUSK_ROOT}/libs/freeverb/include")

# borealis/version.h, normally written by borealis_detect_version() from git describe.
set(BOREALIS_APP_DESCRIBE "twilight-vita-dev")
set(BOREALIS_APP_VERSION "0.0.1.0")
set(BOREALIS_APP_SHORT_VERSION "0.0.1")
set(BOREALIS_APP_VERSION_CODE 1000)
set(BOREALIS_APP_BRANCH "main")
set(BOREALIS_APP_REVISION "")
set(BOREALIS_APP_DATE "")
set(BOREALIS_PLATFORM_NAME "PS Vita")
set(BOREALIS_LIB_DESCRIBE "4ac5e70")
configure_file("${DUSK_ROOT}/extern/borealis/cmake/version.h.in" "${CMAKE_BINARY_DIR}/generated/borealis/version.h")

add_library(twv_game_headers INTERFACE)
# Aurora's pinned imgui/RmlUi must win over the VitaSDK sysroot's own (older) imgui.h.
target_include_directories(twv_game_headers INTERFACE ${TWV_GAME_INCLUDES}
        "${CMAKE_BINARY_DIR}/generated"
        "${TWV_DEPS_DIR}/imgui"
        "${TWV_DEPS_DIR}/rmlui/Include"
        "${TWV_DEPS_DIR}/dawnheaders/include")
target_compile_definitions(twv_game_headers INTERFACE ${TWV_GAME_DEFS}
        BOREALIS_BUILD_TYPE="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,None>") # borealis_core's interface define
target_link_libraries(twv_game_headers INTERFACE
        twv_tracy twv_json twv_cxxopts twv_picosha2 twv_xxhash twv_miniz
        fmt::fmt absl::flat_hash_map)
# SDL3 and libpng: headers only for now. The prebuilt VitaSDK SDL3 uses short
# enums; it is linked only once the enum-ABI audit has cleared its API structs.
target_include_directories(twv_game_headers SYSTEM INTERFACE
        $<TARGET_PROPERTY:SDL3::SDL3-static,INTERFACE_INCLUDE_DIRECTORIES>
        ${PNG_INCLUDE_DIRS})
# Upstream flags for GNU-like compilers (Dusklight CMakeLists.txt "GNU" block and ARM signed char).
target_compile_options(twv_game_headers INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-multichar>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-trigraphs>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>
        # GCC 15 rejects errors inside never-instantiated template bodies
        # (src/dusk/ui/input.cpp); upstream's compilers accept them. Diagnostic only.
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-template-body>
        -fsigned-char)

foreach (_lib IN LISTS JSYSTEM_LIBRARIES)
    target_link_libraries(${_lib} PRIVATE twv_game_headers)
    target_compile_definitions(${_lib} PRIVATE $<$<CONFIG:Debug>:DEBUG=1>)
endforeach ()

option(TWV_VITA_MATRIX_BLOCKS "Write unchanged J3D XF matrix commands in one bounded FIFO block" ON)
set_property(SOURCE ${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DGD.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS TWV_VITA_MATRIX_BLOCKS=$<BOOL:${TWV_VITA_MATRIX_BLOCKS}>)
set_property(SOURCE ${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DGD.cpp APPEND PROPERTY
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/platform/vita)

# Same per-file defines upstream applies (game_base vs game_debug files).
set_source_files_properties(${DOLZEL_FILES} ${Z2AUDIOLIB_FILES} ${REL_FILES} ${DOLPHIN_FILES} ${DUSK_FILES}
        PROPERTIES COMPILE_DEFINITIONS "NDEBUG=1;NDEBUG_DEFINED=1;DEBUG_DEFINED=0")
set_source_files_properties(${SSYSTEM_FILES}
        PROPERTIES COMPILE_DEFINITIONS "$<$<CONFIG:Debug>:DEBUG=1>")

# The decompiled game proper: frameworks, scenes, actors, audio logic.
add_library(twv_game STATIC ${SSYSTEM_FILES} ${DOLZEL_FILES} ${Z2AUDIOLIB_FILES} ${REL_FILES} ${DOLPHIN_FILES})
target_link_libraries(twv_game PUBLIC twv_game_headers)
target_precompile_headers(twv_game PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/platform/vita/compat/twv_prelude.h>"
        "$<$<COMPILE_LANGUAGE:CXX>:${DUSK_ROOT}/include/dusk_pch.hpp>")
