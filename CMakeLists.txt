cmake_minimum_required(VERSION 3.20)
project(deepseek_balance CXX)

# 只跑 Windows；实现语言见 不入库文件/v0.2 设计.md §12
if(NOT WIN32)
    message(FATAL_ERROR "deepseek-balance 只支持 Windows")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 交付要求是「双击即用、无运行时装依赖」→ 静态链接 CRT（设计 §1.3 S1）
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /W4 /permissive-)
else()
    message(FATAL_ERROR "本项目按 MSVC 工具链构建（vcvars64.bat），见 不入库文件/v0.2 实施步骤.md §0.4")
endif()

# 单元集：src/ 下的每个可执行目标各自一份。现在只有一个骨架；A0 的实验室随后加进来。
add_executable(dshb
    src/main.cpp
    src/renderer.cpp)

# 入口点是 wWinMain，所以必须是 GUI 子系统；否则链接期找不到 main()。
# 副作用是对的：双击运行不会弹控制台窗口，而这个程序最终是要能双击即用的。
set_target_properties(dshb PROPERTIES WIN32_EXECUTABLE TRUE)

target_compile_definitions(dshb PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00)

# 与静态 CRT 搭配的库；d2d1/d3d11/dcomp/dwrite 都是 Windows 自带组件，不需要分发
target_link_libraries(dshb PRIVATE
    d2d1 d3d11 dcomp dwrite
    user32 gdi32 ole32 uuid shell32)

# Per-Monitor V2 DPI 感知：靠清单声明，不靠运行时 API（设计 §11.2.1）
target_sources(dshb PRIVATE src/app.manifest)
set_property(TARGET dshb PROPERTY VS_DPI_AWARE "PerMonitor")

# ---------------------------------------------------------------------------
# A0 实验室（src/lab，两条渲染路径对比）已在此处移除。
# 结论：走 DirectComposition（设计文档 §12.3.3b、实施步骤 A0），
# 实验室代码与产物按计划整块删除，不再保留构建目标。
# ---------------------------------------------------------------------------
