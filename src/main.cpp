#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
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
// Minimal HTTP server ONLY for captive portal redirects (no WebSocket, no heavy lifting)
AsyncWebServer httpServer(80);
SSLCert cert = SSLCert(
  cert_der, cert_der_len,
  key_der, key_der_len
);
// Reduced slots for faster cleanup: 2 WebSockets + 2 for page loads
// Fewer slots = faster connection cleanup cycle = better performance
HTTPSServer secureServer = HTTPSServer(&cert, 443, 4);

// DNS server for captive portal
DNSServer dnsServer;
const byte DNS_PORT = 53;

// Preferences
Preferences preferences;

// Motor state
int currentSpeed = SPEED_MEDIUM;
int motorTrim = 0;

// Client tracking structure - must be declared before use
struct TankClient {
  uint32_t id;
  std::string role;  // "controller", "streamer", "viewer"
};

// WebSocket handler tracking (HTTPS only - no HTTP WebSocket)
std::map<WebsocketHandler*, TankClient> wssClients;

// Shared buffer for WebSocket messages with mutex protection
SemaphoreHandle_t wsBufferMutex;
uint8_t wsMessageBuffer[3072];

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
    // Peek at first byte to determine message type without mutex
    int firstByte = inbuf->sgetc();
    if (firstByte == EOF) return;

    // Check if this is likely JSON text (starts with '{')
    // Video chunks start with chunk index (0-99) followed by total chunks
    bool isTextMessage = (firstByte == '{');

    // For text messages (registration, commands), use a local buffer to avoid mutex contention
    if (isTextMessage) {
      char localBuffer[512];  // Enough for JSON commands
      size_t len = 0;
      int byte;
      while ((byte = inbuf->sbumpc()) != EOF && len < sizeof(localBuffer) - 1) {
        localBuffer[len++] = (uint8_t)byte;
      }
      localBuffer[len] = '\0';

      if (len > 0) {
        std::string message(localBuffer, len);
        handleTextMessage(message);
      }
      return;
    }

    // For binary messages (video chunks), use mutex-protected shared buffer
    // Use longer timeout to ensure critical messages don't fail
    if (xSemaphoreTake(wsBufferMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
      Serial.println("[WSS] Buffer busy, dropping video chunk");
      // Drain the input buffer
      while (inbuf->sbumpc() != EOF) {}
      return;
    }

    size_t len = 0;
    int byte;
    while ((byte = inbuf->sbumpc()) != EOF && len < 3072) {
      wsMessageBuffer[len++] = (uint8_t)byte;
    }

    if (len >= 2) {
      uint8_t chunkIndex = wsMessageBuffer[0];
      uint8_t totalChunks = wsMessageBuffer[1];

      // If it looks like a video chunk header
      if (totalChunks > 0 && totalChunks < 100 && chunkIndex < totalChunks) {
        handleBinaryFrame(wsMessageBuffer, len);
      }
    }

    xSemaphoreGive(wsBufferMutex);
  }

  void handleBinaryFrame(uint8_t* data, size_t len) {
    if (len < 2) return;

    // Only streamer should send binary frames
    if (wssClients[this].role != "streamer") return;

    // Direct relay: broadcast chunk immediately to all controllers/viewers
    // No reassembly, no logging - pure pass-through for maximum performance
    for (auto& pair : wssClients) {
      if (pair.first != this && (pair.second.role == "controller" || pair.second.role == "viewer")) {
        try {
          pair.first->send(data, len, WebsocketHandler::SEND_TYPE_BINARY);
        } catch (...) {
          // Silently skip failed sends to prevent blocking
        }
      }
    }
  }

  void handleTextMessage(const std::string& msg) {
    // Handle sensor data from streamer - relay silently (no logging to reduce overhead)
    if (wssClients[this].role == "streamer" && msg.find("\"type\":\"sensor\"") != std::string::npos) {
      for (auto& pair : wssClients) {
        if (pair.first != this && (pair.second.role == "controller" || pair.second.role == "viewer")) {
          try {
            pair.first->send((uint8_t*)msg.data(), msg.length(), WebsocketHandler::SEND_TYPE_TEXT);
          } catch (...) {}
        }
      }
      return;
    }

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

          // Send current trim value to controllers
          if (role == "controller") {
            char trimMsg[64];
            snprintf(trimMsg, sizeof(trimMsg), "{\"type\":\"trim_value\",\"value\":%d}", motorTrim);
            send((uint8_t*)trimMsg, strlen(trimMsg), WebsocketHandler::SEND_TYPE_TEXT);
          }
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
      else if (msg.find("\"type\":\"trim\"") != std::string::npos) {
        // Parse trim value (-20 to +20)
        size_t valPos = msg.find("\"value\":");
        if (valPos != std::string::npos) {
          int trimValue = atoi(msg.c_str() + valPos + 8);
          motorTrim = constrain(trimValue, -20, 20);
          // Save to preferences
          preferences.begin("tankbot", false);
          preferences.putInt("trim", motorTrim);
          preferences.end();
          Serial.printf("[WSS] Trim updated: %d\n", motorTrim);
        }
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
  res->setHeader("Connection", "close");  // Close connection after serving to free up slot

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

void handleView(HTTPRequest *req, HTTPResponse *res) {
  serveHTMLFile(req, res, "/view.html");
}

void handleBasic(HTTPRequest *req, HTTPResponse *res) {
  serveHTMLFile(req, res, "/basic.html");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== TankBot Binary Streaming v2 ===");

  // Create mutex for shared WebSocket buffer
  wsBufferMutex = xSemaphoreCreateMutex();
  if (!wsBufferMutex) {
    Serial.println("Failed to create mutex!");
  }

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
    Serial.println("mDNS: https://tank.local");
  }

  // HTTP Server (port 80) - ONLY captive portal redirects (lightweight)
  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("https://192.168.4.1/");
  });
  httpServer.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("https://192.168.4.1/");
  });
  httpServer.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("https://192.168.4.1/");
  });
  httpServer.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("https://192.168.4.1/");
  });
  httpServer.begin();
  Serial.println("HTTP redirect server started on port 80");

  // HTTPS Server (port 443) - All content
  ResourceNode *nodeRoot = new ResourceNode("/", "GET", &handleLandingPage);
  ResourceNode *nodeStream = new ResourceNode("/stream-source", "GET", &handleStreamSource);
  ResourceNode *nodeEnhanced = new ResourceNode("/enhanced", "GET", &handleEnhanced);
  ResourceNode *nodeView = new ResourceNode("/view", "GET", &handleView);
  ResourceNode *nodeBasic = new ResourceNode("/basic", "GET", &handleBasic);

  // WebSocket node for secure WebSocket connections
  WebsocketNode *nodeWss = new WebsocketNode("/ws", &TankWebSocketHandler::create);

  secureServer.registerNode(nodeRoot);
  secureServer.registerNode(nodeStream);
  secureServer.registerNode(nodeEnhanced);
  secureServer.registerNode(nodeView);
  secureServer.registerNode(nodeBasic);
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
  delay(1);
}
