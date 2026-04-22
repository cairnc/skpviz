# layerviewer

Scaffold for an AOSP layer-trace viewer: SDL3 + ImGui + Skia (Ganesh/GL).

The plan is to clone a subset of AOSP `frameworks/native` — SurfaceFlinger
frontend, CompositionEngine, RenderEngine — and stub out HAL / HWComposer /
GraphicBuffer / Fence so the real SF code can replay a captured layer trace
and render it here.

## Building

Dependencies are git submodules.

```sh
git submodule update --init --recursive
```

### macOS

Needs Xcode command line tools, CMake, and Ninja (`brew install cmake ninja`).

```sh
cd third_party/skia
python3 tools/git-sync-deps
bin/gn gen out/Release --args='is_debug=false skia_use_gl=true skia_use_metal=true'
ninja -C out/Release skia
cd ../..

cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build
```

### Linux

```sh
sudo apt-get install -y cmake ninja-build python3 \
    libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libxext-dev libxss-dev libxtst-dev \
    libwayland-dev libxkbcommon-dev libdecor-0-dev \
    libdbus-1-dev libibus-1.0-dev libpipewire-0.3-dev \
    libfontconfig1-dev libfreetype-dev

cd third_party/skia
python3 tools/git-sync-deps
bin/gn gen out/Release --args='is_debug=false skia_use_gl=true'
ninja -C out/Release skia
cd ../..

cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build
```

### Windows

Needs Visual Studio 2022 with C++ workload, CMake, and Ninja.

```sh
cd third_party/skia
python3 tools/git-sync-deps
python3 bin/gn.py gen out/Release --args="is_debug=false skia_use_gl=true"
ninja -C out/Release skia
cd ../..

cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build
```

### Run

```sh
./build/layerviewer
```

## License

[MIT](LICENSE)
