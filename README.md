# TankBot - ESP32 Web-Controlled Tracked Robot

A web-controlled tank robot built with ESP32 and L298N motor driver, featuring real-time FPV video streaming, sensor data overlay, HTTPS security, and multiple control modes.


![TankBot in Action](images/tankbot-demo.gif)


## Hardware Components

- **Microcontroller**: ESP32 DevKit (38-pin, CP2102, USB-C)
- **Motor Driver**: L298N H-Bridge
- **Motors**: 2x 33GB-520-18.7F DC motors (included with chassis)
- **Chassis**: TP101 Tank-style chassis ([widely available](https://www.google.com/search?client=safari&rls=en&q=TP101+chassis&ie=UTF-8&oe=UTF-8))
- **Power**: 2x 18650 lithium batteries (7.4V nominal) (holder included with chassis)
- **Fasteners**: 12x M3x6 screws

### 3D Printed Parts

Custom mounting brackets and accessories are available on Printables: [TankBot ESP32 on Printables](https://www.printables.com/model/1516204-tank-bot-esp32)

## Wiring Diagram

### ESP32 to L298N Connections

| ESP32 Pin | L298N Pin | Function |
|-----------|-----------|----------|
| P16 | IN1 | Left motor direction |
| P17 | IN2 | Left motor direction |
| P18 | IN3 | Right motor direction |
| P19 | IN4 | Right motor direction |
| P25 | ENA | Left motor speed (PWM) |
| P26 | ENB | Right motor speed (PWM) |
| GND | GND | Common ground |

### L298N Setup

1. **Remove the jumpers** from ENA and ENB pins on the L298N
2. Connect ENA to P25 and ENB to P26 as shown above
3. Connect your battery power to the L298N power input
4. Connect motors to OUT1/OUT2 (left) and OUT3/OUT4 (right)

### Power Connections

The TP101 chassis includes a dual 18650 battery holder with a power switch. Wire the power as follows:

1. **Battery to L298N**:
   - Connect battery holder red wire (+) to L298N 12V input
   - Connect battery holder black wire (-) to L298N GND

2. **L298N to ESP32**:
   - Connect ESP32 5V pin to L298N 5V input (this powers the L298N's logic circuitry)
   - Connect L298N GND to ESP32 GND (common ground)
   - The ESP32 is powered via USB during programming, or via a barrel plug connector tapped from the battery wires during operation

3. **Important Notes**:
   - The dual 18650 setup provides 7.4V nominal (8.4V fully charged)
   - The batteries power the L298N's motor outputs via the 12V input
   - The ESP32's 5V pin powers the L298N's logic circuitry
   - Always connect common ground between all components
   - During operation, power the ESP32 with a barrel plug connector from the battery wires

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) installed in VSCode or via CLI
- USB cable to connect ESP32 to computer

### Installation

1. Open this project in PlatformIO:
   ```bash
   cd TankBot
   ```

2. Build the project:
   ```bash
   pio run
   ```

3. Upload to ESP32:
   ```bash
   pio run --target upload
   ```

4. Monitor serial output (optional):
   ```bash
   pio device monitor
   ```

### First Time Upload

If you encounter upload issues:
- Make sure the ESP32 is connected via USB
- Press and hold the BOOT button on ESP32 during upload if needed
- Check that the correct port is detected

## How to Use

1. **Power on the robot** - Connect battery and power up the ESP32

2. **Connect to WiFi**:
   - SSID: `TankBot`
   - Password: `tankbot2025`

3. **Open web interface**:
   - Captive portal will automatically open, or visit: `https://192.168.4.1`
   - Choose your device role from the landing page:
     - **🕹️ Basic Controls** - Simple control interface (no video)
     - **🎮 Enhanced Controls** - Drive with FPV video and sensor data overlay
     - **📹 Stream Device** - Use a phone's camera to stream video to the controller
     - **👁️ View Only** - Watch the video stream without control (access via `/view`)

4. **Control the robot**:

   **Basic Controls Mode:**
   - **Button Mode** (default): Arrow buttons for Forward, Backward, Left, Right
     - Press and hold buttons to move, release to stop
     - Red STOP button for emergency stop
   - **Joystick Mode**: Click JS button (top-left) to toggle joystick control
     - Drag joystick to control direction and turning simultaneously
     - Forward/backward movement + left/right steering
     - Release joystick to stop
   - Adjust speed slider for 3 speed levels: Slow, Medium, Fast (works in both modes)

   **Enhanced Controls Mode:**
   - Same control options as Basic Controls
   - Live FPV video feed from stream device
   - Real-time sensor data overlay (orientation, GPS location, speed)
   - Status indicator showing connection quality and FPS

5. **Set up video streaming** (for Enhanced Controls):
   - On a second device (phone/tablet), connect to TankBot WiFi
   - Choose **📹 Stream Device** from the landing page
   - Grant camera and location permissions when prompted
   - Click "Start Streaming"
   - The video will appear on any Enhanced Controls or View Only devices
   - Adjust quality settings if needed (lower = more stable)

6. **Calibrate steering** (if robot drifts to one side):
   - In Basic Controls, click the ⚙️ (gear) icon in the top-right corner
   - Drive forward and observe which direction it drifts
   - Adjust the "Steering Trim" slider:
     - If drifting LEFT: Move slider to the RIGHT
     - If drifting RIGHT: Move slider to the LEFT
   - Fine-tune until robot drives straight
   - Trim is automatically saved and persists across sessions

## Features

### Core System
- **WiFi Access Point**: Robot creates its own secure WiFi network
- **HTTPS Security**: All communications encrypted via self-signed certificate
- **Captive Portal**: Auto-popup control interface when connecting to WiFi
- **Dual-Server Architecture**:
  - HTTP server (port 80) for captive portal redirects only
  - HTTPS server (port 443) for all content and WebSocket connections
- **mDNS Support**: Access via https://tank.local
- **Optimized Performance**: 4 connection slots for fast cleanup and low latency

### Control Interfaces
- **Multi-Device Support**: Four distinct roles on same network
  - **Basic Controls** - Lightweight control interface (no video overhead)
  - **Enhanced Controls** - Full FPV control with video and sensor overlay
  - **Stream Device** - Dedicated camera streaming from secondary phone/tablet
  - **View Only** - Spectator mode with video feed (no control)
- **Responsive Web UI**: Works seamlessly on phones, tablets, and computers
- **Dual Control Modes**: Toggle between button controls and joystick
  - Button mode: Discrete directional controls (Forward, Backward, Left, Right)
  - Joystick mode: Analog control with simultaneous forward/back and turning
- **Touch Support**: Optimized for touch screens with mobile control

### Video Streaming
- **Real-Time FPV**: Live video stream from secondary device camera
- **Binary WebSocket Protocol**: Chunked transmission for reliability (2.8KB chunks)
- **Adjustable Quality**: Resolution (160p-320p), FPS (8-12), and compression settings
- **Frame Reassembly**: Robust client-side reassembly with error handling
- **Performance Optimized**: Zero-copy relay, no server-side logging overhead

### Sensor Integration
- **Device Orientation**: Real-time heading, pitch, and roll from phone's gyroscope
- **GPS Tracking**: Live latitude, longitude, altitude, and speed overlay
- **HUD Display**: On-screen sensor data overlay on Enhanced/View modes
- **Sensor Broadcasting**: 2-second intervals to reduce ESP32 load

### Motor Control
- **Speed Control**: 3 speed levels (Slow: 160, Medium: 220, Fast: 255)
- **Steering Trim**: Dual-persistence trim adjustment system
  - Client-side: localStorage for instant persistence across page navigation
  - Server-side: ESP32 Preferences (NVS) for persistence across power cycles
  - Range: -20 to +20 adjustment to compensate for uneven track tension
  - Syncs automatically between Basic and Enhanced controls
  - Works in both button and joystick modes
- **Real-time Feedback**: WebSocket-based status updates on all interfaces

## Control Mechanics

- **Forward/Backward**: Both motors run in same direction
- **Left**: Left motor backward, right motor forward (spin in place)
- **Right**: Left motor forward, right motor backward (spin in place)
- **Speed**: PWM control (0-255) on ENA/ENB pins

## Troubleshooting

### Robot doesn't move
- Check battery is charged and connected
- Verify all wiring connections
- Check that jumpers are removed from ENA/ENB
- Use serial monitor to see debug output

### Can't connect to WiFi
- ESP32 might still be booting (wait 10-15 seconds)
- Check password is correct: `tankbot2025`
- Move closer to the robot
- Restart ESP32

### Motors run at full speed regardless of setting
- Ensure jumpers are removed from ENA and ENB
- Verify P25 and P26 are connected to ENA and ENB

### Web page doesn't load
- Verify you're connected to TankBot WiFi
- Try `http://192.168.4.1` directly
- Clear browser cache
- Check serial monitor for ESP32 IP address

## Customization

### Change WiFi Credentials

Edit in [src/main.cpp](src/main.cpp):
```cpp
const char* ssid = "TankBot";
const char* password = "tankbot2025";
```

### Adjust Speed Levels

Edit in [src/main.cpp](src/main.cpp):
```cpp
#define SPEED_SLOW 100    // Range: 0-255
#define SPEED_MEDIUM 180  // Range: 0-255
#define SPEED_FAST 255    // Range: 0-255
```

### Change Pin Assignments

Edit pin definitions in [src/main.cpp](src/main.cpp):
```cpp
#define IN1 16
#define IN2 17
#define IN3 18
#define IN4 19
#define ENA 25
#define ENB 26
```

## Serial Monitor Output

When connected via serial, you'll see:
```
=== TankBot Starting ===
Motors initialized
Setting up Access Point: TankBot
AP IP address: 192.168.4.1
HTTP server started

=== TankBot Ready! ===
Connect to WiFi: TankBot
Password: tankbot2025
Then visit: http://192.168.4.1
========================
```

## License
GNU AGPL 3.0 
Open source - feel free to modify and improve!
