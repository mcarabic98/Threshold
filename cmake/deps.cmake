# Third-party dependencies, fetched at configure time into external/.
include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/external" CACHE PATH "Fetched dependencies" FORCE)
set(FETCHCONTENT_QUIET OFF)

# Dear ImGui (docking branch — multi-window menu later)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        docking
  GIT_SHALLOW    TRUE
)
# Logging
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.0
  GIT_SHALLOW    TRUE
)
# JSON (config + version mappings later)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
# x86/x64 disassembler (jvm.dll code/pointer resolution later)
FetchContent_Declare(zydis
  GIT_REPOSITORY https://github.com/zyantific/zydis.git
  GIT_TAG        v4.1.0
  GIT_SHALLOW    TRUE
)

set(SPDLOG_BUILD_EXAMPLE   OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH     OFF CACHE BOOL "" FORCE)
set(SPDLOG_WCHAR_FILENAMES ON  CACHE BOOL "" FORCE)
set(JSON_BuildTests        OFF CACHE BOOL "" FORCE)
set(JSON_Install           OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS      OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(FORCE_SHARED_CRT       ON  CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(imgui spdlog json zydis)

# Dear ImGui ships without CMakeLists — build it ourselves as one static lib.
add_library(imgui_backend STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui_backend PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_backend PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(imgui_backend PUBLIC d3d11 dxgi d3dcompiler)
