# Hardware abstraction status

Sensor discovery and microphone capture use the AppKit platform profile for
their `/sys` and `/dev` roots (`LILYGO_UI_DEFAULT_SYSFS_ROOT` and
`LILYGO_UI_DEFAULT_DEV_ROOT`). The existing `LILYGO_SENSORS_SYS_ROOT` and
`LILYGO_SENSORS_DEV_ROOT` overrides remain available for diagnostics and tests.

The current `LilyGoUI::Hardware` facade covers backlight, battery, and playback
volume only. It has no generic IIO or hwmon enumeration, ALSA capture, keyboard,
or I2C probing API, so those adapters remain local to this app. A shared
migration requires capability and lifecycle contracts for streaming sources and
for safe I2C probing before the direct Linux calls can be removed.
