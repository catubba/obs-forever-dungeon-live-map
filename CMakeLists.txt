cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin.cpp)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PLUGIN_VERSION="${PLUGIN_VERSION}")

# The matching and the reading of the game files, apart from OBS: the plugin and the tools that try them
# on recorded data share it.
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)
add_library(matching STATIC src/mosaic.cpp src/minimap.cpp src/casc.cpp src/wdc5.cpp src/interiors.cpp)
target_include_directories(matching PUBLIC src)
target_compile_features(matching PUBLIC cxx_std_17)
target_link_libraries(matching PUBLIC ZLIB::ZLIB)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE matching)
foreach(tool mosaic-check replay casc-read maps-list)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE matching)
endforeach()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
