cmake_minimum_required(VERSION 3.20)
project(deepseek_balance CXX)

if(NOT WIN32)
    message(FATAL_ERROR "deepseek-balance only supports Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /W4 /permissive-)
else()
    message(FATAL_ERROR "build with MSVC via vcvars64.bat; see bu-rukku steps doc section 0.4")
endif()

add_executable(dshb
    src/main.cpp
    src/renderer.cpp
    src/paths.cpp
    src/single_instance.cpp
    src/amount.cpp
    src/sampling.cpp
    src/state_machine.cpp
    src/widget_display.cpp
  )

set_target_properties(dshb PROPERTIES WIN32_EXECUTABLE TRUE)

target_compile_definitions(dshb PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00)

target_link_libraries(dshb PRIVATE
    d2d1 d3d11 dcomp dwrite windowscodecs
    user32 gdi32 ole32 uuid shell32)

target_sources(dshb PRIVATE src/app.manifest)
set_property(TARGET dshb PROPERTY VS_DPI_AWARE "PerMonitor")
# ---------------------------------------------------------------------------
# 鐙珛鎺㈤拡锛氬彧鎵撳嵃杩愬姩鏇茬嚎锛屼笉纰板叡浜姸鎬併€?# 鑷鍧楅噷鐘舵€佹搷浣滃お澶氾紝瑙傚療涓棿閲忓鏄撹嚜宸辩畻閿欙紱鍗曠嫭鐨勬帰閽堟病鏈夎繖绉嶄綑鍦般€?# ---------------------------------------------------------------------------
add_executable(tauprobe tools/tauprobe.cpp src/widget_display.cpp src/state_machine.cpp src/amount.cpp)
add_executable(coordprobe tools/coordprobe.cpp src/amount.cpp)
target_compile_definitions(tauprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(tauprobe PRIVATE src)
