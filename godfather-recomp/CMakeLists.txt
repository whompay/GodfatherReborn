cmake_minimum_required(VERSION 3.20)
project(GodfatherRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "The Godfather: The Don's Edition static recompilation port"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# The city profile attributes most main-thread time to calls through the
# generic vm_read*/vm_write* ABI.  Build transformed copies of the lifted TUs
# which bind those names to the runtime's validated lean accessors.  Canonical
# lifter output remains untouched, and OFF gives an exact baseline build.
option(PORT_PPU_MEMLEAN "Inline validated lean memory accessors in all lifted PPU functions" OFF)
# v1121C codegen switches for the lifted PPU TUs (all default OFF = the pre-v1121C build; NOTES_v1121C_codegen.md).
# They run ps3recomp/tools/ppu_codegen_v1121c.py after ppu_memacc_v243.py on the PORT_PPU_MEMLEAN copies, so they need
# PORT_PPU_MEMLEAN=ON (the play-fast build-memlean2 configuration). With all three OFF the memlean command is unchanged.
#   GF_PPU_VMB_LOCAL  each lifted function keeps vm_base in a local (no reload of the global after every host store)
#   GF_PPU_FPFAST     fadd/fsub/fmul/fdiv/fmadd helpers test the IEEE result for NaN first (bit-identical)
#   GF_PPU_RESTRICT   EXPERIMENTAL: ctx is __restrict + a larger LLVM capture-tracking budget (pays off in leaf functions)
option(GF_PPU_VMB_LOCAL "v1121C: cache vm_base in a local in every lifted function (needs PORT_PPU_MEMLEAN)" OFF)
option(GF_PPU_FPFAST "v1121C: NaN check after the result in the PPC FP helpers, bit-identical (needs PORT_PPU_MEMLEAN)" OFF)
option(GF_PPU_RESTRICT "v1121C EXPERIMENTAL: lifted functions take ctx as __restrict (needs PORT_PPU_MEMLEAN)" OFF)
set(GF_PPU_CG_ARGS "")
if(GF_PPU_VMB_LOCAL)
    list(APPEND GF_PPU_CG_ARGS --vmb-local)
endif()
if(GF_PPU_FPFAST)
    list(APPEND GF_PPU_CG_ARGS --fpfast)
endif()
if(GF_PPU_RESTRICT)
    list(APPEND GF_PPU_CG_ARGS --restrict)
endif()
set(GF_PPU_CG_COMMAND "")
set(GF_PPU_CG_DEPENDS "")
if(GF_PPU_CG_ARGS)
    if(NOT PORT_PPU_MEMLEAN)
        message(WARNING "GF_PPU_VMB_LOCAL / GF_PPU_FPFAST / GF_PPU_RESTRICT need PORT_PPU_MEMLEAN=ON; ignored")
        set(GF_PPU_CG_ARGS "")
    else()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(GF_PPU_CG_COMMAND COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/ppu_codegen_v1121c.py"
            "@DST@" "@DST@" ${GF_PPU_CG_ARGS} --stats
            --hdr=${PS3RECOMP_DIR}/runtime/ppu/ppu_codegen_v1121c.h)
        set(GF_PPU_CG_DEPENDS "${PS3RECOMP_DIR}/tools/ppu_codegen_v1121c.py"
                              "${PS3RECOMP_DIR}/runtime/ppu/ppu_codegen_v1121c.h")
        message(STATUS "PPU codegen:     v1121C ${GF_PPU_CG_ARGS}")
    endif()
endif()
if(PORT_PPU_MEMLEAN)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(MEMLEAN_DIR "${CMAKE_CURRENT_BINARY_DIR}/recompiled-memlean")
    set(MEMLEAN_SOURCES "")
    foreach(src IN LISTS RECOMP_SOURCES)
        get_filename_component(src_name "${src}" NAME)
        set(dst "${MEMLEAN_DIR}/${src_name}")
        string(REPLACE "@DST@" "${dst}" _gf_cg_cmd "${GF_PPU_CG_COMMAND}")   # v1121C (empty when its options are OFF)
        add_custom_command(
            OUTPUT "${dst}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${MEMLEAN_DIR}"
            COMMAND "${Python3_EXECUTABLE}"
                    "${PS3RECOMP_DIR}/tools/ppu_memacc_v243.py"
                    "${src}" "${dst}"
                    --all-functions --mode=all --stats
                    --hdr=${PS3RECOMP_DIR}/runtime/ppu/ppu_memory_v243.h
            ${_gf_cg_cmd}
            DEPENDS "${src}"
                    "${PS3RECOMP_DIR}/tools/ppu_memacc_v243.py"
                    "${PS3RECOMP_DIR}/runtime/ppu/ppu_memory_v243.h"
                    ${GF_PPU_CG_DEPENDS}
            VERBATIM
            COMMENT "Inlining lean PPU memory accessors in ${src_name}")
        list(APPEND MEMLEAN_SOURCES "${dst}")
    endforeach()
    set(RECOMP_SOURCES ${MEMLEAN_SOURCES})
    message(STATUS "PPU memory:      lean all-function build")
endif()

# v1057T call-count tracker (separate build dir only: build.cmd + build-fncount.flag). Every lifted function counts its
# calls into g_gf_fnc (gf_fncount.c); F2 writes snapshots. Canonical sources untouched.
option(GF_FNCOUNT "Count calls of every lifted function (F2 snapshots)" OFF)
if(GF_FNCOUNT)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(FNC_DIR "${CMAKE_CURRENT_BINARY_DIR}/recompiled-fncount")
    set(FNC_SOURCES "")
    foreach(src IN LISTS RECOMP_SOURCES)
        get_filename_component(src_name "${src}" NAME)
        get_filename_component(src_ext "${src}" EXT)
        if(src_ext STREQUAL ".cpp")
            set(dst "${FNC_DIR}/${src_name}")
            add_custom_command(
                OUTPUT "${dst}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${FNC_DIR}"
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gf_fncount.py" "${src}" "${dst}"
                DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gf_fncount.py"
                VERBATIM
                COMMENT "Adding call counters to ${src_name}")
            list(APPEND FNC_SOURCES "${dst}")
        else()
            list(APPEND FNC_SOURCES "${src}")
        endif()
    endforeach()
    list(APPEND FNC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gf_fncount.c")
    set(RECOMP_SOURCES ${FNC_SOURCES})
    message(STATUS "PPU:             v1057T call-count tracker build")
endif()

# Selected-function variant of the same transform (v1041P).  Only the lifted
# functions named in the list file (one func_XXXXXXXX per line, '#' comments;
# godfather-recomp/perf/hot_functions.txt is the measured city set) get the lean
# accessors, rewritten in place: same symbol, no twin, no per-call selector.
# Every other function keeps the generic vm_read*/vm_write* calls.  Empty = off.
# Ignored when PORT_PPU_MEMLEAN (all functions) is ON.
set(PORT_PPU_MEMLEAN_LIST "" CACHE STRING
    "Inline lean memory accessors only in the lifted functions listed in this file (empty = off)")
set(PORT_PPU_MEMLEAN_DIRECT OFF)
if(PORT_PPU_MEMLEAN)
    set(PORT_PPU_MEMLEAN_DIRECT ON)
elseif(PORT_PPU_MEMLEAN_LIST)
    get_filename_component(MEMLEAN_LIST_ABS "${PORT_PPU_MEMLEAN_LIST}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${MEMLEAN_LIST_ABS}")
        message(FATAL_ERROR "PORT_PPU_MEMLEAN_LIST: no such file ${MEMLEAN_LIST_ABS}")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(MEMLEAN_DIR "${CMAKE_CURRENT_BINARY_DIR}/recompiled-memlean-list")
    set(MEMLEAN_SOURCES "")
    foreach(src IN LISTS RECOMP_SOURCES)
        get_filename_component(src_name "${src}" NAME)
        set(dst "${MEMLEAN_DIR}/${src_name}")
        add_custom_command(
            OUTPUT "${dst}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${MEMLEAN_DIR}"
            COMMAND "${Python3_EXECUTABLE}"
                    "${PS3RECOMP_DIR}/tools/ppu_memacc_v243.py"
                    "${src}" "${dst}"
                    "--list=${MEMLEAN_LIST_ABS}" --mode=all --stats
                    --hdr=${PS3RECOMP_DIR}/runtime/ppu/ppu_memory_v243.h
            DEPENDS "${src}" "${MEMLEAN_LIST_ABS}"
                    "${PS3RECOMP_DIR}/tools/ppu_memacc_v243.py"
                    "${PS3RECOMP_DIR}/runtime/ppu/ppu_memory_v243.h"
            VERBATIM
            COMMENT "Inlining lean PPU memory accessors in the listed functions of ${src_name}")
        list(APPEND MEMLEAN_SOURCES "${dst}")
    endforeach()
    set(RECOMP_SOURCES ${MEMLEAN_SOURCES})
    set(PORT_PPU_MEMLEAN_DIRECT ON)
    message(STATUS "PPU memory:      lean accessors in listed functions (${MEMLEAN_LIST_ABS})")
endif()

# Optimisation flags guard (v1041P).  A build tree whose cache holds EMPTY
# CMAKE_<LANG>_FLAGS / CMAKE_<LANG>_FLAGS_<CONFIG> compiles every TU (lifted
# code, runtime library, ppu_loader's vm_* accessors) without /O2, without
# /DNDEBUG and without /EHsc, and links without /debug.  Warn about it; with
# PORT_RESTORE_DEFAULT_FLAGS=ON put CMake's own MSVC-style defaults back for
# this configure (normal variables, so the runtime subdirectory gets them too).
option(PORT_RESTORE_DEFAULT_FLAGS
       "Restore CMake's default MSVC-style compile/link flags when the cache holds empty ones" OFF)
if(MSVC AND CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" PORT_BT)
    if("${CMAKE_CXX_FLAGS_${PORT_BT}}" STREQUAL "")
        if(PORT_RESTORE_DEFAULT_FLAGS)
            if("${PORT_BT}" STREQUAL "RELWITHDEBINFO")
                set(PORT_CFG_FLAGS "/Zi /O2 /Ob1 /DNDEBUG")
                set(PORT_CFG_LINK "/debug /INCREMENTAL")
            elseif("${PORT_BT}" STREQUAL "RELEASE")
                set(PORT_CFG_FLAGS "/O2 /Ob2 /DNDEBUG")
                set(PORT_CFG_LINK "/INCREMENTAL:NO")
            elseif("${PORT_BT}" STREQUAL "MINSIZEREL")
                set(PORT_CFG_FLAGS "/O1 /Ob1 /DNDEBUG")
                set(PORT_CFG_LINK "/INCREMENTAL:NO")
            else()
                set(PORT_CFG_FLAGS "/Zi /Ob0 /Od")
                set(PORT_CFG_LINK "/debug /INCREMENTAL")
            endif()
            set(CMAKE_C_FLAGS_${PORT_BT} "${PORT_CFG_FLAGS}")
            set(CMAKE_CXX_FLAGS_${PORT_BT} "${PORT_CFG_FLAGS}")
            if("${CMAKE_CXX_FLAGS}" STREQUAL "")
                set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /EHsc")
            endif()
            if("${CMAKE_C_FLAGS}" STREQUAL "")
                set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS")
            endif()
            if("${CMAKE_EXE_LINKER_FLAGS}" STREQUAL "")
                set(CMAKE_EXE_LINKER_FLAGS "/machine:x64")
            endif()
            if("${CMAKE_EXE_LINKER_FLAGS_${PORT_BT}}" STREQUAL "")
                set(CMAKE_EXE_LINKER_FLAGS_${PORT_BT} "${PORT_CFG_LINK}")
            endif()
            message(STATUS "Build flags:     v1041P restored ${CMAKE_BUILD_TYPE} defaults (${PORT_CFG_FLAGS}; CXX ${CMAKE_CXX_FLAGS})")
        else()
            message(WARNING
                "CMAKE_CXX_FLAGS_${PORT_BT} is EMPTY in this build tree's cache: no /O2, no /DNDEBUG, no /EHsc. "
                "Every TU compiles unoptimised. Reconfigure a fresh build directory, or pass "
                "-DPORT_RESTORE_DEFAULT_FLAGS=ON.")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

option(PORT_SPU_SIMD "Build the lifted SPU sources with SSE4.2/AVX2/FMA (v1042Z)" ON)
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# v1043C: everything except the lifted SPU images is compiled ONCE into an object library and linked into two
# executables: ${PROJECT_NAME} (SPU code SSE4.2 + AVX2 + FMA) and ${PROJECT_NAME}-compat (SPU code SSE4.2 only)
# for CPUs without AVX2/FMA (Sandy/Ivy Bridge, Pentium/Celeron before Alder Lake, AMD FX/Jaguar). gf_cpugate.c
# (compiled into each exe without -m flags) starts the compat exe automatically when the CPU lacks AVX2/FMA.
# PORT_BUILD_COMPAT_EXE=OFF builds only the main exe. The only per-exe sources are the SPU images + gf_cpugate.c.
option(PORT_BUILD_COMPAT_EXE "Also link ${PROJECT_NAME}-compat.exe (SPU code without AVX2/FMA) (v1043C)" ON)
# v1056A: Jolt Physics (third_party/jolt, MIT) for the rigid-body ragdoll (gf_ragdoll_jolt.cpp). Built here as a
# plain static library at the default ISA (SSE2 x64 baseline, Jolt detects it), so it suits both exes.
# GF_USE_JOLT=OFF -> gf_ragdoll_jolt_stub.c, and gf_ragdoll.c falls back to its particle solver.
option(GF_USE_JOLT "Rigid-body ragdoll on Jolt Physics (v1056A)" ON)
set(GF_RAG_JOLT_SRC gf_ragdoll_jolt_stub.c)
if(GF_USE_JOLT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jolt/Jolt/Jolt.h")
    file(GLOB_RECURSE GF_JOLT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jolt/Jolt/*.cpp")
    add_library(gf_jolt STATIC ${GF_JOLT_SOURCES})
    target_include_directories(gf_jolt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jolt")
    target_compile_definitions(gf_jolt PUBLIC JPH_OBJECT_STREAM)
    target_compile_features(gf_jolt PUBLIC cxx_std_17)
    if(MSVC)
        target_compile_options(gf_jolt PRIVATE /W0)
    else()
        target_compile_options(gf_jolt PRIVATE -w)
    endif()
    set(GF_RAG_JOLT_SRC gf_ragdoll_jolt.cpp)
    message(STATUS "Ragdoll:         Jolt Physics rigid-body core (v1056A, ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jolt)")
else()
    message(STATUS "Ragdoll:         particle solver only (GF_USE_JOLT=OFF or third_party/jolt missing)")
endif()
add_library(gf_common OBJECT
    main.cpp
    stubs.cpp
    compat_stubs.c
    gf_prompts.c
    gf_cutscene.c
    gf_combat.c
    gf_footcam.c
    gf_trainer.c
    gf_coop.c
    gf_coop_probe.c
    gf_coop_puppet.c
    gf_coop_pose.c
    gf_coop_glue.c
    gf_coop_look.c
    gf_coop_npclook.c
    gf_coop_proxy.c
    gf_ragdoll.c
    gf_ragdoll_bhv.c
    gf_footik.c
    gf_smoke.c
    gf_shop.c
    gf_dialogue.c
    gf_gps.c
    gf_crew.c             # two crew members, crew car seats + windows, crew medic
    gf_living.c           # Living City: street fights, police rules, busier streets
    gf_sit.c
    gf_perf.c
    gf_hotfn.c            # v1121F native hot-function fast paths (lane F)
    ${GF_RAG_JOLT_SRC}
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
)
# v1060W2 world sync: gf_coop_world.c always; W1 / W3 modules when present, else link stubs (no duplicates).
get_target_property(_gf_world_srcs gf_common SOURCES)
target_sources(gf_common PRIVATE gf_coop_world.c)
target_sources(gf_common PRIVATE gf_coop_wpose.c)   # v1061F world NPC pose stream
target_sources(gf_common PRIVATE gf_coop_vehicle.c)   # v1063V car sync (agent V)
target_sources(gf_common PRIVATE gf_coop_combat.c)   # v1062C co-op combat sync (agent C)
target_sources(gf_common PRIVATE gf_coop_npcfix.c)   # v1067X NPC LOD + grab-pose sync
target_sources(gf_common PRIVATE gf_coop_peers.c)   # v1072S N-peer puppet pool (agent S)
target_sources(gf_common PRIVATE gf_coop_promote.c)   # v1126P proxy -> native promotion (agent P)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gf_coop_mission.c")   # v1072S for agent M (guarded until the file exists)
  target_sources(gf_common PRIVATE gf_coop_mission.c)
endif()
foreach(_gfw IN ITEMS worldre proxy)
    if(_gfw STREQUAL "worldre")
        set(_gfw_real gf_coop_worldre.c)
        set(_gfw_stub gf_coop_world_stub_re.c)
    else()
        set(_gfw_real gf_coop_proxy.c)
        set(_gfw_stub gf_coop_world_stub_proxy.c)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_gfw_real}")
        list(FIND _gf_world_srcs ${_gfw_real} _gfw_at)
        if(_gfw_at EQUAL -1)
            target_sources(gf_common PRIVATE ${_gfw_real})
        endif()
        message(STATUS "Co-op world:     ${_gfw_real}")
    else()
        target_sources(gf_common PRIVATE ${_gfw_stub})
        message(STATUS "Co-op world:     ${_gfw_stub} (stub, ${_gfw_real} not written yet)")
    endif()
endforeach()
# v1113I PORT_PPU_ISA (default ON): the lifted PPU TUs of ${PROJECT_NAME} are compiled for the CPU floor that exe already
# needs for its SPU code (AVX2 + FMA: Haswell / Zen and later) plus BMI1/BMI2/LZCNT/MOVBE, which every such CPU has
# (gf_cpugate.c checks all of them and starts the -compat exe otherwise). For baseline x86-64 clang-cl turned every guest
# load/store into load + bswap, every PPC fmadd-family op into a CALL to the UCRT's fma() (6,496 call sites) and every
# trunc/nearbyint into a CALL (1,230 trunc sites). -ffp-contract=off: nothing is fused that the PPU code did not fuse
# itself, so every result stays bit-identical (fma()/trunc()/nearbyint() are exact IEEE operations either way).
# ${PROJECT_NAME}-compat gets its own baseline copy (gf_ppu_compat), so the lifted PPU code compiles twice per build dir.
# PORT_PPU_ISA=OFF = one shared baseline copy (the pre-v1113I build).
option(PORT_PPU_ISA "Build the lifted PPU code of the main exe with AVX2/FMA/BMI2/MOVBE (v1113I)" ON)
add_library(gf_ppu OBJECT ${RECOMP_SOURCES})
set(GF_PPU_FOR_COMPAT gf_ppu)
if(PORT_PPU_ISA AND (NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    target_compile_options(gf_ppu PRIVATE -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mmovbe -ffp-contract=off)
    if(PORT_BUILD_COMPAT_EXE AND PORT_SPU_SIMD AND SPU_SOURCES)
        add_library(gf_ppu_compat OBJECT ${RECOMP_SOURCES})
        set(GF_PPU_FOR_COMPAT gf_ppu_compat)
    endif()
    message(STATUS "Lifted PPU:      AVX2/FMA/BMI2/MOVBE in ${PROJECT_NAME} (v1113I)")
endif()
add_executable(${PROJECT_NAME}
    $<TARGET_OBJECTS:gf_common>
    $<TARGET_OBJECTS:gf_ppu>
    gf_cpugate.c
    ${SPU_SOURCES}
)
set(GF_TARGETS gf_common gf_ppu ${PROJECT_NAME})
if(TARGET gf_ppu_compat)
    list(APPEND GF_TARGETS gf_ppu_compat)
endif()

# v1121C GF_PPU_RESTRICT: LLVM's capture tracking gives up after 100 uses of a pointer, and a lifted function uses ctx
# hundreds of times, so without a larger budget the __restrict never proves ctx un-captured (measured: no effect).
if(GF_PPU_RESTRICT AND PORT_PPU_MEMLEAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(t gf_ppu gf_ppu_compat)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE "SHELL:-mllvm -capture-tracking-max-uses-to-explore=100000")
        endif()
    endforeach()
endif()

# v1121C GF_PPU_THINLTO (default OFF): ThinLTO for the main exe's lifted PPU objects only (cross-TU inlining of small
# lifted callees; the 10 lifted TUs call each other directly). gf_ppu objects become LLVM bitcode, so the link must be
# done by lld-link: configure with -DCMAKE_LINKER=lld-link (VS ships it next to clang-cl). The -compat exe keeps its
# normal gf_ppu_compat objects. /lldltocache keeps relinks after a one-TU change cheap.
option(GF_PPU_THINLTO "v1121C: ThinLTO for the lifted PPU code of the main exe (needs clang-cl + lld-link)" OFF)
if(GF_PPU_THINLTO)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "GF_PPU_THINLTO needs clang / clang-cl")
    endif()
    if(MSVC AND NOT CMAKE_LINKER MATCHES "lld-link")
        message(FATAL_ERROR "GF_PPU_THINLTO: the gf_ppu objects are bitcode; configure with -DCMAKE_LINKER=lld-link (now: ${CMAKE_LINKER})")
    endif()
    target_compile_options(gf_ppu PRIVATE -flto=thin)
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
        if(TARGET ${PROJECT_NAME}-compat AND NOT TARGET gf_ppu_compat)
            target_link_options(${PROJECT_NAME}-compat PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
        endif()
    endif()
    message(STATUS "Lifted PPU:      v1121C ThinLTO (gf_ppu)")
endif()

# v1121C GF_PPU_PGO (default empty = off): clang instrumentation PGO for the main exe's lifted PPU objects.
#   gen: gf_ppu is instrumented (-fprofile-instr-generate) and gf_pgo_dump.c is linked: it writes the profile every
#        GF_PGO_DUMP_SEC seconds (default 30) and at exit, to LLVM_PROFILE_FILE (default default_%p.profraw in the cwd).
#        Play/soak normally (slower), then: llvm-profdata merge -o gf_ppu.profdata <the .profraw files>
#   use: gf_ppu is compiled with -fprofile-instr-use=GF_PPU_PGO_PROFDATA. Functions changed since the training run just
#        lose their profile (clang drops mismatched CFG hashes), nothing breaks.
set(GF_PPU_PGO "" CACHE STRING "v1121C: lifted PPU PGO: empty = off, gen = instrumented build, use = optimise with GF_PPU_PGO_PROFDATA")
set(GF_PPU_PGO_PROFDATA "" CACHE FILEPATH "v1121C: merged .profdata for GF_PPU_PGO=use")
if(GF_PPU_PGO STREQUAL "gen")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "GF_PPU_PGO needs clang / clang-cl")
    endif()
    target_compile_options(gf_ppu PRIVATE -fprofile-instr-generate)
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gf_pgo_dump.c")
    if(MSVC)
        # CMake links with the linker directly, so the profile runtime that clang-cl would add has to be named here.
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
                        OUTPUT_VARIABLE _gf_rdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        file(TO_CMAKE_PATH "${_gf_rdir}" _gf_rdir)
        set(_gf_prof "${_gf_rdir}/lib/windows/clang_rt.profile-x86_64.lib")
        if(NOT EXISTS "${_gf_prof}")   # fallback: <clang-cl dir>/../lib/clang/<major>/lib/windows
            get_filename_component(_gf_cdir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            string(REGEX MATCH "^[0-9]+" _gf_cmaj "${CMAKE_CXX_COMPILER_VERSION}")
            set(_gf_prof "${_gf_cdir}/../lib/clang/${_gf_cmaj}/lib/windows/clang_rt.profile-x86_64.lib")
        endif()
        if(NOT EXISTS "${_gf_prof}")
            message(FATAL_ERROR "GF_PPU_PGO=gen: profile runtime not found at ${_gf_prof}")
        endif()
        target_link_libraries(${PROJECT_NAME} PRIVATE "${_gf_prof}")
    endif()
    message(STATUS "Lifted PPU:      v1121C PGO instrumented build (gf_ppu)")
elseif(GF_PPU_PGO STREQUAL "use")
    if(NOT EXISTS "${GF_PPU_PGO_PROFDATA}")
        message(FATAL_ERROR "GF_PPU_PGO=use: GF_PPU_PGO_PROFDATA '${GF_PPU_PGO_PROFDATA}' does not exist")
    endif()
    target_compile_options(gf_ppu PRIVATE "-fprofile-instr-use=${GF_PPU_PGO_PROFDATA}")
    message(STATUS "Lifted PPU:      v1121C PGO from ${GF_PPU_PGO_PROFDATA} (gf_ppu)")
elseif(NOT GF_PPU_PGO STREQUAL "")
    message(FATAL_ERROR "GF_PPU_PGO must be empty, gen or use (got '${GF_PPU_PGO}')")
endif()
set(GF_EXES ${PROJECT_NAME})
if(PORT_BUILD_COMPAT_EXE AND PORT_SPU_SIMD AND SPU_SOURCES)
    add_executable(${PROJECT_NAME}-compat
        $<TARGET_OBJECTS:gf_common>
        $<TARGET_OBJECTS:${GF_PPU_FOR_COMPAT}>
        gf_cpugate.c
        ${SPU_SOURCES}
    )
    # read by the SPU sources' and gf_cpugate.c's COMPILE_OPTIONS / COMPILE_DEFINITIONS generator expressions
    set_target_properties(${PROJECT_NAME}-compat PROPERTIES GF_SPU_COMPAT 1)
    list(APPEND GF_TARGETS ${PROJECT_NAME}-compat)
    list(APPEND GF_EXES ${PROJECT_NAME}-compat)
    message(STATUS "Executables:     ${PROJECT_NAME} (SPU AVX2/FMA) + ${PROJECT_NAME}-compat (SPU SSE4.2) (v1043C)")
endif()
set_source_files_properties(gf_cpugate.c PROPERTIES
    COMPILE_DEFINITIONS "GF_SPU_COMPAT=$<BOOL:$<TARGET_PROPERTY:GF_SPU_COMPAT>>")

if(PORT_PPU_MEMLEAN_DIRECT)
    # Direct lean bodies (all functions or the listed ones) need the startup
    # model check in ppu_run (v243a_lean_require) before the first guest call.
    foreach(t IN LISTS GF_TARGETS)
        target_compile_definitions(${t} PRIVATE PORT_PPU_MEMLEAN_ALL=1)
    endforeach()
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # v1042Z PORT_SPU_SIMD (default ON): the SPU helper headers select their SIMD forms by ISA macro --
        # spu_shufb as pshufb/blendv (__SSE4_1__), the whole-register helpers of spu_helpers_vec.h (__SSE4_1__ /
        # __AVX2__), exact fused fma (__FMA__). With "-O2" alone clang-cl targets SSE2, so every lifted SPU image
        # (the VP6 movie decoder, the RWAC audio mixer, SPURS jobs) ran the scalar byte-loop fallbacks: spu_shufb
        # alone was ~18% of the movie SPU thread. Scoped to the SPU sources (the PPU TUs are untouched).
        # -ffp-contract=off: nothing is fused that the SPU did not fuse. Needs a CPU with AVX2 + FMA (Haswell+).
        # v1043C: -mavx2/-mfma only in ${PROJECT_NAME}; the -compat exe (target property GF_SPU_COMPAT) gets SSE4.2.
        if(PORT_SPU_SIMD)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS
                "-O2;-msse4.2;$<$<NOT:$<BOOL:$<TARGET_PROPERTY:GF_SPU_COMPAT>>>:-mavx2>;$<$<NOT:$<BOOL:$<TARGET_PROPERTY:GF_SPU_COMPAT>>>:-mfma>;-ffp-contract=off")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

foreach(t IN LISTS GF_TARGETS)
target_include_directories(${t} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# (on the object library this carries the runtime's usage requirements -- include dirs, definitions)
target_link_libraries(${t} PRIVATE
    ps3recomp_runtime
)
if(TARGET gf_jolt)
    target_link_libraries(${t} PRIVATE gf_jolt)   # v1056A
endif()
endforeach()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
foreach(t IN LISTS GF_EXES)
    set_target_properties(${t} PROPERTIES LINKER_LANGUAGE CXX)
endforeach()

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    foreach(t IN LISTS GF_EXES)
    target_link_libraries(${t} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        dxguid
    )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
foreach(t IN LISTS GF_TARGETS)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${t} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${t} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${t} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()
endforeach()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        # RelWithDebInfo supplies /Ob1.  The lifted code is dominated by tiny
        # guest-memory/accessor helpers and register shuffles, so let clang-cl
        # inline beyond functions explicitly marked inline and use intrinsic
        # forms where available.  Keeping this scoped to the generated TUs
        # avoids changing runtime/debugger stack shapes.
        COMPILE_OPTIONS "/w;/bigobj;/Ob2;/Oi")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# v1129H GF_HOST_OB2 (default OFF = the old flags). RelWithDebInfo passes /Ob1 (inline only functions marked inline)
# to the runtime library and to the game's own C/C++ code; the runtime's own "-O2" does not undo it (the expansion of
# /O2 sets no inline level, the explicit /Ob1 stays). The lifted TUs already get /Ob2 above. ON appends /Ob2 to
# ps3recomp_runtime (FIFO walker, D3D12 backend, syscalls, audio) and gf_common only: the lifted PPU/SPU objects keep
# their command lines, so turning it on rebuilds only the host code. The cache keeps the value per build dir.
option(GF_HOST_OB2 "v1129H: /Ob2 for ps3recomp_runtime and gf_common (clang-cl)" OFF)
if(GF_HOST_OB2 AND MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE /Ob2)
    target_compile_options(gf_common PRIVATE /Ob2)
    message(STATUS "Host code:       v1129H /Ob2 for ps3recomp_runtime + gf_common (GF_HOST_OB2)")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
