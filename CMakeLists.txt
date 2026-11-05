# Mario Kart Wii for the PS Vita and the PS3.
#
#   cmake -G Ninja -B build/vita -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/vita
#
#   cmake -G Ninja -B build/ps3 -DMKW_PLATFORM=ps3 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/ps3
#
# Vita: needs VITASDK (with vitaGL from vdpm). PS3: needs PS3DEV (ps3toolchain
# with PSL1GHT). Both need the sources tools/bootstrap.py restores into vendor/
# (Kinoko patched, citro3d, libctru headers; PSL1GHT's cgcomp for the PS3).
cmake_minimum_required(VERSION 3.16)
set(MKW_PLATFORM vita CACHE STRING "Console to build for: vita or ps3")
if(NOT MKW_PLATFORM STREQUAL "vita" AND NOT MKW_PLATFORM STREQUAL "ps3")
  message(FATAL_ERROR "MKW_PLATFORM must be vita or ps3")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(MKW_PLATFORM STREQUAL "ps3")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/platform/ps3/ps3.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    if(NOT DEFINED ENV{VITASDK})
      message(FATAL_ERROR "Set VITASDK to the Vita SDK folder")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()
if(MKW_PLATFORM STREQUAL "ps3")
  project(mkw_ps3 C CXX)
else()
  project(mkw_vita C CXX)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
endif()

set(VITA_APP_NAME "Mario Kart Wii")
set(VITA_TITLEID "MKWV00001")
set(VITA_VERSION "01.00")
# PS3: the package's title id and content id; game files go in its USRDIR
# (platform/ps3/src/ctru_file.cpp).
set(PS3_APP_NAME "Mario Kart Wii")
set(PS3_TITLEID "MKWP00001")
set(PS3_CONTENTID "UP0001-${PS3_TITLEID}_00-0000000000000000")
option(MKW_RELEASE "No test hooks or runtime log" OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN ${CMAKE_BINARY_DIR}/gen)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/vita/gen_headers.py ${GEN}
  RESULT_VARIABLE gen_result)
if(gen_result)
  message(FATAL_ERROR "tools/vita/gen_headers.py failed")
endif()

set(KINOKO ${CMAKE_SOURCE_DIR}/vendor/kinoko)
set(CITRO3D ${CMAKE_SOURCE_DIR}/vendor/citro3d)
set(LIBCTRU ${CMAKE_SOURCE_DIR}/vendor/libctru/libctru)
set(PLATFORM ${CMAKE_SOURCE_DIR}/platform/${MKW_PLATFORM})

# Same floating-point contract as the 3DS build: IEEE single precision, no
# contraction, no fast-math, so Kinoko replays match the Wii bit for bit.
# Some state (Kinoko's Vector3f, members without initializers) relied on
# memory the 3DS happened to hand over zeroed: -ftrivial-auto-var-init=zero
# zeroes every automatic.
if(MKW_PLATFORM STREQUAL "ps3")
  # ps3_libc.h and the _GLIBCXX macros: PSL1GHT's newlib predates its libstdc++.
  set(PS3_LIBC "SHELL:-include ${CMAKE_SOURCE_DIR}/platform/ps3/include/ps3_libc.h"
    -D_GLIBCXX_USE_C99_MATH_TR1=1 -D_GLIBCXX11_USE_C99_STDIO=1 -D_GLIBCXX11_USE_C99_STDLIB=1)
  set(MKW_CODEGEN -mcpu=cell -mhard-float -O2 -g -ffunction-sections -fdata-sections
    -fno-strict-aliasing -ffp-contract=off -ftrivial-auto-var-init=zero ${PS3_LIBC})
  set(MKW_DEFINES __3DS__ REVOLUTION)
else()
  set(MKW_CODEGEN -mcpu=cortex-a9 -mfpu=neon -O2 -g -ffunction-sections -fdata-sections
    -fno-strict-aliasing -ffp-contract=off -ftrivial-auto-var-init=zero)
  set(MKW_DEFINES __3DS__ __vita__ REVOLUTION)
endif()
if(MKW_RELEASE)
  list(APPEND MKW_DEFINES MKW_RELEASE KINOKO_QUIET)
else()
  # Development builds run with any compatible game-file set in ux0:data/mkw
  # (no content-stamp check) and keep the QA hooks and runtime.log.
  list(APPEND MKW_DEFINES MKW_EXTERNAL_ASSETS)
endif()
set(MKW_INCLUDES
  ${PLATFORM}/include # <3ds.h> stand-in; before libctru's own
  ${LIBCTRU}/include
  ${CITRO3D}/include
  ${CMAKE_SOURCE_DIR}/include
  ${KINOKO}/include
  ${KINOKO}/source
  ${GEN})

file(GLOB_RECURSE kinoko_src ${KINOKO}/source/*.cc)
list(FILTER kinoko_src EXCLUDE REGEX "/(main|KReplaySystem|Context)\\.cc$")
add_library(kinoko STATIC ${kinoko_src})
target_compile_options(kinoko PRIVATE ${MKW_CODEGEN} -std=gnu++23 -fno-exceptions -fno-rtti -fcheck-new -Wno-psabi)
target_compile_definitions(kinoko PRIVATE ${MKW_DEFINES})
target_include_directories(kinoko PRIVATE ${MKW_INCLUDES})

# citro3d's portable parts: matrix maths and the .t3x loader, plus libctru's
# LZ decompressor that loader uses. The GPU side is platform/*/src/c3d_*. The
# PS3 reads .t3x headers with its own loader (platform/ps3/src/tex3ds.cpp).
file(GLOB c3d_maths ${CITRO3D}/source/maths/*.c)
if(MKW_PLATFORM STREQUAL "ps3")
  add_library(ctrport STATIC ${c3d_maths} ${LIBCTRU}/source/util/decompress/decompress.c)
else()
  add_library(ctrport STATIC ${c3d_maths} ${CITRO3D}/source/tex3ds.c ${LIBCTRU}/source/util/decompress/decompress.c)
endif()
target_compile_options(ctrport PRIVATE ${MKW_CODEGEN} -std=gnu11 -Wno-unused-function)
target_compile_definitions(ctrport PRIVATE ${MKW_DEFINES})
target_include_directories(ctrport PRIVATE ${MKW_INCLUDES})

file(GLOB game_src ${CMAKE_SOURCE_DIR}/src/*.cpp)
# 3DS only: serialized the LINEAR heap through linker --wrap.
list(FILTER game_src EXCLUDE REGEX "/linear_lock\\.cpp$")

if(MKW_PLATFORM STREQUAL "ps3")
  # PSL1GHT's NV40 program assembler (cgcomp), for the programs the RSX layer
  # writes at run time (platform/ps3/src/rsx_asm.cpp).
  set(CGCOMP ${CMAKE_SOURCE_DIR}/vendor/psl1ght/tools/cgcomp)
  add_library(rsxasm STATIC
    ${CGCOMP}/source/parser.cpp ${CGCOMP}/source/vpparser.cpp ${CGCOMP}/source/fpparser.cpp
    ${CGCOMP}/source/compilervp.cpp ${CGCOMP}/source/compilerfp.cpp
    ${PLATFORM}/src/rsx_asm.cpp)
  target_compile_options(rsxasm PRIVATE -mcpu=cell -mhard-float -O2 -g -std=gnu++17 -Wno-write-strings ${PS3_LIBC} "SHELL:-include stdexcept")
  target_include_directories(rsxasm PRIVATE ${CGCOMP}/include)

  file(GLOB platform_src ${PLATFORM}/src/*.cpp ${PLATFORM}/src/*.c)
  list(FILTER platform_src EXCLUDE REGEX "/rsx_asm\\.cpp$")
  # Shared with the Vita: zeroed operator new, citro3d's C target constructors.
  list(APPEND platform_src ${CMAKE_SOURCE_DIR}/platform/vita/src/zeroed_new.cpp ${CMAKE_SOURCE_DIR}/platform/vita/src/c3d_glue.c)
  add_executable(mkw ${game_src} ${platform_src})
  target_compile_options(mkw PRIVATE ${MKW_CODEGEN} -Wall -Wno-psabi
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23 -fno-exceptions -fno-rtti -fcheck-new>)
  target_compile_definitions(mkw PRIVATE ${MKW_DEFINES})
  target_include_directories(mkw PRIVATE ${MKW_INCLUDES} ${PSL1GHT}/ppu/include ${PSL1GHT}/ppu/include/simdmath)
  # Paths naming the Vita's data folder are mapped onto the PS3's (ctru_file.cpp).
  target_link_options(mkw PRIVATE -mhard-float -Wl,--gc-sections
    -Wl,--wrap=fopen -Wl,--wrap=freopen -Wl,--wrap=mkdir -Wl,--wrap=rename -Wl,--wrap=remove -Wl,--wrap=chdir)
  target_link_directories(mkw PRIVATE ${PSL1GHT}/ppu/lib ${PS3DEV}/portlibs/ppu/lib)
  target_link_libraries(mkw PRIVATE
    kinoko ctrport rsxasm
    rsx gcm_sys io audio sysutil rt lv2 m stdc++
    # PlayStation Move (platform/ps3/src/ctru_sys.cpp): libgem on SPURS.
    gem camera spurs sysmodule)

  # mkw.elf -> mkw.self (run from a homebrew loader) and mkw.pkg (install from
  # the XMB's Package Manager), as PSL1GHT's ppu_rules make them.
  set(PS3_BIN ${PS3DEV}/bin)
  set(PS3_PKG ${CMAKE_BINARY_DIR}/pkg)
  set(PS3_ICON ${CMAKE_SOURCE_DIR}/platform/ps3/ICON0.PNG)
  if(NOT EXISTS ${PS3_ICON})
    set(PS3_ICON ${PS3_BIN}/ICON0.PNG)
  endif()
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/mkw.self ${CMAKE_BINARY_DIR}/mkw.pkg
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:mkw> -o ${CMAKE_BINARY_DIR}/mkw.stripped.elf
    COMMAND ${PS3_BIN}/sprxlinker ${CMAKE_BINARY_DIR}/mkw.stripped.elf
    COMMAND ${PS3_BIN}/make_self ${CMAKE_BINARY_DIR}/mkw.stripped.elf ${CMAKE_BINARY_DIR}/mkw.self
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${PS3_PKG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PS3_PKG}/USRDIR
    COMMAND ${CMAKE_COMMAND} -E copy ${PS3_ICON} ${PS3_PKG}/ICON0.PNG
    COMMAND ${PS3_BIN}/make_self_npdrm ${CMAKE_BINARY_DIR}/mkw.stripped.elf ${PS3_PKG}/USRDIR/EBOOT.BIN ${PS3_CONTENTID}
    COMMAND ${PS3_BIN}/sfo --title ${PS3_APP_NAME} --appid ${PS3_TITLEID} -f ${PS3_BIN}/sfo.xml ${PS3_PKG}/PARAM.SFO
    COMMAND ${PS3_BIN}/pkg --contentid ${PS3_CONTENTID} ${PS3_PKG}/ ${CMAKE_BINARY_DIR}/mkw.pkg
    DEPENDS mkw
    VERBATIM)
  add_custom_target(mkw_pkg ALL DEPENDS ${CMAKE_BINARY_DIR}/mkw.self ${CMAKE_BINARY_DIR}/mkw.pkg)
  return()
endif()

file(GLOB platform_src ${PLATFORM}/src/*.cpp ${PLATFORM}/src/*.c)
add_executable(mkw ${game_src} ${platform_src})
target_compile_options(mkw PRIVATE ${MKW_CODEGEN} -Wall -Wno-psabi
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23 -fno-exceptions -fno-rtti -fcheck-new>)
target_compile_definitions(mkw PRIVATE ${MKW_DEFINES})
target_include_directories(mkw PRIVATE ${MKW_INCLUDES})
# __sce_headroom (VitaSDK's armelf.x): at least 64 KiB between the code and data
# segments. DATA_SEGMENT_ALIGN otherwise starts .data on the 64 KiB boundary
# right after the code whenever the code size lands just so, and vita-elf-create
# then crashes (heap corruption, 0xc0000374) while converting to mkw.velf.
target_link_options(mkw PRIVATE -Wl,--gc-sections -Wl,-q -Wl,--wrap=fopen -Wl,--defsym=__sce_headroom=0x10000)
target_link_libraries(mkw PRIVATE
  kinoko ctrport
  vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
  SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub
  ScePower_stub SceAudio_stub SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub
  SceSysmodule_stub SceIme_stub SceRtc_stub SceLibKernel_stub SceKernelThreadMgr_stub
  SceRazorCapture_stub_weak SceRazorHud_stub_weak
  m stdc++ pthread)

vita_create_self(eboot.bin mkw UNSAFE)
# The bubble icon and LiveArea from the player's disc (tools/vita/livearea_art.py,
# run by tools/vita/build_assets.py), else plain placeholders.
if(EXISTS ${CMAKE_SOURCE_DIR}/build/vita/sce_sys_art/icon0.png)
  set(MKW_SCE_SYS ${CMAKE_SOURCE_DIR}/build/vita/sce_sys_art)
else()
  set(MKW_SCE_SYS ${CMAKE_SOURCE_DIR}/platform/vita/sce_sys)
endif()
vita_create_vpk(mkw.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${MKW_SCE_SYS} sce_sys)
