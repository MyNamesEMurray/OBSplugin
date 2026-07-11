cmake_minimum_required(VERSION 3.16...3.30)

project(ios-camera-source VERSION 0.1.0 LANGUAGES C)

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
  src/protocol.h
  src/net-compat.h
  src/h264-decoder.h
)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs ${FFMPEG_TARGET})

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
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
