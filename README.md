# ESP32 Spotify Controller

A physical Spotify controller built with ESP32, featuring a TFT display and hardware controls for playback management.

![Project Demo](docs/demo.gif) <!-- Add your demo GIF/image here -->

## Features

- 🎵 **Playback Control**: Play, pause, next, previous track
- 🔊 **Volume Management**: Rotary encoder for volume adjustment with mute toggle
- 🔀 **Shuffle Toggle**: Hardware button to enable/disable shuffle
- ❤️ **Like Songs**: Add/remove tracks from your Spotify Liked Songs
- 📊 **Real-time Display**: Shows current track, artist, progress, and playback device
- 🌐 **WiFi Connected**: Communicates directly with Spotify API
- ⚡ **Responsive UI**: Built with LVGL for smooth graphics
- 🈚 **CJK Support**: Displays Japanese/Chinese/Korean artist names and tracks

## Hardware Requirements

### Components
- **ESP32 Development Board** (NodeMCU-32S or compatible)
- **TFT Display** - 240x320 ST7789/ST7735 (SPI interface)
- **Rotary Encoder** with push button
- **6x Push Buttons** (for playback controls)
- **LED** (optional, for visual feedback)
- **Resistors** - 10kΩ pull-up resistors (if needed)
- Breadboard and jumper wires

### Pinout

| Component | ESP32 GPIO | Notes |
|-----------|-----------|-------|
| **TFT Display** | | |
| MOSI/SDA | 23 | SPI Data |
| SCLK/SCL | 18 | SPI Clock |
| CS | 15 | Chip Select |
| DC | 2 | Data/Command |
| RST | 4 | Reset |
| BLK | 3.3V | Backlight (or PWM pin) |
| **Rotary Encoder** | | |
| CLK | 13 | Clock |
| DT | 14 | Data |
| SW | 32 | Switch/Button |
| **Control Buttons** | | |
| Previous | 25 | INPUT_PULLUP |
| Play | 26 | INPUT_PULLUP |
| Pause | 33 | INPUT_PULLUP |
| Next | 27 | INPUT_PULLUP |
| Shuffle | 5 | INPUT_PULLUP |
| Like | 0 | INPUT_PULLUP (avoid holding during boot) |
| **LED** | | |
| Status LED | 22 | OUTPUT |

### Wiring Notes
- All buttons use internal pull-up resistors (active LOW)
- GPIO 0 (Like button) - avoid holding during boot/flashing
- Connect TFT display to 3.3V (NOT 5V)
- Common ground for all components

## Software Dependencies

### PlatformIO Libraries
```ini
lib_deps = 
    bblanchon/ArduinoJson@^7.0.0
    finianlandes/SpotifyEsp32@^3.0.0
    bodmer/TFT_eSPI@^2.5.43
    lvgl/lvgl@^9.4.0
    madhephaestus/ESP32Encoder@^0.12.0
```

### Required Files
- `secrets.h` - WiFi and Spotify credentials (see Configuration)
- `ui.h/ui.cpp` - LVGL UI files (generated from SquareLine Studio)
- NotoSansCJK_Regular_compressed_v2.c is to be placed in the folder .pio\libdeps\nodemcu-32s\lvgl\src\font\NotoSansCJK_Regular_compressed_v2.c
- User_Setup.h is to replace the same file found in .pio\libdeps\nodemcu-32s\TFT_eSPI\User_Setup.h
- lv_conf.h is to replace the same file found in .pio\libdeps\nodemcu-32s\lvgl\lv_conf.h

## Installation

### 1. Clone Repository
```bash
git clone https://github.com/yourusername/esp32-spotify-controller.git
cd esp32-spotify-controller
```

### 2. Configure Spotify API

1. Go to [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
2. Create a new app
3. Note your `Client ID` and `Client Secret`
4. Add `http://localhost:8888/callback` to Redirect URIs
5. Get your Refresh Token using [this tool](https://github.com/SpotifyEsp32/RefreshTokenGenerator)

### 3. Create `secrets.h`

Create `src/secrets.h`:
```cpp
#ifndef SECRETS_H
#define SECRETS_H

// WiFi Credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Spotify API Credentials
#define CLIENT_ID "your_spotify_client_id"
#define CLIENT_SECRET "your_spotify_client_secret"
#define REFRESH_TOKEN "your_spotify_refresh_token"

#endif
```

### 4. Configure TFT_eSPI

Edit `lib/TFT_eSPI/User_Setup.h` or create a custom setup:
```cpp
#define ST7789_DRIVER  // Or ST7735_DRIVER for your display

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
```

### 5. Build and Upload

Using PlatformIO:
```bash
pio run -t upload
pio device monitor
```

## Configuration

### platformio.ini
```ini
[env:nodemcu-32s]
platform = espressif32
board = nodemcu-32s
framework = arduino
upload_speed = 921600
monitor_speed = 115200
build_type = release

board_build.partitions = huge_app.csv

build_flags = 
    -Os
    -DCORE_DEBUG_LEVEL=0
    -DBOARD_HAS_PSRAM=0
    -DLVGL_OPTIMIZE_SIZE=1
    -DCONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ=240
    -DCONFIG_BT_ENABLED=0
    -DCONFIG_BLE_ENABLED=0
    -DCONFIG_BLUEDROID_ENABLED=0
```

### Memory Optimization

The project uses ~95% of flash due to LVGL and CJK fonts. To reduce size:
- Regenerate fonts with only needed characters
- Use compressed font format in LVGL font converter
- Disable unused LVGL features in `lv_conf.h`

## Usage

### Controls

| Button/Input | Action |
|--------------|--------|
| Play Button | Resume/Start playback |
| Pause Button | Pause playback |
| Next Button | Skip to next track |
| Previous Button | Previous track |
| Shuffle Button | Toggle shuffle mode |
| Like Button | Add/remove from Liked Songs |
| Rotary CW | Increase volume (+2) |
| Rotary CCW | Decrease volume (-2) |
| Rotary Press | Mute/Unmute |

### Display Information
- Current track name
- Artist name
- Playback device
- Progress bar with time
- Shuffle status (green icon when enabled)
- Like status (heart icon)
- Current time and date

## Project Structure
```
esp32-spotify-controller/
├── src/
│   ├── main.cpp              # Main application logic
│   ├── ui.cpp/ui.h           # LVGL UI (from SquareLine)
│   ├── secrets.h             # Credentials (not in repo)
│   ├── debouncer.h/cpp       # Button debouncing
│   ├── rotary.h/cpp          # Rotary encoder handler
│   ├── output_pin.h/cpp      # LED control
│   ├── esp_time.h/cpp        # Time/date functions
│   └── fonts/                # Custom CJK fonts
├── platformio.ini            # PlatformIO configuration
├── README.md                 # This file
└── docs/                     # Documentation and images
```

## Architecture

### Dual-Core Processing
- **Core 0**: LVGL GUI rendering and display updates
- **Core 1**: Spotify API calls and background tasks

### Thread-Safe Data Exchange
- Mutex-protected data sharing between cores
- Non-blocking API calls
- Smooth UI updates without freezing

### API Polling Strategy
- Updates every 1 second
- Immediate execution of button actions
- Smart progress interpolation between API calls

## Troubleshooting

### Common Issues

**PSRAM Error on Boot**
```
E (758) psram: PSRAM ID read error: 0xffffffff
```
Solution: Remove `board_build.arduino.memory_type` from platformio.ini

**Display Not Working**
- Check TFT_eSPI configuration matches your display
- Verify wiring (especially CS, DC, RST pins)
- Ensure backlight (BLK) is connected to 3.3V

**Buttons Not Responding**
- GPIO 0 can cause boot issues - avoid holding during power-on
- Check pull-up resistor configuration
- Add serial debug to verify button readings

**Flash Memory Full**
```
Flash: [==========] 95%+
```
Solution: Regenerate CJK fonts with reduced character set

**WiFi Connection Failed**
- Verify credentials in `secrets.h`
- Check 2.4GHz WiFi (ESP32 doesn't support 5GHz)
- Ensure strong signal strength

## Future Improvements

- [ ] Battery power option

## Credits

- [SpotifyEsp32](https://github.com/SpotifyEsp32/SpotifyEsp32) - Spotify API library
- [LVGL](https://lvgl.io/) - Graphics library
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) - Display driver

## License

This project is licensed under the MIT License - see [LICENSE](LICENSE) file for details.

## Contributing

Pull requests are welcome! For major changes, please open an issue first to discuss what you would like to change.

---

**⭐ If you found this project helpful, please give it a star!**
```

## Additional Files to Create

### 1. `.gitignore`
```
.pio/
.vscode/
src/secrets.h
*.bin
*.elf
```

### 2. `LICENSE` (MIT Example)
```
MIT License

Copyright (c) 2025 [Your Name]

Permission is hereby granted, free of charge, to any person obtaining a copy...
