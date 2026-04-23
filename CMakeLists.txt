cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Frontend API is required — we wire into RECORDING_STARTED/STOPPED events.
set(ENABLE_FRONTEND_API ON CACHE BOOL "Use obs-frontend-api for UI functionality" FORCE)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/input-logger.c)

if(APPLE)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/hooks-macos.m)
  set_source_files_properties(src/hooks-macos.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "-framework Cocoa" "-framework ApplicationServices" "-framework CoreGraphics" "-framework CoreFoundation"
  )
elseif(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/hooks-windows.c)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE user32)
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/hooks-linux.c)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
