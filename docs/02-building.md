# Build And Test

[Back to README](../README.md)

Project identity, version, and package metadata come from [`lpm.toml`](../lpm.toml).
The stable CMake entry points are `host-simulator` and `cm0-cross`. Run commands
from the repository root and keep build output outside the source tree's tracked
files.

## Host Dependencies

The desktop simulator requires:

- Git and the initialized `third_party/cm0-appkit` submodule;
- CMake 3.21 or later, Make, and a C++17 compiler;
- `pkg-config`, SDL2, and FreeType development packages;
- LPM compatible with the current `lpm.toml` schema.

Linux also needs ALSA development headers for microphone support:

```sh
sudo apt install build-essential cmake git pkg-config libsdl2-dev \
  libfreetype6-dev libasound2-dev
```

On macOS, install the equivalent packages with Homebrew. ALSA is unavailable
there, so microphone capture remains unavailable unless demo data is enabled.

Initialize the SDK once per checkout:

```sh
git submodule update --init --recursive
```

## Desktop Simulator

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

The simulator uses SDL2 and enables the host test suite. Demo data is opt-in:

```sh
LILYGO_SENSORS_DEMO=1 ./build/host-simulator/lilygo-ui-sensors
```

The standard render viewports are `568x1232`, `1232x568`, `320x480`, and
`320x240`. After UI changes, inspect portrait and landscape render output in
`build/host-simulator` in addition to running CTest.

## CM0 Device Build

The cross preset targets the CM0 ARM64 BSP. The first configure may download the
pinned BSP and verify its SHA-256 checksum; later builds reuse `.cache/`.

```sh
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

Use `-DCM0_BSP_CACHE_DIR=/absolute/path/to/cache` to choose another BSP cache
location. Cross compilation requires the target sysroot to provide AppKit, LVGL,
FreeType, DRM, ALSA, and the other libraries declared by the package.

The resulting ARM64 Debian package is written to `dist/`. Deploy it with the
device account and host configured in `lpm.toml`; credentials are not stored in
that file.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| LPM cannot be found | Install a compatible LPM and put `lpm` on `PATH`. |
| AppKit or bundled LVGL cannot be found | Initialize the pinned submodule and rerun configure. |
| SDL2, FreeType, or ALSA is missing | Install host development packages; for cross builds, check the target sysroot. |
| BSP download fails | Check network access and disk space, then rerun the configure step. |
| Render tests fail visually | Inspect both standard orientations and follow the [UI guidelines](01-user-interface.md). |
