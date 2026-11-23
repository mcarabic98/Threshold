cmake_minimum_required(VERSION 3.20)
project(Threshold CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/deps.cmake)

add_executable(threshold
  src/main.cpp
  src/app/app.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/util.cpp
  src/memory/process.cpp
  src/memory/pattern.cpp
  src/jvm/vmstructs.cpp
  src/jvm/jvm_runtime.cpp
  src/jvm/heap_reader.cpp
  src/jvm/klass_reader.cpp
  src/mc/finder.cpp
  src/mc/game_model.cpp
  src/mc/mappings.cpp
  src/modules/module.cpp
  src/modules/esp.cpp
  src/modules/hud.cpp
  src/overlay/overlay_window.cpp
  src/overlay/renderer.cpp
  src/ui/menu.cpp
)

target_include_directories(threshold PRIVATE src)

target_compile_definitions(threshold PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
  SPDLOG_WCHAR_FILENAMES
)

target_compile_options(threshold PRIVATE /utf-8 /W3 /EHsc)

target_link_libraries(threshold PRIVATE
  imgui_backend
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  Zydis
  d3d11
  dxgi
  d3dcompiler
  dcomp
  version
  user32
)

# Ship the mappings next to the binary (loadMappings reads appDir()/mappings).
add_custom_command(TARGET threshold POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/mappings $<TARGET_FILE_DIR:threshold>/mappings
  COMMENT "Copying mappings")

