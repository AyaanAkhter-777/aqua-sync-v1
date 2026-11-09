# AquaSync V1 - Roof Water Tank Monitor

A low-cost water level monitor for an overhead tank, built on an **ESP32** and a **JSN-SR04T** waterproof ultrasonic sensor. It measures the water level, tracks fills, keeps a 24-hour history, and serves everything over Wi-Fi (HTTP/JSON) and Bluetooth LE so a phone app can show it live.

V1 is **monitoring only**. There is no motor control and no mains wiring.

> Status: tested on a bucket (calibrated, multiple runs). Roof installation and test video coming soon.

## Features

- Live water level in percent, plus raw distance in cm
- Fill counter and time of the last fill, saved in flash so they survive a reboot
- 24-hour level history (one sample every 10 minutes), saved in flash
- Sensor error detection: `no echo`, `too close`, `out of range`
- Noise-resistant readings: median filter plus smoothing
- Wi-Fi HTTP server with JSON endpoints and CORS enabled for web apps
- Bluetooth LE status notifications, running together with Wi-Fi
- Automatic Wi-Fi reconnect, with a fallback access point (`AquaSync_AP`) if the home network is not found
- NTP time sync (set to India, UTC+5:30)

## Hardware

| Part | Notes |
|---|---|
| ESP32 DevKit | Any common 30/38-pin DevKit |
| JSN-SR04T | Waterproof ultrasonic sensor with a separate control board |
| 1k and 2k resistors | Voltage divider for the ECHO line |
| 470 uF capacitor | Across the sensor's VCC and GND, close to the sensor |
| 5V supply | USB for bench testing |

Planned for the roof install: IP65 enclosure, cable gland, 12V 1A adapter placed indoors, a 12V to 5V buck converter on the roof, and a 4-core cable between them.

## Wiring

| Sensor pin | ESP32 |
|---|---|
| VCC | VIN (5V) |
| GND | GND |
| TRIG | GPIO5 |
| ECHO | GPIO18, **through a voltage divider** (see below) |

**Why the divider:** the sensor's ECHO pin outputs 5V, but ESP32 pins tolerate only 3.3V.

```
ECHO ---[ 1k ]---+---> GPIO18
                 |
               [ 2k ]
                 |
                GND
```

Put the 470 uF capacitor across the sensor's VCC and GND, close to the sensor. The long leg goes to VCC and the leg marked with a stripe goes to GND.

The JSN-SR04T cannot measure closer than about 20 cm. Mount it so the water never gets nearer than that to the sensor face.

## Setup

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) and add ESP32 board support (Boards Manager, "esp32" by Espressif).
2. Open `Aqua-Sync-Bucket-Test-V1.ino`.
3. Set your Wi-Fi details in the user settings block:

   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* AP_PASS       = "CHANGE_ME_8_CHARS_MIN";
   ```

   Never commit your real Wi-Fi password to a public repository.
4. Select your ESP32 board and port, then upload.
5. Open the Serial Monitor at **115200 baud**. You should see the IP address once Wi-Fi connects.

## Calibration

Level is calculated from two numbers in the settings block:

```cpp
const float TANK_HEIGHT_CM   = 50.0f;  // sensor-to-bottom distance when the tank is empty
const float SENSOR_OFFSET_CM = 20.0f;  // sensor-to-water distance when the tank is full
```

The values in this repo are for the **bucket test** (empty = 50 cm, full = 20 cm). For a real tank:

1. Measure the distance from the sensor face to the bottom of the tank (empty). Put it in `TANK_HEIGHT_CM`.
2. Measure the distance from the sensor face to the water surface when the tank is full. Put it in `SENSOR_OFFSET_CM`.
3. Upload again and compare the reported level with the real water level.

Readings below `MIN_VALID_CM` (18 cm) or above `TANK_HEIGHT_CM + 15` are treated as invalid.

## HTTP API

The device uses hostname `aquasync` and serves on port 80.

| Method | Endpoint | Description |
|---|---|---|
| GET | `/status` | Current level, distance, sensor state, fills, time, Wi-Fi signal, uptime |
| GET | `/history` | Last 144 level samples, 10 minutes apart (`-1` means sensor error at that time) |
| POST | `/reset` | Clears the smoothing window and fill tracker state |
| POST | `/motor` | Placeholder for V2, currently just returns the status |

Example `/status` response:

```json
{
  "hw": 1,
  "level": 72,
  "distance": 34,
  "sensor": true,
  "motor": false,
  "full": false,
  "fills": 3,
  "last_fill": 1731150000,
  "time": 1731160000,
  "rssi": -61,
  "up": 5230
}
```

If the sensor has a problem, `sensor` is `false` and an `error` field is added (`no echo`, `too close` or `out of range`).

## Bluetooth LE

The ESP32 advertises a BLE service and sends a compact status JSON as notifications about once per update. UUIDs are defined at the top of the sketch.

## How fill tracking works

A fill starts when the level rises 8 percentage points above its recent low. It ends when the level stops rising for 2 minutes, or reaches 98 percent. Each finished fill increments the counter and stores the time.

## Roadmap

- [x] V1: bucket test, level, fills, history, Wi-Fi + BLE
- [ ] V1: roof installation with weatherproof enclosure
- [ ] V2: motor control with motor-on detection

## Demo

Test video: _coming soon_

## License

Add a license of your choice (MIT is a common option for hobby projects).
