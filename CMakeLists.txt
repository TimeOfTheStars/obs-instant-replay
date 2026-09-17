cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Clip export encodes straight from the ring with the FFmpeg that ships in obs-deps — the same
# build OBS itself links, so the DLLs already loaded by the OBS process satisfy the imports.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Headers carrying Q_OBJECT must be listed here as well, otherwise AUTOMOC skips them.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/core/angle-capture.cpp
    src/core/angle-capture.hpp
    src/core/angle-manager.cpp
    src/core/angle-manager.hpp
    src/core/capture-tap.hpp
    src/core/frame-ring.cpp
    src/core/frame-ring.hpp
    src/core/main-mix-tap.cpp
    src/core/main-mix-tap.hpp
    src/core/memory-calculator.cpp
    src/core/memory-calculator.hpp
    src/core/playback-engine.cpp
    src/core/playback-engine.hpp
    src/core/plugin-settings.cpp
    src/core/plugin-settings.hpp
    src/core/replay-director.cpp
    src/core/replay-director.hpp
    src/core/replay-source.cpp
    src/core/replay-source.hpp
    src/core/view-tap.cpp
    src/core/view-tap.hpp
    src/export/clip-exporter.cpp
    src/export/clip-exporter.hpp
    src/export/export-path.cpp
    src/export/export-path.hpp
    src/plugin-main.cpp
    src/ui/replay-dock.cpp
    src/ui/replay-dock.hpp
    src/ui/replay-timeline.cpp
    src/ui/replay-timeline.hpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
