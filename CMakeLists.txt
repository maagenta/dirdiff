# DirDiff - directory comparer built on WDL/SWELL
#
#   Windows (Visual Studio 2019+):  cmake -B build -A x64 && cmake --build build --config Release
#   macOS (Xcode or Command Line Tools): cmake -B build && cmake --build build
#   Linux (gdk-3, freetype, fontconfig, Xi, GL): cmake -B build && cmake --build build
#
# By default WDL is the submodule in WDL/ (the sources are in WDL/WDL/).
# Before building: git submodule update --init
# Another location: -DWDL_DIR=/path/to/WDL/WDL

cmake_minimum_required(VERSION 3.16)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
project(DirDiff VERSION 1.0 LANGUAGES C CXX)

set(WDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL" CACHE PATH "WDL folder containing swell/, lice/ and wingui/")
if(NOT EXISTS "${WDL_DIR}/swell/swell.h")
  message(FATAL_ERROR "WDL not found at '${WDL_DIR}'. Run: git submodule update --init "
                      "or pass -DWDL_DIR=.../WDL/WDL")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(APP_SOURCES
  main.cpp
  diffengine.cpp
  diffengine.h
  resource.h
  "${WDL_DIR}/wingui/wndsize.cpp")

if(WIN32)
  # ------------------------------------------------------------------ Windows
  enable_language(RC)
  if(MSVC)
    # the .rc is UTF-8 (#pragma code_page(65001)); /c65001 in case the resource compiler ignores it
    string(APPEND CMAKE_RC_FLAGS " /c65001")
  else()
    string(APPEND CMAKE_RC_FLAGS " -c 65001")
  endif()

  add_executable(dirdiff WIN32 ${APP_SOURCES} "${WDL_DIR}/win32_utf8.c" dirdiff.rc)
  target_compile_definitions(dirdiff PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(dirdiff PRIVATE comctl32 uxtheme ole32 shell32 uuid)

  if(MSVC)
    target_compile_options(dirdiff PRIVATE /utf-8 /W3)
    # the manifest is already inside dirdiff.rc
    target_link_options(dirdiff PRIVATE /MANIFEST:NO)
  else()
    # MinGW-GCC: WDL/wingui/wndsize.cpp casts FARPROC to void* (MSVC accepts it)
    set_source_files_properties("${WDL_DIR}/wingui/wndsize.cpp" PROPERTIES COMPILE_OPTIONS "-fpermissive")
    target_link_options(dirdiff PRIVATE -static)
  endif()

elseif(APPLE)
  # -------------------------------------------------------------------- macOS
  enable_language(OBJCXX)

  set(SWELL_SOURCES
    "${WDL_DIR}/swell/swell.cpp"
    "${WDL_DIR}/swell/swell-ini.cpp"
    "${WDL_DIR}/swell/swell-dlg.mm"
    "${WDL_DIR}/swell/swell-gdi.mm"
    "${WDL_DIR}/swell/swell-kb.mm"
    "${WDL_DIR}/swell/swell-menu.mm"
    "${WDL_DIR}/swell/swell-misc.mm"
    "${WDL_DIR}/swell/swell-miscdlg.mm"
    "${WDL_DIR}/swell/swell-wnd.mm"
    "${WDL_DIR}/swell/swell-appstub.mm"
    "${WDL_DIR}/swell/swellappmain.mm")

  add_executable(dirdiff MACOSX_BUNDLE ${APP_SOURCES} mac_main.mm ${SWELL_SOURCES}
                 dirdiff.rc_mac_dlg dirdiff.rc_mac_menu)
  set_target_properties(dirdiff PROPERTIES
    OUTPUT_NAME "DirDiff"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.example.dirdiff"
    MACOSX_BUNDLE_BUNDLE_NAME "DirDiff"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

  # own prefix for SWELL's Objective-C classes (avoids clashes with other SWELL modules)
  target_compile_definitions(dirdiff PRIVATE SWELL_APP_PREFIX=SWELL_dirdiff)
  target_compile_options(dirdiff PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fno-objc-arc>)
  target_link_libraries(dirdiff PRIVATE "-framework Cocoa" "-framework Carbon" "-framework CoreFoundation")

else()
  # -------------------------------------------------- Linux / BSD (SWELL + GDK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SWELLDEPS REQUIRED gdk-3.0 freetype2 fontconfig)

  set(SWELL_SOURCES
    "${WDL_DIR}/swell/swell.cpp"
    "${WDL_DIR}/swell/swell-ini.cpp"
    "${WDL_DIR}/swell/swell-miscdlg-generic.cpp"
    "${WDL_DIR}/swell/swell-wnd-generic.cpp"
    "${WDL_DIR}/swell/swell-menu-generic.cpp"
    "${WDL_DIR}/swell/swell-kb-generic.cpp"
    "${WDL_DIR}/swell/swell-dlg-generic.cpp"
    "${WDL_DIR}/swell/swell-gdi-generic.cpp"
    "${WDL_DIR}/swell/swell-misc-generic.cpp"
    "${WDL_DIR}/swell/swell-gdi-lice.cpp"
    "${WDL_DIR}/swell/swell-generic-gdk.cpp"
    "${WDL_DIR}/swell/swell-appstub-generic.cpp")
  set(LICE_SOURCES
    "${WDL_DIR}/lice/lice_image.cpp"
    "${WDL_DIR}/lice/lice_arc.cpp"
    "${WDL_DIR}/lice/lice_line.cpp"
    "${WDL_DIR}/lice/lice_text.cpp"
    "${WDL_DIR}/lice/lice_textnew.cpp"
    "${WDL_DIR}/lice/lice.cpp"
    "${WDL_DIR}/lice/lice_colorspace.cpp")

  add_executable(dirdiff ${APP_SOURCES} ${SWELL_SOURCES} ${LICE_SOURCES}
                 dirdiff.rc_mac_dlg dirdiff.rc_mac_menu)
  target_compile_definitions(dirdiff PRIVATE
    SWELL_TARGET_GDK=3 SWELL_LICE_GDI SWELL_FREETYPE SWELL_FONTCONFIG _FILE_OFFSET_BITS=64)
  target_include_directories(dirdiff PRIVATE ${SWELLDEPS_INCLUDE_DIRS})
  target_compile_options(dirdiff PRIVATE ${SWELLDEPS_CFLAGS_OTHER} -fno-math-errno -Wno-multichar)
  target_link_directories(dirdiff PRIVATE ${SWELLDEPS_LIBRARY_DIRS})
  target_link_libraries(dirdiff PRIVATE ${SWELLDEPS_LIBRARIES} X11 Xi GL pthread dl)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
    target_compile_options(dirdiff PRIVATE -fsigned-char) # WDL assumes signed char
  endif()
endif()

# the sources include "../WDL/..."; with this directory it resolves to WDL/WDL/...
target_include_directories(dirdiff PRIVATE "${WDL_DIR}")

if(NOT WIN32)
  # dirdiff.rc_mac_dlg / .rc_mac_menu are regenerated from dirdiff.rc if perl is available
  # (the generated files are kept in the project in case it is not)
  find_program(PERL_EXECUTABLE perl)
  if(PERL_EXECUTABLE)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/dirdiff.rc_mac_dlg" "${CMAKE_CURRENT_SOURCE_DIR}/dirdiff.rc_mac_menu"
      COMMAND "${PERL_EXECUTABLE}" "${WDL_DIR}/swell/swell_resgen.pl" dirdiff.rc
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      DEPENDS dirdiff.rc
      COMMENT "swell_resgen: dirdiff.rc -> dirdiff.rc_mac_dlg"
      VERBATIM)
  endif()
  set_source_files_properties(dirdiff.rc_mac_dlg dirdiff.rc_mac_menu PROPERTIES HEADER_FILE_ONLY TRUE)
endif()
