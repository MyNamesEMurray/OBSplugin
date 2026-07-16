# 3.18+: find_path/find_library REQUIRED (used for the FFmpeg lookups)
# is silently misparsed as a search path on older CMake.
cmake_minimum_required(VERSION 3.18...3.30)

project(lenslink VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- libobs ----------------------------------------------------------------
# Prefer an installed libobs CMake package (obs-studio >= 27 installs one);
# fall back to pkg-config for distro packages.
find_package(libobs QUIET)
if(NOT TARGET OBS::libobs)
  if(LIBOBS_INCLUDE_DIR AND LIBOBS_LIBRARY)
    # Manual override for builds against an obs-studio source tree
    # (e.g. Windows CI): -DLIBOBS_INCLUDE_DIR=... -DLIBOBS_LIBRARY=...
    add_library(libobs-manual INTERFACE)
    target_include_directories(libobs-manual INTERFACE ${LIBOBS_INCLUDE_DIR})
    target_link_libraries(libobs-manual INTERFACE ${LIBOBS_LIBRARY})
    add_library(OBS::libobs ALIAS libobs-manual)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOBS REQUIRED IMPORTED_TARGET GLOBAL libobs)
    add_library(OBS::libobs ALIAS PkgConfig::LIBOBS)
  endif()
endif()

# --- FFmpeg (libavcodec / libavutil) ---------------------------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(FFMPEG IMPORTED_TARGET GLOBAL libavcodec libavutil)
endif()
if(TARGET PkgConfig::FFMPEG)
  set(FFMPEG_TARGET PkgConfig::FFMPEG)
else()
  # Manual fallback (e.g. Windows with the obs-studio dependency bundle):
  # pass -DFFMPEG_INCLUDE_DIR=... -DAVCODEC_LIBRARY=... -DAVUTIL_LIBRARY=...
  find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
  find_library(AVCODEC_LIBRARY avcodec REQUIRED)
  find_library(AVUTIL_LIBRARY avutil REQUIRED)
  add_library(ffmpeg-manual INTERFACE)
  target_include_directories(ffmpeg-manual INTERFACE ${FFMPEG_INCLUDE_DIR})
  target_link_libraries(ffmpeg-manual INTERFACE ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})
  set(FFMPEG_TARGET ffmpeg-manual)
endif()

# --- plugin ----------------------------------------------------------------
add_library(${PROJECT_NAME} MODULE
  src/plugin-main.c
  src/ios-camera-source.c
  src/h264-decoder.c
  src/usbmux.c
  src/web-control.c
  src/lipsync.c
  src/mdns.c
  src/plugin-settings.c
  src/gpu-frame.c
  src/pipeline-bench.c
  src/protocol.h
  src/web-control.h
  src/lipsync.h
  src/net-compat.h
  src/h264-decoder.h
  src/usbmux.h
  src/mdns.h
)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs ${FFMPEG_TARGET})

if(WIN32)
  # dxguid: gpu-frame.c references IID_ID3D11Texture2D/IID_IDXGIKeyedMutex,
  # which are extern GUIDs resolved from this static lib in C builds.
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 dxguid)
endif()

if(APPLE)
  # GPU pipeline: gpu-frame.c reads the decoder's CVPixelBuffers
  # (CVPixelBufferGetIOSurface & co) to bind their IOSurfaces as textures.
  target_link_libraries(${PROJECT_NAME} PRIVATE "-framework CoreVideo")
endif()

# --- frontend UI (status bar + dock; optional) -------------------------------
# Qt6 plus the obs-frontend-api HEADER are enough: the API's five symbols
# are resolved at runtime from the OBS process (frontend-ui.cpp), so no
# frontend library is linked and OBS builds without a frontend simply run
# with the UI off. When Qt6 or the header is missing, the plugin builds
# exactly as before, without the feature.
option(ENABLE_FRONTEND_UI "Build the OBS status-bar/dock health UI" ON)
if(ENABLE_FRONTEND_UI)
  # Qt's CMake package requires the CXX language before find_package;
  # probe non-fatally so a C-only toolchain still builds the plugin.
  include(CheckLanguage)
  check_language(CXX)
  if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    find_package(Qt6 QUIET COMPONENTS Core Widgets)
  endif()

  # The header ships with libobs dev packages (Linux) or lives in the
  # obs-studio source tree (frontend/api in 31+, UI/obs-frontend-api
  # before); OBS_FRONTEND_API_HINT lets CI pass those paths explicitly.
  get_target_property(_obs_includes OBS::libobs INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _obs_includes)
    set(_obs_includes "")
  endif()
  find_path(OBS_FRONTEND_API_INCLUDE_DIR obs-frontend-api.h
            HINTS ${OBS_FRONTEND_API_HINT} ${_obs_includes}
                  ${LIBOBS_INCLUDE_DIR})

  if(Qt6_FOUND AND OBS_FRONTEND_API_INCLUDE_DIR)
    set_target_properties(${PROJECT_NAME} PROPERTIES
      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_sources(${PROJECT_NAME} PRIVATE src/frontend-ui.cpp src/health.h)
    target_compile_definitions(${PROJECT_NAME} PRIVATE LENSLINK_FRONTEND)
    target_include_directories(${PROJECT_NAME} PRIVATE
      ${OBS_FRONTEND_API_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets
      ${CMAKE_DL_LIBS})
    message(STATUS "lenslink: frontend UI enabled")
  else()
    message(STATUS "lenslink: frontend UI disabled "
                   "(Qt6 or obs-frontend-api.h not found)")
  endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# --- install ---------------------------------------------------------------
# Default layout matches a Linux system-wide OBS install; override
# CMAKE_INSTALL_PREFIX / the two dirs below for other layouts, or just copy
# the built module + data/ into your OBS plugin folder (see BUILDING.md).
include(GNUInstallDirs)

if(NOT OBS_PLUGIN_DESTINATION)
  set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
endif()
if(NOT OBS_DATA_DESTINATION)
  set(OBS_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins")
endif()

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}")
install(DIRECTORY data/ DESTINATION "${OBS_DATA_DESTINATION}/${PROJECT_NAME}")
