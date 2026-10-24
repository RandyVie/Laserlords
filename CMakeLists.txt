cmake_minimum_required(VERSION 3.20)
project(laserlords VERSION 0.8.1 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL3: use an installed copy, or build it from source and link it statically
# (-DLL_BUNDLED_SDL=ON, or automatically when none is installed). Static SDL makes the
# Linux / Steam Deck and Windows builds self-contained.
option(LL_BUNDLED_SDL "Build SDL3 from source and link it statically" OFF)
if(NINTENDO_SWITCH)
    # Switch homebrew (devkitPro toolchain): SDL2 from the portlibs, see the Switch section below
elseif(NOT LL_BUNDLED_SDL)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        message(STATUS "SDL3 not found: building it from source")
        set(LL_BUNDLED_SDL ON)
    endif()
endif()
if(LL_BUNDLED_SDL AND NOT NINTENDO_SWITCH)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(LL_FETCH_OPTS "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        set(LL_FETCH_OPTS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.14.tar.gz
        ${LL_FETCH_OPTS})
    FetchContent_MakeAvailable(SDL3)
    set(LL_SDL SDL3::SDL3-static)
elseif(NOT NINTENDO_SWITCH)
    set(LL_SDL SDL3::SDL3)
endif()

# Musashi: m68kmake generates m68kops.c/.h from m68k_in.c at build time.
set(MUSASHI ${CMAKE_SOURCE_DIR}/runner/third_party/musashi)
set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi)
if(CMAKE_CROSSCOMPILING)
    # the generator must run on the build machine: compile it with the host compiler now
    file(MAKE_DIRECTORY ${MUSASHI_GEN})
    find_program(HOST_CC NAMES cc clang gcc REQUIRED NO_CMAKE_FIND_ROOT_PATH)
    execute_process(COMMAND ${HOST_CC} -o ${MUSASHI_GEN}/m68kmake_host ${MUSASHI}/m68kmake.c RESULT_VARIABLE rc)
    execute_process(COMMAND ${MUSASHI_GEN}/m68kmake_host ${MUSASHI_GEN} ${MUSASHI}/m68k_in.c RESULT_VARIABLE rc2)
    if(rc OR rc2)
        message(FATAL_ERROR "could not build/run the Musashi generator on the host")
    endif()
else()
    add_executable(m68kmake ${MUSASHI}/m68kmake.c)
    add_custom_command(
        OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GEN}
        COMMAND m68kmake ${MUSASHI_GEN} ${MUSASHI}/m68k_in.c
        DEPENDS m68kmake ${MUSASHI}/m68k_in.c)
endif()
add_library(musashi STATIC
    ${MUSASHI}/m68kcpu.c ${MUSASHI}/m68kdasm.c ${MUSASHI}/softfloat/softfloat.c
    ${MUSASHI_GEN}/m68kops.c)
target_include_directories(musashi PUBLIC ${MUSASHI} ${MUSASHI_GEN})
target_compile_definitions(musashi PUBLIC MUSASHI_CNF="${CMAKE_SOURCE_DIR}/runner/src/m68kconf_ll.h")
if(NOT MSVC)
    target_compile_options(musashi PRIVATE -w)
endif()

# libchdr: read the MAME CHD disc image directly (bundled zstd/lzma/miniz, static).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(runner/third_party/libchdr EXCLUDE_FROM_ALL)

# Runner core (everything but main) as a library, shared by the game and the tests.
add_library(llcore STATIC
    runner/src/cpu.c runner/src/os9.c runner/src/io.c
    runner/src/disc.c runner/src/mem.c runner/src/log.c runner/src/video.c runner/src/cdfm.c runner/src/audio.c runner/src/input.c runner/src/soundmap.c)
target_include_directories(llcore PUBLIC runner/src)
target_link_libraries(llcore PUBLIC musashi chdr-static)

if(NINTENDO_SWITCH)
    # Nintendo Switch homebrew: laserlords.nro (libnx + SDL2), started from the Homebrew Menu.
    # Built in devkitPro's Docker image by scripts/build-switch.sh.
    find_package(SDL2 CONFIG REQUIRED)
    add_executable(laserlords_switch runner/src/switch/main_switch.c runner/src/settings.c)
    target_link_libraries(laserlords_switch PRIVATE llcore SDL2::SDL2-static)
    target_compile_options(laserlords_switch PRIVATE -Wall -Wextra)
    target_compile_options(llcore PRIVATE -Wall -Wextra)
    nx_generate_nacp(OUTPUT laserlords.nacp NAME "Laser Lords" AUTHOR "CD-i runner" VERSION "${PROJECT_VERSION}")
    nx_create_nro(laserlords_switch OUTPUT laserlords.nro ICON ${CMAKE_SOURCE_DIR}/packaging/switch/icon.jpg NACP laserlords.nacp)
    return()
endif()

add_executable(laserlords runner/src/main.c runner/src/settings.c)
target_link_libraries(laserlords PRIVATE llcore ${LL_SDL})

# macOS: LaserLords.app with SDL3 bundled in Contents/Frameworks (runs without Homebrew).
if(APPLE)
    set(LL_ICNS ${CMAKE_SOURCE_DIR}/packaging/macos/LaserLords.icns)
    add_executable(LaserLordsApp MACOSX_BUNDLE runner/src/main.c runner/src/settings.c ${LL_ICNS})
    set_source_files_properties(${LL_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_link_libraries(LaserLordsApp PRIVATE llcore ${LL_SDL})
    # target name differs from "laserlords" by more than case: macOS file systems are case-insensitive
    set_target_properties(LaserLordsApp PROPERTIES OUTPUT_NAME LaserLords
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks")
    if(LL_BUNDLED_SDL)   # static SDL: just sign
        add_custom_command(TARGET LaserLordsApp POST_BUILD
            COMMAND sh ${CMAKE_SOURCE_DIR}/packaging/macos/fix_bundle.sh $<TARGET_BUNDLE_DIR:LaserLordsApp> VERBATIM)
    else()               # shared SDL: copy it into the bundle, relink, sign
        add_custom_command(TARGET LaserLordsApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:LaserLordsApp>/Frameworks
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:SDL3::SDL3> $<TARGET_BUNDLE_CONTENT_DIR:LaserLordsApp>/Frameworks/
            COMMAND sh ${CMAKE_SOURCE_DIR}/packaging/macos/fix_bundle.sh $<TARGET_BUNDLE_DIR:LaserLordsApp> VERBATIM)
    endif()
endif()

if(UNIX)
    target_link_libraries(llcore PUBLIC m)
endif()
if(WIN32)
    set_target_properties(laserlords PROPERTIES OUTPUT_NAME laserlords-cli)   # console build for logs
    add_executable(LaserLordsGui WIN32 runner/src/main.c runner/src/settings.c packaging/windows/laserlords.rc)
    target_link_libraries(LaserLordsGui PRIVATE llcore ${LL_SDL})
    set_target_properties(LaserLordsGui PROPERTIES OUTPUT_NAME LaserLords)
endif()
if(NOT MSVC)
    target_compile_options(llcore PRIVATE -Wall -Wextra)
    target_compile_options(laserlords PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(test_kernel tests/test_kernel.c)
target_link_libraries(test_kernel PRIVATE llcore)
add_test(NAME kernel COMMAND test_kernel WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(kernel PROPERTIES ENVIRONMENT "LL_BIN=${CMAKE_SOURCE_DIR}/data/ll.bin")
file(GLOB LL_CHD "${CMAKE_SOURCE_DIR}/*.chd")
if(LL_CHD)
    list(GET LL_CHD 0 LL_CHD_FIRST)
    add_test(NAME kernel_chd COMMAND test_kernel WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(kernel_chd PROPERTIES ENVIRONMENT "LL_BIN=${LL_CHD_FIRST}")
endif()
