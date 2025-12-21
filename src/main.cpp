#include <Arduino.h>
#include <WiFi.h>
// Include ESPAsyncWebServer first to use its HTTP_* enum values
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
// Include PsychicHttp last and undef conflicting macros
#undef HTTP_GET
#undef HTTP_POST
#undef HTTP_PUT
#undef HTTP_DELETE
#undef HTTP_HEAD
#undef HTTP_PATCH
#undef HTTP_OPTIONS
#include <PsychicHttp.h>
#include "cert_pem.h"

// Motor control pins
#define IN1 16  // Left motor direction
#define IN2 17  // Left motor direction
#define IN3 18  // Right motor direction
#define IN4 19  // Right motor direction
#define ENA 25  // Left motor speed (PWM)
#define ENB 26  // Right motor speed (PWM)

// PWM settings
#define PWM_FREQ 1000
#define PWM_RESOLUTION 8
#define PWM_CHANNEL_A 0
#define PWM_CHANNEL_B 1

// Speed levels (0-255)
#define SPEED_SLOW 160
#define SPEED_MEDIUM 220
#define SPEED_FAST 255

// Connection limits
#define MAX_TOTAL_CLIENTS 6
#define MAX_VIEWERS 4

// WiFi credentials
const char* ssid = "TankBot";
const char* password = "tankbot2025";

// HTTP AsyncWebServer for all pages and WebSocket (port 80)
AsyncWebServer httpServer(80);
AsyncWebSocket ws("/ws");

// HTTPS PsychicHttp server for secure pages and WebSocket (port 443)
PsychicHttpsServer secureServer;
PsychicWebSocketHandler wss;

// DNS server for captive portal
DNSServer dnsServer;
const byte DNS_PORT = 53;

// Preferences for persistent storage
Preferences preferences;

// Current speed setting
int currentSpeed = SPEED_MEDIUM;

// Trim adjustment for compensating track tension differences
int motorTrim = 0;

// Client tracking structures
struct TankBotClient {
  uint32_t id;
  String role;  // "controller", "streamer", "viewer", "none"
};

std::vector<TankBotClient> clients;
uint32_t controllerClientId = 0;
uint32_t streamerClientId = 0;

// Secure WebSocket client tracking (for HTTPS connections)
PsychicWebSocketClient* streamerClient = nullptr;
PsychicWebSocketClient* controllerClient = nullptr;

// Frame counter for viewer frame skipping
uint32_t frameCounter = 0;

// Function declarations
void stopMotors();
void moveForward();
void moveBackward();
void turnLeft();
void turnRight();
void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len);
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
String getConnectionStatus();
TankBotClient* getClientById(uint32_t id);
int getViewerCount();

// Landing page HTML
const char LANDING_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>TankBot Control Center</title>
  <style>
    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }

    html, body {
      font-family: Arial, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      height: 100vh;
      width: 100vw;
      overflow: hidden;
      position: fixed;
    }

    body {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }

    .container {
      background: white;
      border-radius: 20px;
      padding: 40px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
      max-width: 500px;
      width: 100%;
    }

    h1 {
      text-align: center;
      color: #333;
      margin-bottom: 10px;
      font-size: 2.2em;
    }

    .subtitle {
      text-align: center;
      color: #666;
      margin-bottom: 30px;
      font-size: 1em;
    }

    .status-info {
      background: #f0f0f0;
      border-radius: 10px;
      padding: 15px;
      margin-bottom: 30px;
      text-align: center;
    }

    .status-item {
      margin: 5px 0;
      color: #555;
    }

    .mode-buttons {
      display: grid;
      gap: 15px;
    }

    .mode-btn {
      background: #667eea;
      color: white;
      border: none;
      border-radius: 12px;
      padding: 20px;
      font-size: 1.1em;
      cursor: pointer;
      transition: all 0.3s;
      text-align: left;
      position: relative;
    }

    .mode-btn:hover {
      background: #5568d3;
      transform: translateY(-2px);
      box-shadow: 0 5px 15px rgba(102, 126, 234, 0.4);
    }

    .mode-btn:active {
      transform: translateY(0);
    }

    .mode-btn.full {
      background: #ccc;
      cursor: not-allowed;
    }

    .mode-btn.full:hover {
      transform: none;
      box-shadow: none;
    }

    .mode-title {
      font-weight: bold;
      font-size: 1.2em;
      margin-bottom: 5px;
    }

    .mode-desc {
      font-size: 0.85em;
      opacity: 0.9;
    }

    @media (max-width: 480px) {
      .container {
        padding: 25px;
      }

      h1 {
        font-size: 1.8em;
      }

      .mode-btn {
        padding: 15px;
        font-size: 1em;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>TankBot</h1>
    <div class="subtitle">Control Center</div>

    <div class="mode-buttons">
      <button class="mode-btn" id="btnBasic" onclick="location.href='/basic'">
        <div class="mode-title">Basic Controls</div>
        <div class="mode-desc">Simple button and joystick interface</div>
      </button>

      <a href="https://192.168.4.1/enhanced" target="_blank" class="mode-btn" id="btnEnhanced" style="display: flex; flex-direction: column; align-items: center; justify-content: center; text-decoration: none;">
        <div class="mode-title">Enhanced Controls</div>
        <div class="mode-desc">FPV mode with video overlay and sensor data</div>
      </a>

      <button class="mode-btn" id="btnView" onclick="location.href='/view'">
        <div class="mode-title">View Stream</div>
        <div class="mode-desc">Watch-only mode (no controls)</div>
      </button>

      <a href="https://192.168.4.1/stream-source" target="_blank" class="mode-btn" id="btnStream" style="display: flex; flex-direction: column; align-items: center; justify-content: center; text-decoration: none;">
        <div class="mode-title">Stream Device</div>
        <div class="mode-desc">Use this device as camera source (grant camera permission first)</div>
      </a>
    </div>
  </div>

  <script>
    let ws = null;

    function connectWebSocket() {
      ws = new WebSocket('ws://' + window.location.hostname + ':80/ws');

      ws.onopen = function() {
        console.log('WebSocket connected');
        ws.send(JSON.stringify({type: 'status_request'}));
      };

      ws.onmessage = function(event) {
        const msg = JSON.parse(event.data);
        if (msg.type === 'status') {
          updateStatus(msg);
        }
      };

      ws.onerror = function(error) {
        console.error('WebSocket error:', error);
      };

      ws.onclose = function() {
        console.log('WebSocket disconnected');
        setTimeout(connectWebSocket, 2000);
      };
    }

    function updateStatus(status) {
      const info = document.getElementById('statusInfo');
      const controllerStatus = status.controller_available ? 'Available' : 'In Use';
      const streamerStatus = status.streamer_active ? 'Active' : 'None';

      info.innerHTML = `
        <div class="status-item">Controller: ${controllerStatus}</div>
        <div class="status-item">Stream: ${streamerStatus}</div>
        <div class="status-item">Viewers: ${status.viewer_count}/4</div>
        <div class="status-item">Connections: ${status.total_clients}/6</div>
      `;

      // Disable buttons if full
      const btnEnhanced = document.getElementById('btnEnhanced');
      const btnView = document.getElementById('btnView');
      const btnStream = document.getElementById('btnStream');

      if (!status.controller_available) {
        btnEnhanced.classList.add('full');
        btnEnhanced.onclick = () => alert('Controller already in use by another device.');
      } else {
        btnEnhanced.classList.remove('full');
        btnEnhanced.onclick = () => location.href = '/enhanced';
      }

      if (status.streamer_active) {
        btnStream.classList.add('full');
        btnStream.onclick = () => alert('A stream device is already active.');
      } else {
        btnStream.classList.remove('full');
        btnStream.onclick = () => location.href = '/stream-source';
      }

      if (status.viewer_count >= ${MAX_VIEWERS}) {
        btnView.classList.add('full');
        btnView.onclick = () => alert('Maximum viewers reached.');
      } else {
        btnView.classList.remove('full');
        btnView.onclick = () => location.href = '/view';
      }
    }

    connectWebSocket();
  </script>
</body>
</html>
)=====";

void setupMotors() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  ledcSetup(PWM_CHANNEL_A, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(PWM_CHANNEL_B, PWM_FREQ, PWM_RESOLUTION);

  ledcAttachPin(ENA, PWM_CHANNEL_A);
  ledcAttachPin(ENB, PWM_CHANNEL_B);

  stopMotors();
}

void stopMotors() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  ledcWrite(PWM_CHANNEL_A, 0);
  ledcWrite(PWM_CHANNEL_B, 0);
}

void moveForward() {
  int leftSpeed = currentSpeed;
  int rightSpeed = currentSpeed;

  if (motorTrim < 0) {
    leftSpeed = currentSpeed + motorTrim;
  } else if (motorTrim > 0) {
    rightSpeed = currentSpeed - motorTrim;
  }

  leftSpeed = constrain(leftSpeed, 0, 255);
  rightSpeed = constrain(rightSpeed, 0, 255);

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  ledcWrite(PWM_CHANNEL_A, leftSpeed);
  ledcWrite(PWM_CHANNEL_B, rightSpeed);
}

void moveBackward() {
  int leftSpeed = currentSpeed;
  int rightSpeed = currentSpeed;

  if (motorTrim < 0) {
    leftSpeed = currentSpeed + motorTrim;
  } else if (motorTrim > 0) {
    rightSpeed = currentSpeed - motorTrim;
  }

  leftSpeed = constrain(leftSpeed, 0, 255);
  rightSpeed = constrain(rightSpeed, 0, 255);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  ledcWrite(PWM_CHANNEL_A, leftSpeed);
  ledcWrite(PWM_CHANNEL_B, rightSpeed);
}

void turnLeft() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  ledcWrite(PWM_CHANNEL_A, currentSpeed);
  ledcWrite(PWM_CHANNEL_B, currentSpeed);
}

void turnRight() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  ledcWrite(PWM_CHANNEL_A, currentSpeed);
  ledcWrite(PWM_CHANNEL_B, currentSpeed);
}

// Get client by ID
TankBotClient* getClientById(uint32_t id) {
  for (auto &client : clients) {
    if (client.id == id) {
      return &client;
    }
  }
  return nullptr;
}

// Get viewer count
int getViewerCount() {
  int count = 0;
  for (auto &client : clients) {
    if (client.role == "viewer") {
      count++;
    }
  }
  return count;
}

// Get connection status
String getConnectionStatus() {
  StaticJsonDocument<256> doc;
  doc["type"] = "status";
  doc["controller_available"] = (controllerClientId == 0);
  doc["streamer_active"] = (streamerClientId != 0);
  doc["viewer_count"] = getViewerCount();
  doc["total_clients"] = clients.size();

  String output;
  serializeJson(doc, output);
  return output;
}

// Handle WebSocket messages
void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, (char*)data);

    if (error) {
      Serial.print("JSON parse error: ");
      Serial.println(error.c_str());
      return;
    }

    String type = doc["type"];
    uint32_t clientId = client->id();  // Use actual WebSocket client ID

    // Handle status request
    if (type == "status_request") {
      AsyncWebSocketClient *client = ws.client(clientId);
      if (client) {
        client->text(getConnectionStatus());
      }
      return;
    }

    // Handle role registration
    if (type == "register") {
      String role = doc["role"];
      Serial.printf("Registration request from client %u for role: %s\n", clientId, role.c_str());
      TankBotClient *client = getClientById(clientId);

      if (!client) {
        // New client
        TankBotClient newClient;
        newClient.id = clientId;
        newClient.role = "none";
        clients.push_back(newClient);
        client = getClientById(clientId);
      }

      // Check if role is available
      bool roleAccepted = false;
      StaticJsonDocument<128> response;

      if (role == "controller") {
        if (controllerClientId == 0 || controllerClientId == clientId) {
          controllerClientId = clientId;
          client->role = "controller";
          roleAccepted = true;
        } else {
          response["type"] = "role_rejected";
          response["role"] = "controller";
          response["reason"] = "Controller already in use";
        }
      } else if (role == "streamer") {
        if (streamerClientId == 0 || streamerClientId == clientId) {
          streamerClientId = clientId;
          client->role = "streamer";
          roleAccepted = true;
        } else {
          response["type"] = "role_rejected";
          response["role"] = "streamer";
          response["reason"] = "Streamer already active";
        }
      } else if (role == "viewer") {
        if (getViewerCount() < MAX_VIEWERS) {
          client->role = "viewer";
          roleAccepted = true;
        } else {
          response["type"] = "role_rejected";
          response["role"] = "viewer";
          response["reason"] = "Maximum viewers reached";
        }
      }

      if (roleAccepted) {
        response["type"] = "role_accepted";
        response["role"] = role;
        Serial.printf("Client %u registered as %s - ACCEPTED\n", clientId, role.c_str());
      } else {
        Serial.printf("Client %u registration REJECTED\n", clientId);
      }

      String output;
      serializeJson(response, output);
      Serial.printf("Sending response to client %u: %s\n", clientId, output.c_str());
      AsyncWebSocketClient *wsClient = ws.client(clientId);
      if (wsClient) {
        wsClient->text(output);
        Serial.printf("Response sent successfully\n");
      } else {
        Serial.printf("ERROR: Could not find WebSocket client %u\n", clientId);
      }

      // Broadcast status update to all clients
      ws.textAll(getConnectionStatus());
      return;
    }

    // Handle motor commands (only from controller)
    TankBotClient *sender = getClientById(clientId);
    if (sender && sender->role == "controller") {
      if (type == "motor") {
        String direction = doc["direction"];

        if (direction == "forward") moveForward();
        else if (direction == "backward") moveBackward();
        else if (direction == "left") turnLeft();
        else if (direction == "right") turnRight();
        else if (direction == "stop") stopMotors();
      }
      else if (type == "joystick") {
        float leftPower = doc["left"];
        float rightPower = doc["right"];

        int leftSpeed = abs(leftPower * currentSpeed);
        int rightSpeed = abs(rightPower * currentSpeed);

        if (motorTrim < 0) {
          leftSpeed = constrain(leftSpeed + (motorTrim * abs(leftPower)), 0, 255);
        } else if (motorTrim > 0) {
          rightSpeed = constrain(rightSpeed - (motorTrim * abs(rightPower)), 0, 255);
        }

        if (leftPower >= 0) {
          digitalWrite(IN1, HIGH);
          digitalWrite(IN2, LOW);
        } else {
          digitalWrite(IN1, LOW);
          digitalWrite(IN2, HIGH);
        }

        if (rightPower >= 0) {
          digitalWrite(IN3, LOW);
          digitalWrite(IN4, HIGH);
        } else {
          digitalWrite(IN3, HIGH);
          digitalWrite(IN4, LOW);
        }

        ledcWrite(PWM_CHANNEL_A, leftSpeed);
        ledcWrite(PWM_CHANNEL_B, rightSpeed);
      }
      else if (type == "speed") {
        int speedLevel = doc["value"];
        switch(speedLevel) {
          case 1: currentSpeed = SPEED_SLOW; break;
          case 2: currentSpeed = SPEED_MEDIUM; break;
          case 3: currentSpeed = SPEED_FAST; break;
        }
      }
      else if (type == "trim") {
        motorTrim = doc["value"];
        motorTrim = constrain(motorTrim, -20, 20);

        preferences.begin("tankbot", false);
        preferences.putInt("trim", motorTrim);
        preferences.end();

        Serial.printf("Trim saved: %d\n", motorTrim);
      }
    }

    // Handle video frames from streamer - relay to controller and viewers
    if (sender && sender->role == "streamer" && type == "video") {
      String frame = doc["frame"];

      // Always send to controller (full framerate)
      if (controllerClientId != 0) {
        AsyncWebSocketClient *controllerWs = ws.client(controllerClientId);
        if (controllerWs && controllerWs->status() == WS_CONNECTED) {
          String controllerMsg;
          serializeJson(doc, controllerMsg);
          controllerWs->text(controllerMsg);
        }
      }

      // Send to viewers with frame skipping (every other frame)
      frameCounter++;
      if (frameCounter % 2 == 0) {
        for (auto &client : clients) {
          if (client.role == "viewer") {
            AsyncWebSocketClient *viewerWs = ws.client(client.id);
            if (viewerWs && viewerWs->status() == WS_CONNECTED) {
              String viewerMsg;
              serializeJson(doc, viewerMsg);
              viewerWs->text(viewerMsg);
            }
          }
        }
      }
      return;
    }

    // Handle sensor data from streamer - relay to all viewers and controller
    if (sender && sender->role == "streamer" && type == "sensor") {
      // Send to controller
      if (controllerClientId != 0) {
        AsyncWebSocketClient *controllerWs = ws.client(controllerClientId);
        if (controllerWs && controllerWs->status() == WS_CONNECTED) {
          String sensorMsg;
          serializeJson(doc, sensorMsg);
          controllerWs->text(sensorMsg);
        }
      }

      // Send to all viewers
      for (auto &client : clients) {
        if (client.role == "viewer") {
          AsyncWebSocketClient *viewerWs = ws.client(client.id);
          if (viewerWs && viewerWs->status() == WS_CONNECTED) {
            String sensorMsg;
            serializeJson(doc, sensorMsg);
            viewerWs->text(sensorMsg);
          }
        }
      }
      return;
    }
  }
}

// WebSocket event handler
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());

      // Check connection limit
      if (ws.count() > MAX_TOTAL_CLIENTS) {
        Serial.printf("Connection limit reached, rejecting client #%u\n", client->id());
        StaticJsonDocument<128> response;
        response["type"] = "error";
        response["message"] = "Connection limit reached";
        String output;
        serializeJson(response, output);
        client->text(output);
        client->close();
        return;
      }

      // Add client with no role initially
      {
        TankBotClient newClient;
        newClient.id = client->id();
        newClient.role = "none";
        clients.push_back(newClient);
      }

      // Send current status
      client->text(getConnectionStatus());
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());

      // Remove client and free up their role
      {
        TankBotClient *disconnectedClient = getClientById(client->id());
        if (disconnectedClient) {
          if (disconnectedClient->role == "controller") {
            controllerClientId = 0;
            Serial.println("Controller disconnected");
          } else if (disconnectedClient->role == "streamer") {
            streamerClientId = 0;
            Serial.println("Streamer disconnected");
          }

          // Remove from vector
          clients.erase(
            std::remove_if(clients.begin(), clients.end(),
              [client](const TankBotClient& c) { return c.id == client->id(); }),
            clients.end()
          );
        }
      }

      // Broadcast updated status
      ws.textAll(getConnectionStatus());
      break;

    case WS_EVT_DATA:
      handleWebSocketMessage(client, arg, data, len);
      break;

    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

// HTTPS server handler functions
void handleRoot(HTTPRequest *req, HTTPResponse *res) {
  res->setHeader("Content-Type", "text/html");
  res->print(LANDING_page);
}

void handleFromFile(HTTPRequest *req, HTTPResponse *res, const char *filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) {
    res->setStatusCode(404);
    res->print("File not found");
    return;
  }

  res->setHeader("Content-Type", "text/html");
  while (file.available()) {
    res->write(file.read());
  }
  file.close();
}

void handleBasic(HTTPRequest *req, HTTPResponse *res) {
  handleFromFile(req, res, "/basic.html");
}

void handleEnhanced(HTTPRequest *req, HTTPResponse *res) {
  handleFromFile(req, res, "/enhanced.html");
}

void handleView(HTTPRequest *req, HTTPResponse *res) {
  handleFromFile(req, res, "/view.html");
}

void handleStreamSource(HTTPRequest *req, HTTPResponse *res) {
  handleFromFile(req, res, "/stream-source.html");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n=== TankBot Starting ===");

  // Load saved trim value
  preferences.begin("tankbot", true);
  motorTrim = preferences.getInt("trim", 0);
  preferences.end();
  Serial.print("Loaded trim value: ");
  Serial.println(motorTrim);

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted successfully");
  }

  // Setup motors
  setupMotors();
  Serial.println("Motors initialized");

  // Setup WiFi Access Point
  Serial.print("Setting up Access Point: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password, 1, 0, MAX_TOTAL_CLIENTS + 2);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Setup DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", IP);
  Serial.println("DNS server started for captive portal");

  // Setup mDNS
  if (MDNS.begin("tank")) {
    Serial.println("mDNS responder started");
    Serial.println("You can access at: http://tank.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error setting up mDNS responder!");
  }

  // Setup HTTP server (port 80) for main pages, captive portal, and WebSocket
  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", LANDING_page);
  });

  httpServer.on("/basic", HTTP_GET, [](AsyncWebServerRequest *request){
    File file = LittleFS.open("/basic.html", "r");
    if (!file) {
      request->send(404, "text/plain", "File not found");
      return;
    }
    request->send(LittleFS, "/basic.html", "text/html");
  });

  httpServer.on("/enhanced", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/enhanced.html", "text/html");
  });

  httpServer.on("/view", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/view.html", "text/html");
  });

  httpServer.on("/stream-source", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/stream-source.html", "text/html");
  });

  // Captive portal endpoints - redirect to main page
  httpServer.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });
  httpServer.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });
  httpServer.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });
  httpServer.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });
  httpServer.on("/success.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });
  httpServer.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });
  httpServer.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("http://" + request->host() + "/");
  });

  // WebSocket handler
  ws.onEvent(onWebSocketEvent);
  httpServer.addHandler(&ws);

  httpServer.begin();
  Serial.println("HTTP server started on port 80");

  // Setup PsychicHttp HTTPS server for secure pages and WebSocket (port 443)
  secureServer.setCertificate(SSL_CERT_PEM, SSL_KEY_PEM);

  // Serve stream-source.html page over HTTPS
  secureServer.on("/stream-source", HTTP_GET, [](PsychicRequest *request) {
    File file = LittleFS.open("/stream-source.html", "r");
    if (file) {
      String content = file.readString();
      file.close();
      return request->reply(200, "text/html", content.c_str());
    }
    return request->reply(404, "text/plain", "File not found");
  });

  // Serve enhanced.html page over HTTPS
  secureServer.on("/enhanced", HTTP_GET, [](PsychicRequest *request) {
    File file = LittleFS.open("/enhanced.html", "r");
    if (file) {
      String content = file.readString();
      file.close();
      return request->reply(200, "text/html", content.c_str());
    }
    return request->reply(404, "text/plain", "File not found");
  });

  // Setup secure WebSocket (wss://) handler
  wss.onOpen([](PsychicWebSocketClient *client) {
    Serial.printf("[Secure WS] Client #%u connected\n", client->socket());
  });

  wss.onFrame([](PsychicWebSocketRequest *request, httpd_ws_frame *frame) -> esp_err_t {
    String msg = String((char*)frame->payload).substring(0, frame->len);
    Serial.printf("[Secure WS] Received from #%u: %s\n", request->client()->socket(), msg.c_str());

    // Parse and handle message using existing onWebSocketEvent logic
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, msg);

    if (!error) {
      String msgType = doc["type"].as<String>();

      // Handle registration
      if (msgType == "register") {
        String role = doc["role"].as<String>();
        uint32_t clientId = doc["client_id"];

        if (role == "streamer") {
          if (streamerClient == nullptr) {
            streamerClient = request->client();
            DynamicJsonDocument response(256);
            response["type"] = "role_accepted";
            response["role"] = "streamer";
            String responseStr;
            serializeJson(response, responseStr);
            request->reply(responseStr.c_str());
            Serial.println("[Secure WS] Streamer registered");
          } else {
            DynamicJsonDocument response(256);
            response["type"] = "role_rejected";
            response["role"] = "streamer";
            response["reason"] = "Streamer already connected";
            String responseStr;
            serializeJson(response, responseStr);
            request->reply(responseStr.c_str());
          }
        } else if (role == "controller") {
          if (controllerClient == nullptr) {
            controllerClient = request->client();
            DynamicJsonDocument response(256);
            response["type"] = "role_accepted";
            response["role"] = "controller";
            String responseStr;
            serializeJson(response, responseStr);
            request->reply(responseStr.c_str());
            Serial.println("[Secure WS] Controller registered");
          } else {
            DynamicJsonDocument response(256);
            response["type"] = "role_rejected";
            response["role"] = "controller";
            response["reason"] = "Controller already connected";
            String responseStr;
            serializeJson(response, responseStr);
            request->reply(responseStr.c_str());
          }
        }
      }
      // Handle video frames - forward to controller
      else if (msgType == "video" && request->client() == streamerClient) {
        if (controllerClient != nullptr) {
          controllerClient->sendMessage(msg.c_str());
        }
        // Also forward to HTTP WebSocket viewers
        ws.textAll(msg);
      }
      // Handle sensor data - forward to controller
      else if (msgType == "sensor" && request->client() == streamerClient) {
        if (controllerClient != nullptr) {
          controllerClient->sendMessage(msg.c_str());
        }
        // Also forward to HTTP WebSocket viewers
        ws.textAll(msg);
      }
      // Handle motor commands from controller
      else if (msgType == "motor" && request->client() == controllerClient) {
        String direction = doc["direction"].as<String>();

        // Execute motor commands directly
        if (direction == "forward") moveForward();
        else if (direction == "backward") moveBackward();
        else if (direction == "left") turnLeft();
        else if (direction == "right") turnRight();
        else if (direction == "stop") stopMotors();
      }
      // Handle speed commands from controller
      else if (msgType == "speed" && request->client() == controllerClient) {
        int speedValue = doc["value"];
        currentSpeed = speedValue;
        Serial.printf("Speed set to: %d\n", speedValue);
      }
    }

    return ESP_OK;
  });

  wss.onClose([](PsychicWebSocketClient *client) {
    Serial.printf("[Secure WS] Client #%u disconnected\n", client->socket());

    // Clear client references if they disconnect
    if (client == streamerClient) {
      streamerClient = nullptr;
      Serial.println("[Secure WS] Streamer disconnected");
    }
    if (client == controllerClient) {
      controllerClient = nullptr;
      Serial.println("[Secure WS] Controller disconnected");
    }
  });

  secureServer.attachHandler("/ws", &wss);

  Serial.println("Starting HTTPS server...");
  secureServer.listen(443);
  Serial.println("HTTPS server started on port 443 with secure WebSocket support");

  Serial.println("\n=== TankBot Ready! ===");
  Serial.print("Connect to WiFi: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);
  Serial.println("\nAccess the control panel at:");
  Serial.println("  http://tank.local");
  Serial.print("  http://");
  Serial.println(IP);
  Serial.println("========================\n");
}

void loop() {
  dnsServer.processNextRequest();
  ws.cleanupClients();
}
