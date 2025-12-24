# TankBot Project Context

## Project Overview
ESP32-based tracked robot with multi-device web control, real-time FPV video streaming, and sensor integration.

## Architecture

### Hardware
- ESP32 DevKit (38-pin, CP2102, USB-C)
- L298N motor driver
- 2x DC motors (33GB-520-18.7F)
- 2x 18650 batteries (7.4V)

### Software Stack
- **Platform**: PlatformIO (Arduino framework)
- **Web Servers**:
  - AsyncWebServer (port 80) - HTTP redirects only
  - esp32_https_server (port 443) - All content + WebSocket
- **WebSocket Protocol**: Binary streaming (2.8KB chunks) for video
- **Storage**: ESP32 Preferences (NVS) for persistent settings

### Pin Configuration
```cpp
#define IN1 16    // Left motor direction
#define IN2 17    // Left motor direction
#define IN3 18    // Right motor direction
#define IN4 19    // Right motor direction
#define ENA 25    // Left motor PWM
#define ENB 26    // Right motor PWM
```

### Key Files
- `src/main.cpp` - ESP32 firmware (HTTPS server, WebSocket, motor control)
- `data/landing.html` - Main menu
- `data/basic.html` - Basic controls (no video)
- `data/enhanced.html` - Enhanced controls with FPV video
- `data/stream-source.html` - Camera streaming device
- `data/view.html` - View-only mode
- `platformio.ini` - Build configuration

## Current Features

### Multi-Device Roles
1. **Basic Controls** - Motor control only (no video overhead)
2. **Enhanced Controls** - Motor control + video + sensor overlay
3. **Stream Device** - Provides camera feed to controllers/viewers
4. **View Only** - Spectator mode with video feed

### WebSocket Message Protocol
```javascript
// Registration
{type: 'register', role: 'controller|streamer|viewer', client_id: timestamp}

// Motor control
{type: 'motor', direction: 'forward|backward|left|right|stop', client_id: id}

// Speed control
{type: 'speed', value: 1|2|3, client_id: id}  // Slow|Medium|Fast

// Trim adjustment
{type: 'trim', value: -20 to 20, client_id: id}

// Sensor data
{type: 'sensor', heading: float, pitch: float, roll: float,
 latitude: float, longitude: float, altitude: float, speed: float}

// Video chunks (binary)
Binary: [chunkIndex(1), totalChunks(1), ...jpegData]
```

### Persistence System
- **Client-side**: localStorage (instant, survives page refresh)
- **Server-side**: ESP32 Preferences/NVS (survives power cycles)
- **Synced**: Trim settings sync between localStorage and ESP32

### Performance Optimizations
- 4 HTTPS connection slots (reduced from 6 for faster cleanup)
- Zero-copy WebSocket relay for video chunks
- No server-side logging for video frames
- Mutex-protected shared buffers
- Direct binary pass-through (no reassembly on ESP32)

## Common Development Tasks

### Build and Upload
```bash
pio run --target upload
```

### Monitor Serial Output
```bash
pio device monitor
```

### Upload Filesystem (HTML/CSS/JS)
```bash
pio run --target uploadfs
```

### Create New Branch
```bash
git checkout -b feature-name
```

### WiFi Credentials
- SSID: `TankBot`
- Password: `tankbot2025`
- IP: `192.168.4.1`

## Known Issues & Constraints
- ESP32 has limited RAM (~320KB) - keep buffers small
- WebSocket chunks limited to <4KB (currently 2.8KB)
- HTTPS is slower than HTTP (expected trade-off for security)
- Video streaming uses significant bandwidth - adjust quality as needed
- Captive portal detection varies by OS/browser

## Dependencies
```ini
lib_deps =
    https://github.com/esphome/ESPAsyncWebServer.git
    https://github.com/esphome/AsyncTCP.git
    https://github.com/johnsonfarmsus/esp32_https_server.git
```

## Important Notes
- Always test changes on actual hardware (ESP32 behavior differs from simulator)
- Keep video chunks under 4KB to avoid WebSocket frame issues
- Trim values must persist in BOTH localStorage AND ESP32 Preferences
- Role registration prevents multiple controllers (enforced server-side)
- HTTPS requires WSS (not WS) for WebSocket connections
