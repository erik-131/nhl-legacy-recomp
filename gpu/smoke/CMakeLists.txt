# Self-contained plume cleared-window smoke test (high-cut engine M0c).
# Isolated from the main nhllegacy/rexglue build on purpose: this only proves the
# vendored plume RHI builds + runs on this machine (D3D12 + Vulkan) before any M1
# integration. Configure/build in its own dir, e.g.:
#   cmake -S gpu/smoke -B out/highcut-smoke -G "Visual Studio 17 2022" -A x64
#   cmake --build out/highcut-smoke --config Release
cmake_minimum_required(VERSION 3.20)
project(plume_smoke CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored plume (third_party/plume relative to repo root, two levels up).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/plume
                 ${CMAKE_BINARY_DIR}/plume_build)

add_executable(plume_smoke main.cpp)
target_link_libraries(plume_smoke PRIVATE plume)

if(WIN32)
    target_link_libraries(plume_smoke PRIVATE d3d12 dxgi dxguid user32 gdi32)
endif()
