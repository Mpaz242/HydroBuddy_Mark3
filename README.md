# HomeKit ESP32 Template

A minimal, reusable starting point for any HomeKit-enabled ESP32 project using the [HomeSpan](https://github.com/HomeSpan/HomeSpan) library.

## Requirements

- **Board:** Any ESP32 (tested on XIAO ESP32S3, ESP32 DevKit)
- **Library:** [HomeSpan](https://github.com/HomeSpan/HomeSpan) — install via Arduino Library Manager
- **Arduino IDE** 2.x or later

## How to Use

### 1. Copy the template

Duplicate this folder and rename it for your new project. The `.ino` filename must match the folder name — Arduino requires this.

### 2. Choose your HomeKit service

In `HomeKit_ESP32_Template.ino`, replace `Service::LightBulb` in the struct definition with the service that matches your device:

| Device type         | HomeSpan service                    |
|---------------------|-------------------------------------|
| Light / LED         | `Service::LightBulb`                |
| Generic switch      | `Service::Switch`                   |
| Window blinds       | `Service::WindowCovering`           |
| Fan                 | `Service::Fan`                      |
| Temperature sensor  | `Service::TemperatureSensor`        |
| Humidity sensor     | `Service::HumiditySensor`           |
| Garage door         | `Service::GarageDoorOpener`         |
| Door lock           | `Service::LockMechanism`            |

### 3. Add your characteristics

Inside the struct, declare the characteristics your service needs:

```cpp
Characteristic::On power{false};               // on/off, default OFF
Characteristic::TargetPosition target{0};      // 0–100%, default 0
Characteristic::CurrentPosition current{0};    // read-only position
Characteristic::RotationSpeed speed{0};        // fan speed 0–100
```

### 4. Wire up your hardware

In the constructor, initialize any pins or peripherals:

```cpp
MyService() {
  pinMode(MOTOR_PIN, OUTPUT);
  ledcSetup(0, 5000, 8);  // PWM example
}
```

### 5. React to HomeKit commands

In `update()`, read the new values HomeKit sent and act on them:

```cpp
boolean update() override {
  int pos = target.getNewVal();
  moveMotorTo(pos);
  current.setVal(pos);
  return true;
}
```

### 6. Set your device identity

In `setup()`, update:
- **Pairing code** — the 8-digit PIN the Home app will ask for (avoid `12345678` and other common codes)
- **Category** — matches the device type (`Category::Lighting`, `Category::WindowCovering`, etc.)
- **Device name** — shown in the Home app
- **Manufacturer, Model, SerialNumber** — shown in device details

```cpp
homeSpan.setPairingCode("XXXXXXXX");  // your 8-digit PIN
homeSpan.begin(Category::WindowCovering, "My Window Blinds");
```

### 7. Pair with HomeKit

1. Flash the sketch to your ESP32
2. Open the **Home** app on iPhone/iPad
3. Tap **+** → **Add Accessory** → **More options**
4. Select your device and enter the pairing code

## OTA (Wireless Updates)

OTA is enabled by default. After the first USB flash, all future uploads can be done over WiFi.

**Three values to set at the top of the sketch:**

```cpp
#define OTA_HOSTNAME   "my-esp32-device"   // becomes my-esp32-device.local on the network
#define OTA_PASSWORD   "homekit123"        // required when uploading via OTA
#define SKETCH_VERSION "1.0"
```

**Upload from the terminal:**

```bash
arduino-cli upload \
  --fqbn esp32:esp32:XIAO_ESP32S3 \
  --port my-esp32-device.local \
  --upload-field password=homekit123 \
  /path/to/YourProject
```

**Upload from Arduino IDE:**
Tools → Port → select `my-esp32-device.local`, then click Upload. The IDE will prompt for the OTA password.

> OTA is handled entirely by HomeSpan — `homeSpan.poll()` listens for incoming updates automatically. No extra code needed in `loop()`.

## Project Structure

```
HomeKit_ESP32_Template/
├── HomeKit_ESP32_Template.ino   ← Main sketch (rename to match folder)
└── README.md                    ← This file
```

## Tips

- Use `homeSpan.setLogLevel(2)` and open Serial Monitor at 115200 baud to debug pairing issues
- If re-pairing fails, hold the ESP32 boot button for 10 seconds to reset HomeKit storage
- The struct-based pattern (`struct MyService : Service::...`) is the recommended HomeSpan approach — it keeps all service logic self-contained and makes it easy to add multiple services to one accessory

## Based On

Built from two working ESP32 HomeKit projects:
- `ESP32_AutoWindowBlinds` — basic HomeKit polling pattern
- `ESP32_AutoWindowBlinds_LED` — struct-based service pattern with GPIO control (XIAO ESP32S3, GPIO2)
