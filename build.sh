#!/bin/sh
# Cross-compiles the port DLL with mingw-w64 (32-bit). On Windows, run the same command from an MSYS2 MINGW32 shell.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-i686-w64-mingw32-g++}
IMGUI=third_party/imgui
mkdir -p build
${WINDRES:-i686-w64-mingw32-windres} -I. res/port.rc -O coff -o build/port_res.o
$CXX -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -shared -o build/d3d9.dll \
  src/main.cpp src/log.cpp src/config.cpp src/patches.cpp src/devices.cpp src/jvs.cpp src/input.cpp \
  src/gfx.cpp src/overlay.cpp \
  $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
  $IMGUI/backends/imgui_impl_dx9.cpp $IMGUI/backends/imgui_impl_win32.cpp \
  -I$IMGUI -Isrc -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
  build/port_res.o d3d9.def -Wl,--enable-stdcall-fixup -Wl,--kill-at \
  -luser32 -lgdi32 -lwinmm -ldwmapi -lole32 -luuid -lshell32
echo "built build/d3d9.dll"
