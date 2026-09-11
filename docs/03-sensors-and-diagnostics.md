# Sensor Operation And Diagnostics

[Back to README](../README.md)

## Data Sources

The application reads Linux interfaces rather than probing chip registers for
measurements:

| Source | Data |
| --- | --- |
| `/sys/bus/iio/devices` | Accelerometer, gyroscope, chip temperature, and environmental sensors |
| `/sys/class/hwmon` | CPU, battery, voltage, current, power, and temperature channels |
| `/sys/class/sound` and ALSA | ES8389 microphone capture |
| `/sys/class/input` and evdev | TCA8418 keyboard events |
| `/dev/i2c-*` | Startup and manual inventory of unregistered I2C addresses |

I2C probing covers `0x03` through `0x77` and uses SMBus Quick or Receive Byte.
It does not write device configuration registers. A responding address is not
proof that a device is identified or that it can provide measurements; registered
devices and readable channels are reported separately.

IIO values use the driver's `raw`, `offset`, and `scale` attributes. Temperatures
are shown in degrees Celsius, humidity in `%RH`, pressure in `hPa`, light in
`lx`, magnetic field in `uT`, and hwmon electrical values in `V`, `A`, or `W`.
Read failures remain errors with unavailable values; zero is never used as a
placeholder.

## Command-Line Probes

```sh
lilygo-ui-sensors --probe --samples 3 --interval 250
lilygo-ui-sensors --keyboard-probe --samples 40 --interval 100
```

Both commands emit NDJSON and do not open a display. The `status` field separates
registered, acknowledged, and busy I2C entries. `LILYGO_SENSORS_DEMO=1` is the
only mode that generates simulated measurements.

## Recording

CSV recordings default to `$XDG_DATA_HOME/lilygo-ui-sensors/recordings`, or
`$HOME/.local/share/lilygo-ui-sensors/recordings` when `XDG_DATA_HOME` is unset.
Set `LILYGO_SENSORS_RECORD_DIR` to override the directory.

Each row contains UTC time, relative time, device, bus, channel, value, unit,
error state, and `device_id`. Only checked channels are recorded. Pausing freezes
the visible values, history, and recording; clearing history keeps the current
CSV file open. Each recording session creates a new file.

Test and offline snapshots can replace the system roots:

```sh
LILYGO_SENSORS_SYS_ROOT=/path/to/sys \
LILYGO_SENSORS_DEV_ROOT=/path/to/dev \
  lilygo-ui-sensors --probe
```

Using non-default roots disables hardware I2C probing. The same overrides also
disable real ALSA microphone capture.

## Keyboard

The list opens a separate keyboard page for detected TCA8418 devices. It reads
the matching evdev node without an exclusive grab, tracks held keys, and keeps up
to 100 recent events. Entering the page starts monitoring; pausing or leaving it
closes the node. Keyboard pause is independent of sensor sampling pause.

Repeat is shown only when the driver reports EV_KEY value `2`; the application
does not synthesize repeat events. The keyboard history is in-memory and is not
written to sensor CSV files.

## Microphone

The ES8389 capture path uses 48 kHz, signed 16-bit, stereo ALSA input and reads
the board microphone channel. Each 100 ms window produces RMS in `dBFS` and 240
normalized waveform samples. RMS is digital full-scale level, not `dB SPL` or
`dBA`; raw waveform samples are in the `-1` to `1` range.

The default ES8389 PGA gain is temporarily set to `+18 dB` and restored when the
capture stops. Override it with `LILYGO_SENSORS_MIC_GAIN_DB` from `0` to `42`.
Override the ALSA device for diagnostics with:

```sh
LILYGO_SENSORS_MIC_DEVICE=plughw:0,0 \
  lilygo-ui-sensors --probe --samples 10 --interval 250
```

Missing, busy, inaccessible, disconnected, or timed-out capture devices remain
unavailable and are retried; no fake zero level is displayed. Raw waveform data
is shown live but is not stored in CSV.
