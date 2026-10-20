# Ninja Gaiden (USA) - native Windows x64 recompilation.
#
# The cycle-accurate backend of mstan/nesrecomp turns the ROM's 6502 code into
# C and runs it on a cycle-accurate model of the NES (CPU, PPU, APU, MMC1).
#
# Configure with the path of the player's own legally obtained dump:
#
#   cmake -S . -B build -G "Visual Studio 18 2026" -A x64 ^
#         -DNESRECOMP_ROM="roms/Ninja Gaiden (USA).nes"
#   cmake --build build --config Release
#
# The game executable is NinjaGaidenRecomp.exe; the graphical launcher is
# NinjaGaidenLauncher.exe.  The launcher performs the same steps itself, so
# that a player can build the game from their own ROM without a command line.

cmake_minimum_required(VERSION 3.20)
project(NinjaGaidenRecomp C)

# ---------------------------------------------------------------- the engine --

# The framework: either the "engine" folder that ships with the release
# package, or a checkout of https://github.com/mstan/nesrecomp.
if(NOT DEFINED NESRECOMP_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/engine/runner/cyc/cyc.cmake")
        set(NESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/engine")
    else()
        set(NESRECOMP_ROOT "C:/Users/omega/Documents/tools/nesrecomp-engine"
            CACHE PATH "Root of the mstan/nesrecomp source tree")
    endif()
endif()
if(NOT EXISTS "${NESRECOMP_ROOT}/runner/cyc/cyc.cmake")
    message(FATAL_ERROR
        "NESRECOMP_ROOT=${NESRECOMP_ROOT} does not contain runner/cyc/cyc.cmake. "
        "Point it at a checkout of https://github.com/mstan/nesrecomp or at the "
        "engine folder of the release package.")
endif()

# --------------------------------------------------------------- the inputs --

set(NESRECOMP_ROM "${CMAKE_CURRENT_SOURCE_DIR}/roms/Ninja Gaiden (USA).nes"
    CACHE FILEPATH "The Ninja Gaiden (USA) iNES image")
set(NESRECOMP_RECOMPILER "${CMAKE_CURRENT_SOURCE_DIR}/tools/NESRecomp.exe"
    CACHE FILEPATH "The host recompiler (NESRecomp.exe)")
set(NESRECOMP_GAME_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
    CACHE FILEPATH "The game's configuration")

if(NOT EXISTS "${NESRECOMP_ROM}")
    message(FATAL_ERROR "NESRECOMP_ROM=${NESRECOMP_ROM} does not exist.")
endif()
if(NOT EXISTS "${NESRECOMP_RECOMPILER}")
    message(FATAL_ERROR
        "NESRECOMP_RECOMPILER=${NESRECOMP_RECOMPILER} does not exist. "
        "Build it from the engine (recompiler/) or take it from a release package.")
endif()
if(NOT EXISTS "${NESRECOMP_GAME_CONFIG}")
    message(FATAL_ERROR "NESRECOMP_GAME_CONFIG=${NESRECOMP_GAME_CONFIG} does not exist.")
endif()

# The seed file holds the addresses the program reached.  It is filled in by
# playing a recorded route (the launcher does that), and starts out empty.
set(NESRECOMP_SEED_FILE "${CMAKE_CURRENT_SOURCE_DIR}/seeds/cyc_seeds.txt"
    CACHE FILEPATH "The cycle seed file")
if(NOT EXISTS "${NESRECOMP_SEED_FILE}")
    file(WRITE "${NESRECOMP_SEED_FILE}"
         "# Ninja Gaiden (USA) - cycle seed file.\n"
         "#\n"
         "# The addresses the program reached while the recorded route was played.\n"
         "# The launcher fills this file after the first build and rebuilds with it,\n"
         "# so that those addresses are compiled natively instead of interpreted.\n"
         "# Format:  4k:BB:AAAA count\n")
endif()

# ------------------------------------------------------- generate the C code --

# The recompiler writes one translation unit per 4 KiB PRG bank into
# generated/ below its working directory.
set(NESRECOMP_CODEGEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/codegen")
file(MAKE_DIRECTORY "${NESRECOMP_CODEGEN_DIR}/generated")
execute_process(
    COMMAND "${NESRECOMP_RECOMPILER}" "${NESRECOMP_ROM}"
            --game "${NESRECOMP_GAME_CONFIG}"
            --cycle-accurate
            --output-prefix game
    WORKING_DIRECTORY "${NESRECOMP_CODEGEN_DIR}"
    RESULT_VARIABLE NESRECOMP_CODEGEN_RESULT
    OUTPUT_VARIABLE NESRECOMP_CODEGEN_OUTPUT
    ERROR_VARIABLE NESRECOMP_CODEGEN_ERROR)
if(NOT NESRECOMP_CODEGEN_RESULT EQUAL 0)
    message(FATAL_ERROR
        "The recompiler failed (exit code ${NESRECOMP_CODEGEN_RESULT}).\n"
        "${NESRECOMP_CODEGEN_OUTPUT}\n${NESRECOMP_CODEGEN_ERROR}")
endif()
message(STATUS "Ninja Gaiden: recompiled the ROM to C (${NESRECOMP_CODEGEN_DIR})")

file(GLOB NESRECOMP_CODEGEN_SOURCES CONFIGURE_DEPENDS
     "${NESRECOMP_CODEGEN_DIR}/generated/game_cyc*.c")
if(NOT NESRECOMP_CODEGEN_SOURCES)
    message(FATAL_ERROR "The recompiler produced no C source in ${NESRECOMP_CODEGEN_DIR}/generated.")
endif()
# Re-run the recompiler when its inputs change.  The seed file matters as much
# as the ROM: it tells the recompiler which addresses the recorded route
# reaches, and those are the ones it compiles as native code.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${NESRECOMP_ROM}" "${NESRECOMP_GAME_CONFIG}" "${NESRECOMP_SEED_FILE}")

# ------------------------------------------------------------ the game target --

include("${NESRECOMP_ROOT}/runner/cyc/cyc.cmake")

add_executable(NinjaGaidenRecomp ${NESRECOMP_CYC_SOURCES} ${NESRECOMP_CODEGEN_SOURCES})
target_include_directories(NinjaGaidenRecomp PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})
target_link_libraries(NinjaGaidenRecomp PRIVATE ${NESRECOMP_CYC_LIBRARIES})
target_compile_definitions(NinjaGaidenRecomp PRIVATE
    _CRT_SECURE_NO_WARNINGS
    CYC_GAME_REGION=0)               # 0 = NTSC
nesrecomp_cyc_enable_sdl(NinjaGaidenRecomp)

if(MSVC)
    # A clean Windows machine need not have the VC redistributable installed.
    # Stage the compiler's redistributable DLLs, never copies from System32.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET NinjaGaidenRecomp POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime}" "$<TARGET_FILE_DIR:NinjaGaidenRecomp>"
            VERBATIM)
    endforeach()
endif()

# --------------------------------------------------------- the launcher target --

# The release package ships the launcher already built, so its sources are
# only compiled when they are here.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.c")
add_executable(NinjaGaidenLauncher WIN32
    launcher/launcher.c
    launcher/settings.c
    launcher/rom_validate.c
    launcher/sha256.c
    launcher/ini_config.c
    launcher/build_runner.c
    launcher/ui.c)
target_include_directories(NinjaGaidenLauncher PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher")
target_compile_definitions(NinjaGaidenLauncher PRIVATE
    _CRT_SECURE_NO_WARNINGS
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    "NGR_LAUNCHER_VERSION=\"${NESRECOMP_GAME_VERSION}\""
    "NGR_PROJECT_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "NGR_ENGINE_ROOT=\"${NESRECOMP_ROOT}\"")
target_link_libraries(NinjaGaidenLauncher PRIVATE
    comctl32 shlwapi ole32 uuid)
if(MSVC)
    target_compile_options(NinjaGaidenLauncher PRIVATE /utf-8 /W3)
    # The launcher is a windowed program, but it links the C runtime the same
    # way the game does: stage the redistributable DLLs next to it.
    foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET NinjaGaidenLauncher POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime}" "$<TARGET_FILE_DIR:NinjaGaidenLauncher>"
            VERBATIM)
    endforeach()
endif()

# The launcher reads the controller and key names with SDL, the same library
# the game uses, so both agree on the names in config.ini.
list(APPEND CMAKE_PREFIX_PATH "${NESRECOMP_ROOT}/runner/external/SDL2/cmake")
find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
    target_link_libraries(NinjaGaidenLauncher PRIVATE SDL2::SDL2)
    add_custom_command(TARGET NinjaGaidenLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:NinjaGaidenLauncher>)
else()
    message(WARNING "SDL2 not found: the launcher will not be able to edit gamepad bindings.")
endif()
endif()   # launcher sources present
