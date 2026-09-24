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
    src/particles.cpp
    src/paths.cpp
    src/panel_drag.cpp
    src/single_instance.cpp
    src/amount.cpp
    src/sampling.cpp
    src/state_machine.cpp
    src/widget_display.cpp src/heartbeat.cpp
    src/api_client.cpp
    src/http_get.cpp
    src/json_min.cpp
    src/balance_source.cpp
    src/curve.cpp
    src/curve_store.cpp
    src/rate_estimator.cpp
    src/tray.cpp
    src/tray.rc
)

set_target_properties(dshb PROPERTIES WIN32_EXECUTABLE TRUE)

target_compile_definitions(dshb PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00)

target_link_libraries(dshb PRIVATE
    d2d1 d3d11 dcomp dwrite windowscodecs
    user32 gdi32 ole32 uuid shell32 winhttp wtsapi32 dwmapi)

target_sources(dshb PRIVATE src/app.manifest)
set_property(TARGET dshb PROPERTY VS_DPI_AWARE "PerMonitor")
# ---------------------------------------------------------------------------
# 鐙珛鎺㈤拡锛氬彧鎵撳嵃杩愬姩鏇茬嚎锛屼笉纰板叡浜姸鎬併€?# 鑷鍧楅噷鐘舵€佹搷浣滃お澶氾紝瑙傚療涓棿閲忓鏄撹嚜宸辩畻閿欙紱鍗曠嫭鐨勬帰閽堟病鏈夎繖绉嶄綑鍦般€?
# ---------------------------------------------------------------------------
add_executable(tauprobe tools/tauprobe.cpp src/widget_display.cpp src/heartbeat.cpp src/state_machine.cpp src/amount.cpp src/curve_store.cpp src/json_min.cpp src/rate_estimator.cpp)
add_executable(coordprobe tools/coordprobe.cpp src/amount.cpp)
target_compile_definitions(tauprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(tauprobe PRIVATE src)

# api probe: balance endpoint transport + parsing + error routing, with an offline case
# runner. Same shape as coordprobe above; it links the shared HTTPS transport
# (src/http_get.cpp) and winhttp is its only extra library.
add_executable(apiprobe tools/apiprobe.cpp src/api_client.cpp src/http_get.cpp src/json_min.cpp)
target_compile_definitions(apiprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(apiprobe PRIVATE src)
target_link_libraries(apiprobe PRIVATE winhttp)

# store probe: curve data layer (a ring covering a whole day of changes, append-on-change,
# curve.json, invalidation). Offline, no network and no shared state; like apiprobe and
# tauprobe it carries its own main() so the widget's argument parsing stays untouched.
add_executable(storeprobe tools/storeprobe.cpp src/curve_store.cpp src/amount.cpp src/json_min.cpp)
target_compile_definitions(storeprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(storeprobe PRIVATE src)

# rate probe: the consumption-rate estimator and the "time to zero" wording
# (src/rate_estimator.h/.cpp). Offline, pure: the estimator takes no clock and no file,
# so this target needs only the estimator and the tuning table -- no store, no network,
# no widget, and its own main() so the widget's argument parsing stays untouched.
add_executable(rateprobe tools/rateprobe.cpp src/rate_estimator.cpp src/amount.cpp)
target_compile_definitions(rateprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(rateprobe PRIVATE src)

# rate baseline: OLD formula vs NEW formula side by side on the REAL curve.json (plus an
# optional synthetic series). It links the real store and the real estimators, because the
# question it answers -- "did the change actually move the number, and did the prediction
# move with it" -- cannot be answered by either probe alone. Offline: it reads a file and
# a command line, opens no socket and starts no widget.
add_executable(ratebaseline tools/ratebaseline.cpp src/rate_estimator.cpp src/curve_store.cpp src/amount.cpp src/json_min.cpp)
target_compile_definitions(ratebaseline PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(ratebaseline PRIVATE src)

# panel probe: the panel's curve DISPLAY SET vs the store's capacity (widget_display.cpp's
# BuildFrameCurve vs curve_store.h's kCapacity). It links the whole display layer -- that is
# the point: the failure it guards (a store holding more points than the panel has slots)
# lives exactly on the seam between the two, and neither storeprobe nor rateprobe can see
# across it. It reaches the display layer through the real file door (Save -> SetCurveStorePath
# -> Load), writes only to %TEMP%, and opens no socket.
add_executable(panelprobe tools/panelprobe.cpp src/widget_display.cpp src/heartbeat.cpp src/state_machine.cpp src/amount.cpp src/curve_store.cpp src/json_min.cpp src/rate_estimator.cpp)
target_compile_definitions(panelprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(panelprobe PRIVATE src)

# close probe: the shutdown flow's state machine (src/widget_display.cpp -- ShutdownEnter /
# ShutdownClick / ShutdownCancel / ShutdownFloorRatio / ShutdownJitterAt).
# ★ Why offline: on the real machine R only ever rises (R(t)=a^t decays, the two clicks raise it),
#   and a cold process starts at R=1.0, so "click 1 pins R to 0.50" needs 75 s of wall clock
#   before it can be seen. This target drives the same DisplayedAmount through 100 simulated
#   seconds in milliseconds, which makes the criterion reproducible; the real window run
#   (--shutdown-test=1 --shutdown-enter-at=75) stays the end-to-end counterpart.
# ★ It links the display layer (like panelprobe) and renders nothing: the numbers it checks are
#   exactly what the renderer is handed (R, colour, beat amplitude/period, jitter, curve layer).
add_executable(closeprobe tools/closeprobe.cpp src/widget_display.cpp src/heartbeat.cpp src/state_machine.cpp src/amount.cpp src/curve_store.cpp src/json_min.cpp src/rate_estimator.cpp)
target_compile_definitions(closeprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(closeprobe PRIVATE src)

# beat probe: the heartbeat displacement waveform (src/heartbeat.h/.cpp) -- envelope shape,
# per-frame jump, the amplitude and period laws, and the timer's trigger behaviour.
# The waveform and both laws are pure functions of (seconds, that beat), so this target needs
# only src/heartbeat.cpp: the module now has no static state and no integral table, so neither
# psapi nor any extra library is needed. Offline, no store, no network, no widget, and its own
# main() so the widget's argument parsing stays untouched.
# ★ It cannot cover the production trigger loop (that lives in widget_display.cpp's AdvanceBeat,
#   which this target does not link): the timer is replicated here as a fixture, and the real
#   loop is verified from the widget with --beat-frame + the [beat] log line.
add_executable(beatprobe tools/beatprobe.cpp src/heartbeat.cpp)
target_compile_definitions(beatprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(beatprobe PRIVATE src)

# drag probe: the panel's self-managed drag (src/panel_drag.cpp) -- anchor arithmetic, the
# ">= 50% of the AREA inside some monitor's work area" clamp, and config.json with its
# fallbacks. It links the real module, and on top of the offline cases it drives the REAL
# widget with injected mouse input and measures the REAL window: 1:1 following, the clamp at
# every edge, the saved-and-reopened position, and the frame rate while dragging. Those four
# numbers cannot be had offline -- the offsets are integers, the area is a ratio, and the
# frame interval only exists while the window is actually moving.
# ★ It links paths.cpp so the "real config" run goes through the same path resolution the
#   widget uses, and it passes --curve-store= into its child so the test can never touch the
#   owner's curve.json (the same safety valve the widget has for its own demo modes).
add_executable(dragprobe tools/dragprobe.cpp src/panel_drag.cpp src/json_min.cpp src/paths.cpp)
target_compile_definitions(dragprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(dragprobe PRIVATE src)
target_link_libraries(dragprobe PRIVATE shell32 ole32)

# tray probe: the notification-area icon and its menu (src/tray.h/.cpp) -- the icon that
# actually goes into the tray, the rect the icon occupies, the hit test the shutdown
# classifier asks, and the menu's item list.
# ★ What only a separate process can answer: the icon resource itself. This target carries
#   src/tray.rc, so tools/trayprobe.cpp loads the SAME embedded RCDATA the widget loads --
#   the bytes come out of an exe, not off the disk, which is the whole claim being verified
#   ("recompiling needs no external file" / "no black square on a light taskbar").
# ★ It also dumps each generated HICON to PNG and composites it over light and dark
#   backgrounds, because "not a black square" and "edges not chewed" are statements about
#   pixels: the numbers to look at are the alpha ramp and the darkest ring pixel.
# ★ A bare `trayprobe` never touches the tray (no NIM_ADD); the cases that do (installing a
#   real icon, popping the real menu) print the flag to pass instead of doing it silently.
# ★ It also links the display layer (as closeprobe does) for exactly one assertion: what the
#   menu's single item DOES -- one cmd reaches Fired in one shot (T20). That claim belongs to
#   the menu contract, and a fresh process is the only place where the shutdown state is
#   naturally Off, i.e. the state the user is in when they click 关闭.
add_executable(trayprobe tools/trayprobe.cpp src/tray.cpp src/tray.rc src/widget_display.cpp src/heartbeat.cpp src/state_machine.cpp src/amount.cpp src/curve_store.cpp src/json_min.cpp src/rate_estimator.cpp)
target_compile_definitions(trayprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(trayprobe PRIVATE src)
target_link_libraries(trayprobe PRIVATE shell32 ole32 windowscodecs gdi32 user32)

# capacity probe: what the curve ring's SIZE costs. It links the display layer because the
# numbers that matter -- the bytes a store occupies, what one whole-ring copy costs, and what
# one frame costs when the ring is full -- belong to the store and to the frame loop at once.
# Like panelprobe it fills the store through the real Append() path and reaches the display
# layer through the real file door (Save -> SetCurveStorePath -> Load), writes only to %TEMP%,
# and opens no socket. It asserts nothing; it prints milliseconds, and the threshold ("a frame
# is 16.7 ms") is read by a person.
add_executable(capacityprobe tools/capacityprobe.cpp src/widget_display.cpp src/heartbeat.cpp src/state_machine.cpp src/amount.cpp src/curve_store.cpp src/json_min.cpp src/rate_estimator.cpp)
target_compile_definitions(capacityprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(capacityprobe PRIVATE src)
