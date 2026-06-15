#!/bin/bash
# Build RGBlue (English) as a 32-bit Windows exe (runs under CrossOver/Wine).
set -e
cd "$(dirname "$0")"
export PATH="/opt/homebrew/bin:$PATH"

CXX=i686-w64-mingw32-g++
CC=i686-w64-mingw32-gcc
TP=third_party
OUT=build
mkdir -p "$OUT"

INC="-I$TP/imgui -I$TP/imgui/backends -I$TP/miniz -Isrc"
CXXFLAGS="-m32 -O2 -DNDEBUG -std=c++17 $INC"
CFLAGS="-m32 -O2 -DNDEBUG -I$TP/miniz"

echo "== compiling miniz (C) =="
$CC $CFLAGS -c "$TP/miniz/miniz.c" -o "$OUT/miniz.o"

echo "== compiling ImGui + backends + app =="
OBJ="$OUT/miniz.o"
for f in $TP/imgui/imgui.cpp $TP/imgui/imgui_draw.cpp $TP/imgui/imgui_tables.cpp \
         $TP/imgui/imgui_widgets.cpp \
         $TP/imgui/backends/imgui_impl_dx9.cpp $TP/imgui/backends/imgui_impl_win32.cpp \
         src/effect_file.cpp src/arc.cpp src/pac.cpp src/sdl.cpp src/lmt.cpp src/lmt_anim.cpp src/model.cpp src/viewport.cpp src/tex.cpp src/bc7.cpp src/savedit.cpp src/main.cpp; do
    o="$OUT/$(basename "$f").o"
    $CXX $CXXFLAGS -c "$f" -o "$o"
    OBJ="$OBJ $o"
done

echo "== linking DMCSEEDITOR.exe =="
$CXX -m32 -mwindows -o "$OUT/DMCSEEDITOR.exe" $OBJ \
    -static -static-libgcc -static-libstdc++ \
    -ld3d9 -lcomdlg32 -lole32 -limm32 -lgdi32 -luser32 -lkernel32 -ldwmapi -lwinmm

echo "== done: $OUT/DMCSEEDITOR.exe =="
file "$OUT/DMCSEEDITOR.exe"
