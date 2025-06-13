# ESP32 Web Debugger (Pure ESP-IDF)

This is a fully web-based GPIO/serial/pin monitor/debug tool for ESP32, written entirely in **pure ESP-IDF v5.3.1** (no Arduino libraries). It runs a real-time WebSocket and HTTP server, giving you control over GPIO modes, pin output states, UART, I2C, analog input, and a live oscilloscope-style voltage graph from a selected ADC pin.

---

## ⚙️ Features

- 🔌 GPIO control (input/output toggle)
- 🔄 Pin state updates with debounce and bounce count tracking
- ⚡ Real-time WebSocket client updates
- 🧲 PWM output (⚠️ *broken*)
- 📈 Oscilloscope view of analog voltage (ADC)
- 🔧 UART send/receive window
- 🔍 I2C scanner (reports detected addresses)
- 📡 Built-in WiFi STA mode + SPIFFS-based HTML interface

---

## 🧪 Current Status / Known Bugs

| Feature            | Status           | Notes                                             |
|--------------------|------------------|--------------------------------------------------|
| Web Interface      | ✅ Working        | Serves over SPIFFS and updates in real time     |
| GPIO Mode/State    | ✅ Working        | Input/Output, with bounce tracking               |
| ADC Oscilloscope   | ✅ Stable         | Smooth and responsive, with voltage smoothing    |
| I2C Scanner        | ✅ Working        | Scans 0x01 to 0x7F and returns live results      |
| UART               | ✅ Basic support  | Can send/receive short ASCII messages            |
| PWM Output         | ⚠️ broken         | Pins are registered, but signal not always output |
| WebSocket Stability| ✅ Stableish      | `ws_clients` array initialized and functional    |

---

## 🔧 Requirements

- ESP32 module (e.g. WROOM, WROVER)
- ESP-IDF v5.3.1 (tested)
- Web browser (Chrome or Firefox recommended)
- WiFi connection to same network as ESP32

---

## 📦 Building & Flashing

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor

Make sure you’ve set the correct port and board.
📁 SPIFFS Setup

Before first boot, upload your index.html and frontend files to SPIFFS:

    Make a spiffs folder in your project root and place your UI files there (e.g., index.html, style.css, etc.)

    Add spiffs_image target to your CMakeLists.txt

    Run:

idf.py -p COMx spiffs

📡 WiFi Setup

Edit these in main.c before building:

#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"

The ESP32 will attempt to connect to your local WiFi network and host the server. The IP address will be printed to serial output after boot.
🌐 Usage

    Connect your computer/phone to the same WiFi network as the ESP32.

    Open your browser and visit the ESP32’s IP address (shown in serial monitor).

    Use the web interface to toggle GPIOs, view analog voltage, scan I2C devices, or send UART commands.

🛠 Future Plans

Fully working PWM with frequency + duty setting

GPIO interrupt monitoring and edge logging

SPI interface tools

File upload interface via Web UI

    Better frontend polish + mobile UI

📎 Notes

This is an evolving project originally built using ESP32-Arduino but now rewritten fully in ESP-IDF for performance, control, and stability. While still buggy in some areas (notably PWM), most features are already functional and usable. More stability improvements and frontend tools are coming soon.
📬 License

MIT — use it, remix it, contribute if you want.
