#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <WebsocketHandler.hpp>
#include <WebsocketNode.hpp>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <map>
#include "cert_der.h"
#include "key_der.h"

using namespace httpsserver;

// Motor control pins
#define IN1 16
#define IN2 17
#define IN3 18
#define IN4 19
#define ENA 25
#define ENB 26

// PWM settings
#define PWM_FREQ 1000
#define PWM_RESOLUTION 8
#define PWM_CHANNEL_A 0
#define PWM_CHANNEL_B 1

// Speed levels
#define SPEED_SLOW 160
#define SPEED_MEDIUM 220
#define SPEED_FAST 255

// Chunk size constant for reference
#define CHUNK_SIZE 2800

// WiFi credentials
const char* ssid = "TankBot";
const char* password = "tankbot2025";

// Servers
AsyncWebServer httpServer(80);
AsyncWebSocket ws("/ws");
SSLCert cert = SSLCert(
  cert_der, cert_der_len,
  key_der, key_der_len
);
// Reduce max connections from 4 to 3 to save memory
// (1 streamer + 1 controller + 1 viewer/spare)
HTTPSServer secureServer = HTTPSServer(&cert, 443, 3);

// DNS server for captive portal
DNSServer dnsServer;
const byte DNS_PORT = 53;

// Preferences
Preferences preferences;

// Motor state
int currentSpeed = SPEED_MEDIUM;
int motorTrim = 0;

// Client tracking
struct TankClient {
  uint32_t id;
  std::string role;  // "controller", "streamer", "viewer"
};
std::vector<TankClient> clients;

// WebSocket handler tracking
std::map<WebsocketHandler*, TankClient> wssClients;

// Forward declarations for motor functions
void stopMotors();
void moveForward();
void moveBackward();
void turnLeft();
void turnRight();

// Custom WebSocket handler for HTTPS server
class TankWebSocketHandler : public WebsocketHandler {
public:
  static WebsocketHandler* create() {
    return new TankWebSocketHandler();
  }

  TankWebSocketHandler() : WebsocketHandler() {
    // Register this handler
    wssClients[this] = {0, "none"};
    Serial.printf("[WSS] New handler created: %p\n", this);
  }

  ~TankWebSocketHandler() {
    wssClients.erase(this);
    Serial.printf("[WSS] Handler destroyed: %p\n", this);
  }

  void onMessage(WebsocketInputStreambuf* inbuf) override {
    // Allocate buffer on heap to avoid stack overflow
    uint8_t* buffer = new uint8_t[3072];
    if (!buffer) {
      Serial.println("[WSS] Memory allocation failed!");
      return;
    }

    size_t len = 0;
    int byte;
    while ((byte = inbuf->sbumpc()) != EOF && len < 3072) {
      buffer[len++] = (uint8_t)byte;
    }

    if (len == 0) {
      delete[] buffer;
      return;
    }

    // Only log significant events to reduce serial overhead
    if (len < 100) {  // Text messages only
      Serial.printf("[WSS] Received %d bytes\n", len);
    }

    // Check if this is a binary frame (video chunk) or text (JSON command)
    if (len >= 2) {
      uint8_t chunkIndex = buffer[0];
      uint8_t totalChunks = buffer[1];

      // If it looks like a video chunk header
      if (totalChunks > 0 && totalChunks < 100 && chunkIndex < totalChunks) {
        handleBinaryFrame(buffer, len);
        return;
      }
    }

    // Otherwise treat as text/JSON
    std::string message((char*)buffer, len);
    handleTextMessage(message);

    delete[] buffer;  // Free heap memory
  }

  void handleBinaryFrame(uint8_t* data, size_t len) {
    if (len < 2) return;

    uint8_t chunkIndex = data[0];
    uint8_t totalChunks = data[1];

    // Only streamer should send binary frames
    if (wssClients[this].role != "streamer") {
      return;  // Silent reject to reduce serial spam
    }

    // Log only first chunk to reduce serial overhead
    if (chunkIndex == 0) {
      Serial.printf("[WSS] Frame: %d chunks -> ", totalChunks);
    }

    // Direct relay: broadcast chunk immediately to all controllers/viewers
    // No reassembly needed - just forward the raw chunk data
    int relayCount = 0;
    for (auto& pair : wssClients) {
      if (pair.first != this && (pair.second.role == "controller" || pair.second.role == "viewer")) {
        // Check if client is ready before sending (basic flow control)
        try {
          pair.first->send(data, len, WebsocketHandler::SEND_TYPE_BINARY);
          relayCount++;
        } catch (...) {
          // Silently skip failed sends to prevent blocking
        }
      }
    }

    // Log completion on last chunk
    if (chunkIndex == totalChunks - 1) {
      Serial.printf("%d clients\n", relayCount);
    }
  }

  void handleTextMessage(const std::string& msg) {
    Serial.printf("[WSS] Text message: %s\n", msg.c_str());

    // Simple JSON parsing for role registration
    if (msg.find("\"type\":\"register\"") != std::string::npos) {
      std::string role = "none";

      if (msg.find("\"role\":\"streamer\"") != std::string::npos) role = "streamer";
      else if (msg.find("\"role\":\"controller\"") != std::string::npos) role = "controller";
      else if (msg.find("\"role\":\"viewer\"") != std::string::npos) role = "viewer";

      if (role != "none") {
        // Check if role is available
        bool available = true;
        if (role == "streamer" || role == "controller") {
          for (auto& pair : wssClients) {
            if (pair.first != this && pair.second.role == role) {
              available = false;
              break;
            }
          }
        }

        if (available) {
          wssClients[this].role = role;
          std::string response = "{\"type\":\"role_accepted\",\"role\":\"" + role + "\"}";
          send((uint8_t*)response.data(), response.length(), WebsocketHandler::SEND_TYPE_TEXT);
          Serial.printf("[WSS] Role assigned: %s\n", role.c_str());
        } else {
          std::string response = "{\"type\":\"role_rejected\",\"reason\":\"" + role + " already in use\"}";
          send((uint8_t*)response.data(), response.length(), WebsocketHandler::SEND_TYPE_TEXT);
          Serial.printf("[WSS] Role rejected: %s already in use\n", role.c_str());
        }
      }
    }
    // Handle motor commands if from controller
    else if (wssClients[this].role == "controller") {
      if (msg.find("\"type\":\"motor\"") != std::string::npos) {
        if (msg.find("\"direction\":\"forward\"") != std::string::npos) moveForward();
        else if (msg.find("\"direction\":\"backward\"") != std::string::npos) moveBackward();
        else if (msg.find("\"direction\":\"left\"") != std::string::npos) turnLeft();
        else if (msg.find("\"direction\":\"right\"") != std::string::npos) turnRight();
        else if (msg.find("\"direction\":\"stop\"") != std::string::npos) stopMotors();
      }
      else if (msg.find("\"type\":\"speed\"") != std::string::npos) {
        if (msg.find("\"value\":1") != std::string::npos) currentSpeed = SPEED_SLOW;
        else if (msg.find("\"value\":2") != std::string::npos) currentSpeed = SPEED_MEDIUM;
        else if (msg.find("\"value\":3") != std::string::npos) currentSpeed = SPEED_FAST;
      }
    }
  }

  void onClose() override {
    Serial.printf("[WSS] Client disconnected: %p\n", this);
  }

  void onError(std::string error) override {
    Serial.printf("[WSS] Error: %s\n", error.c_str());
  }
};

// Function declarations
void stopMotors();
void moveForward();
void moveBackward();
void turnLeft();
void turnRight();
void handleWebSocketMessage(AsyncWebSocketClient *client, uint8_t *data, size_t len, bool isBinary);
void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
TankClient* getClientById(uint32_t id);
void serveHTMLFile(HTTPRequest *req, HTTPResponse *res, const char* filename);

// Motor control functions
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
  if (motorTrim < 0) leftSpeed = currentSpeed + motorTrim;
  else if (motorTrim > 0) rightSpeed = currentSpeed - motorTrim;
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
  if (motorTrim < 0) leftSpeed = currentSpeed + motorTrim;
  else if (motorTrim > 0) rightSpeed = currentSpeed - motorTrim;
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

// Client management
TankClient* getClientById(uint32_t id) {
  for (auto &client : clients) {
    if (client.id == id) return &client;
  }
  return nullptr;
}

// WebSocket binary frame handler
void handleWebSocketMessage(AsyncWebSocketClient *client, uint8_t *data, size_t len, bool isBinary) {
  uint32_t clientId = client->id();
  TankClient* sender = getClientById(clientId);

  if (!sender) {
    Serial.printf("Unknown client %u\n", clientId);
    return;
  }

  // Handle binary frames (video chunks)
  if (isBinary && len >= 2) {
    uint8_t chunkIndex = data[0];
    uint8_t totalChunks = data[1];

    // Only process video from streamer
    if (sender->role != "streamer") {
      Serial.println("[WS Binary] Rejected: not from streamer");
      return;
    }

    // Log only first chunk to reduce serial overhead
    if (chunkIndex == 0) {
      Serial.printf("[WS] Frame: %d chunks -> ", totalChunks);
    }

    // Direct relay: broadcast chunk immediately to all controllers/viewers
    int relayCount = 0;
    for (auto &c : clients) {
      if (c.role == "controller" || c.role == "viewer") {
        AsyncWebSocketClient *recipient = ws.client(c.id);
        if (recipient && recipient->status() == WS_CONNECTED) {
          // Send the chunk exactly as received (including the 2-byte header)
          recipient->binary(data, len);
          relayCount++;
        }
      }
    }

    // Log completion on last chunk
    if (chunkIndex == totalChunks - 1) {
      Serial.printf("%d clients\n", relayCount);
    }
    return;
  }

  // Handle text frames (JSON commands)
  if (!isBinary) {
    String msg = String((char*)data).substring(0, len);
    Serial.printf("[WS Text] Client %u (%s): %s\n", clientId, sender->role.c_str(), msg.c_str());

    // Parse JSON (simple manual parsing for specific commands)
    if (msg.indexOf("\"type\":\"register\"") >= 0) {
      std::string role = "";
      if (msg.indexOf("\"role\":\"streamer\"") >= 0) role = "streamer";
      else if (msg.indexOf("\"role\":\"controller\"") >= 0) role = "controller";
      else if (msg.indexOf("\"role\":\"viewer\"") >= 0) role = "viewer";

      if (role != "") {
        // Check if role is available
        bool roleAvailable = true;
        if (role == "streamer" || role == "controller") {
          for (auto &c : clients) {
            if (c.role == role && c.id != clientId) {
              roleAvailable = false;
              break;
            }
          }
        }

        if (roleAvailable) {
          sender->role = role;
          std::string response = "{\"type\":\"role_accepted\",\"role\":\"" + role + "\"}";
          client->text(response.c_str());
          Serial.printf("  ✓ Role assigned: %s\n", role.c_str());
        } else {
          std::string response = "{\"type\":\"role_rejected\",\"reason\":\"" + role + " already in use\"}";
          client->text(response.c_str());
          Serial.printf("  ✗ Role rejected: %s already in use\n", role.c_str());
        }
      }
    }
    else if (sender->role == "controller") {
      // Handle motor commands
      if (msg.indexOf("\"type\":\"motor\"") >= 0) {
        if (msg.indexOf("\"direction\":\"forward\"") >= 0) moveForward();
        else if (msg.indexOf("\"direction\":\"backward\"") >= 0) moveBackward();
        else if (msg.indexOf("\"direction\":\"left\"") >= 0) turnLeft();
        else if (msg.indexOf("\"direction\":\"right\"") >= 0) turnRight();
        else if (msg.indexOf("\"direction\":\"stop\"") >= 0) stopMotors();
      }
      else if (msg.indexOf("\"type\":\"speed\"") >= 0) {
        if (msg.indexOf("\"value\":1") >= 0) currentSpeed = SPEED_SLOW;
        else if (msg.indexOf("\"value\":2") >= 0) currentSpeed = SPEED_MEDIUM;
        else if (msg.indexOf("\"value\":3") >= 0) currentSpeed = SPEED_FAST;
      }
    }
  }
}

void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                          void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u connected\n", client->id());
    TankClient newClient = {client->id(), "none"};
    clients.push_back(newClient);
  }
  else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
    uint32_t disconnectedId = client->id();
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                  [disconnectedId](const TankClient& c) { return c.id == disconnectedId; }),
                  clients.end());
  }
  else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len) {
      handleWebSocketMessage(client, data, len, info->opcode == WS_BINARY);
    }
  }
}

// HTTPS server handlers
void serveHTMLFile(HTTPRequest *req, HTTPResponse *res, const char* filename) {
  Serial.printf("Serving file: %s\n", filename);

  File file = LittleFS.open(filename, "r");
  if (!file) {
    Serial.printf("File not found: %s\n", filename);
    res->setStatusCode(404);
    res->setStatusText("Not Found");
    res->setHeader("Content-Type", "text/plain");
    res->print("File not found");
    return;
  }

  res->setStatusCode(200);
  res->setStatusText("OK");
  res->setHeader("Content-Type", "text/html; charset=utf-8");

  // Read and send file in chunks
  uint8_t buffer[512];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    res->write(buffer, bytesRead);
  }
  file.close();
  Serial.println("File served successfully");
}

void handleLandingPage(HTTPRequest *req, HTTPResponse *res) {
  serveHTMLFile(req, res, "/landing.html");
}

void handleStreamSource(HTTPRequest *req, HTTPResponse *res) {
  serveHTMLFile(req, res, "/stream-source.html");
}

void handleEnhanced(HTTPRequest *req, HTTPResponse *res) {
  serveHTMLFile(req, res, "/enhanced.html");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== TankBot Binary Streaming v2 ===");

  // Load trim
  preferences.begin("tankbot", true);
  motorTrim = preferences.getInt("trim", 0);
  preferences.end();
  Serial.printf("Motor trim: %d\n", motorTrim);

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
  } else {
    Serial.println("LittleFS mounted");
  }

  // Setup motors
  setupMotors();
  Serial.println("Motors initialized");

  // Setup WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP IP: %s\n", IP.toString().c_str());

  // DNS for captive portal
  dnsServer.start(DNS_PORT, "*", IP);
  Serial.println("DNS server started");

  // mDNS
  if (MDNS.begin("tank")) {
    Serial.println("mDNS: http://tank.local");
  }

  // HTTP Server (port 80) - WebSocket only
  ws.onEvent(handleWebSocketEvent);
  httpServer.addHandler(&ws);

  // Serve landing page on HTTP (redirect to HTTPS)
  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", R"(
<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><title>TankBot</title></head>
<body style="font-family: Arial; text-align: center; padding: 50px;">
  <h1>TankBot Control Center</h1>
  <p>Please use HTTPS to access camera features:</p>
  <a href="https://192.168.4.1" style="font-size: 1.5em;">https://192.168.4.1</a>
  <p style="margin-top: 30px; color: #666;">You will need to accept the security certificate.</p>
</body>
</html>
    )");
  });

  // Captive portal redirects
  httpServer.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *req){ req->redirect("/"); });
  httpServer.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *req){ req->redirect("/"); });

  httpServer.begin();
  Serial.println("HTTP server started on port 80");

  // HTTPS Server (port 443) - HTML pages for camera access
  ResourceNode *nodeRoot = new ResourceNode("/", "GET", &handleLandingPage);
  ResourceNode *nodeStream = new ResourceNode("/stream-source", "GET", &handleStreamSource);
  ResourceNode *nodeEnhanced = new ResourceNode("/enhanced", "GET", &handleEnhanced);

  // WebSocket node for secure WebSocket connections
  WebsocketNode *nodeWss = new WebsocketNode("/ws", &TankWebSocketHandler::create);

  secureServer.registerNode(nodeRoot);
  secureServer.registerNode(nodeStream);
  secureServer.registerNode(nodeEnhanced);
  secureServer.registerNode(nodeWss);

  Serial.println("Starting HTTPS server...");
  uint8_t startResult = secureServer.start();
  if (startResult == 1) {
    Serial.println("✓ HTTPS server started successfully on port 443");
  } else {
    Serial.println("✗ HTTPS server FAILED to start!");
    Serial.println("  Check error messages above for details.");
  }

  Serial.println("\n=== TankBot Ready! ===");
  Serial.printf("Connect to WiFi: %s / %s\n", ssid, password);
  Serial.printf("Then visit: https://%s\n", IP.toString().c_str());
  Serial.println("========================\n");
}

void loop() {
  dnsServer.processNextRequest();
  secureServer.loop();
  ws.cleanupClients();
  delay(1);
}
