cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# StreamMulticast requires both frontend-api and Qt
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# plugin-support static lib is auto-created by helpers_common.cmake from src/plugin-support.c.in
# It needs libobs include dirs so plugin-support.h's `#include <util/base.h>` resolves.

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Wire libobs include dirs into the auto-generated plugin-support helper target
if(TARGET plugin-support)
  target_link_libraries(plugin-support PRIVATE OBS::libobs)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

  # The obs-deps Qt6 ships with broken INTERFACE_INCLUDE_DIRECTORIES propagation
  # for some configurations. We hardcode the include search paths directly
  # to guarantee that <QtWidgets/QWidget> and <QWidget> both resolve.
  set(_OBSDEPS_QT6 "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-07-11-x64")
  if(EXISTS "${_OBSDEPS_QT6}/include")
    target_include_directories(
      ${CMAKE_PROJECT_NAME}
      PRIVATE
        "${_OBSDEPS_QT6}/include"
        "${_OBSDEPS_QT6}/include/QtCore"
        "${_OBSDEPS_QT6}/include/QtGui"
        "${_OBSDEPS_QT6}/include/QtWidgets"
    )
  endif()

  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# C++17 required
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

# All source files (plugin-support.c is in its own static lib target,
# auto-created by helpers_common.cmake and linked via set_target_properties_plugin)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp

    # Core layer
    src/core/Endpoint.cpp
    src/core/ConfigStore.cpp
    src/core/EndpointRegistry.cpp
    src/core/ObsServiceImport.cpp
    src/core/TikTokBridgeImport.cpp

    # Pipeline layer
    src/pipeline/EncoderFactory.cpp
    src/pipeline/OutputController.cpp
    src/pipeline/HealthSampler.cpp

    # UI layer
    src/ui/MultistreamDock.cpp
    src/ui/HealthTab.cpp
    src/ui/ConfigTab.cpp
    src/ui/EndpointDialog.cpp
)

# Include directories
target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Install locale data
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs-plugins/${CMAKE_PROJECT_NAME}"
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
