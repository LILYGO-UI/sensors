# Sensor Operation And Diagnostics

[Back to README](../README.md)

## Data Sources

The application uses the following Linux interfaces for measurements and diagnostics.

| Source | Data |
| --- | --- |
| `/sys/bus/iio/devices` | Accelerometer, gyroscope, chip temperature, and environmental sensors |
| `/sys/class/hwmon` | K230 AHT20 ambient temperature and relative humidity, CPU, battery, and electrical channels |
| `/sys/class/sound` and ALSA | K230 internal INNO codec or CM0 ES8389 microphone capture |
| `/sys/class/input` and evdev | TCA8418 keyboard events |
| `/dev/i2c-*` | Bus inventory and K230 AHT20 readings when the kernel driver fails to bind |

I2C probing covers `0x03` through `0x77` and uses SMBus Quick or Receive Byte.
It does not write device configuration registers. A responding address is not
proof that a device is identified or that it can provide measurements; registered
devices and readable channels are reported separately.

IIO values use the driver's `raw`, `offset`, and `scale` attributes. Temperatures
are shown in degrees Celsius, humidity in `%RH`, pressure in `hPa`, light in
`lx`, magnetic field in `uT`, and hwmon electrical values in `V`, `A`, or `W`.
Read failures remain errors with unavailable values; zero is never used as a
placeholder.

### AHT20 on T-Display-K230

Only the T-Display-K230 has an onboard AHT20. Open **AHT20** in Sensors to see
**Ambient temperature** (°C) and **Relative humidity** (%). The K230 BSP declares
the device at I2C address `0x38`. The app discovers its `aht20` hwmon node at
runtime, so the I2C adapter number and `hwmonN` index can vary. In `--probe`
output, both readings have `device` set to `aht20` and share a `device_id`.
On K230 kernels where the declared AHT20 client has no bound driver, Sensors
uses its I2C adapter to read a CRC-checked measurement every two seconds.
The `wing` account needs membership in the `i2c` group for this path.

### BQ25896 on T-Display-K230

On K230, **BQ25896** reads the charger identified by `model_name` under
`/run/lilygo-k230/power_supply/bq25890-charger`. The board service publishes
USB input voltage in `voltage_now` and measured charge current in `current_now`.
Current limits and charge targets have separate labels. Charging status, charge
type and health appear as text in the device view and in `--probe` (`text_value`).
Numeric channels support history and CSV recording. When a kernel power supply
identifies the chip directly, its `voltage_now` reports battery voltage.

### BQ27220 on T-Display-K230

Select **BQ27220** to inspect the battery fuel gauge. Sensors reads the Linux
`power_supply` attributes or the board service's `bq27220-battery` directory
under `/run/lilygo-k230/power_supply`. The gauge's I2C address is `0x55`.
The K230 service reports positive battery current in mA while charging
and negative mA while discharging. Remaining, full charge and design capacities
are in mAh. Time-to-empty and time-to-full appear in minutes when published.

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

On K230 the Goertek B4012AP422-003 microphone connects to the internal INNO codec.
Sensors opens `plughw:CARD=K230I2SINNO,DEV=0` as 48 kHz S16_LE mono. During
capture it requests the internal audio route from `lilygo-k230-audio.service`.
After closing the PCM it restores the external speaker route if it switched it.
On CM0, Sensors reads the left channel of the ES8389 stereo capture PCM.
Each 100 ms window produces RMS in `dBFS` and 240
normalized waveform samples. RMS is digital full-scale level, not `dB SPL` or
`dBA`; raw waveform samples are in the `-1` to `1` range.

On CM0 the default ES8389 PGA gain is temporarily set to `+18 dB` and restored when the
capture stops. Override it with `LILYGO_SENSORS_MIC_GAIN_DB` from `0` to `42`.
An explicit ALSA override selects generic stereo/left capture and leaves the
board route and mixer under the caller's control.

```sh
LILYGO_SENSORS_MIC_DEVICE=plughw:0,0 \
  lilygo-ui-sensors --probe --samples 10 --interval 250
```

Missing, busy, inaccessible, disconnected, or timed-out capture devices remain
unavailable and are retried; no fake zero level is displayed. Raw waveform data
is shown live but is not stored in CSV.
