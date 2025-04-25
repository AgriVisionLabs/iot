#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoWebsockets.h>
#include <LittleFS.h>
#define RESET_BUTTON_PIN 0
#define LED_PIN 2

using namespace websockets;

// declar and assign buzzer pin
const int buzzerPin = 13;

// declare and assign the websocket url
const char* websocketUrl = "wss://agrivision.tryasp.net/IrrigationDeviceWS";

// declare a websocket client
WebsocketsClient client;

// declare and assign the provisioning key
const String provisioningKey = "IRRIGATE-X7Q4-PUMP-KEY9-Z1B3L";

// webserver setup
Preferences prefs;
WebServer server(80); // listen on port 80 for http requests 
const char* apSSID = "IrrigationUnit";
const char* apPassword = "password";

String pendingSSID = "";
String pendingPassword = "";
bool hasAttempted = false;
bool wasSuccessful = false;

// helper function for loading the certificate
String loadCertificate() {
  File file = LittleFS.open("/cert.txt", "r");
  if (!file || file.isDirectory()) {
    Serial.println("❌ Failed to open cert.txt");
    return "";
  }

  String cert = file.readString();
  file.close();
  
  return cert;
}

// helper functions
void handleRoot() {
  if (!LittleFS.exists("/index.html")) {
    server.send(500, "text/plain", "index.html missing.");
    return;
  }

  File file = LittleFS.open("/index.html", "r");
  String dynamicHtml = file.readString();
  file.close();

  String statusDotClass = "disconnected";
  String networkText = "Not connected to any network";

  if (WiFi.status() == WL_CONNECTED) {
    statusDotClass = "connected";
    networkText = WiFi.SSID();
  }

  dynamicHtml.replace("{{STATUS_DOT_CLASS}}", statusDotClass);
  dynamicHtml.replace("{{NETWORK_NAME}}", networkText);

  server.send(200, "text/html", dynamicHtml);
}

void handleConnect() {
  hasAttempted = false;
  wasSuccessful = false;

  WiFi.disconnect(true);
  delay(150);

  pendingSSID = server.arg("ssid");
  pendingPassword = server.arg("password");

  prefs.begin("wifi", false);
  prefs.putString("ssid", pendingSSID);
  prefs.putString("password", pendingPassword);
  prefs.end();

  Serial.println("🧹 Resetting WiFi state and attempting new connection...");

  WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str()); // async method 

  server.send(200, "text/plain", "OK");
}

// WebSockets
void onWebSocketMessage(WebsocketsMessage message) {
  String msg = message.data();
  Serial.println("📩 WebSocket Message: " + msg);
  if (msg == "buzz"){
    Serial.println("🎵 Buzz command received");
    digitalWrite(buzzerPin, HIGH);
    delay(500);
    digitalWrite(buzzerPin, LOW);
  }
}

void onWebSocketEvent(WebsocketsEvent event, String data) {
  Serial.print("📡 WS Event - "); Serial.println(data);

  if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("🛑 WebSocket connection closed");
  }
}

void connectToWebSocket() {
  client.onMessage(onWebSocketMessage);
  client.onEvent(onWebSocketEvent);

  String cert = loadCertificate();
  if (cert.length() == 0) {
    Serial.println("❌ Certificate is empty or missing. Connection aborted.");
    return;
  }

  client.setCACert(cert.c_str());

  Serial.println("🌐 Connecting to backend WebSocket...");

  if (client.connect(websocketUrl)) {
    Serial.println("✅ WebSocket connected");

    String mac = WiFi.macAddress();
    String ip = WiFi.localIP().toString();

    String jsonMsg = "{";
    jsonMsg += "\"type\":\"connect\",";
    jsonMsg += "\"mac\":\"" + mac + "\",";
    jsonMsg += "\"ip\":\"" + ip + "\",";
    jsonMsg += "\"provisioningKey\":\"" + provisioningKey + "\"";
    jsonMsg += "}";

    client.send(jsonMsg);
  } else {
    Serial.println("❌ Failed to connect to WebSocket");
  }
}

void handleStatus() {
  if (!hasAttempted) {
    Serial.println("🔌 Polling WiFi connection status...");

    int tries = 0;
    
    while (WiFi.status() != WL_CONNECTED && tries < 15) {
      delay(500);
      Serial.print(".");
      tries++;
    }
    Serial.println();

    hasAttempted = true;
    wasSuccessful = (WiFi.status() == WL_CONNECTED);

    if (wasSuccessful) {
      // log connection
      Serial.println("✅ Connected to WiFi!");
      Serial.print("📡 Device IP: ");
      Serial.println(WiFi.localIP());

      // buzz when connected
      pinMode(buzzerPin, OUTPUT);
      digitalWrite(buzzerPin, HIGH);
      delay(500);
      digitalWrite(buzzerPin, LOW);

      server.send(200, "text/plain", "SUCCESS:" + WiFi.localIP().toString());
      delay(4000);

      // switch to STA-only mode 
      if (WiFi.getMode() == WIFI_AP_STA || WiFi.getMode() == WIFI_AP) {
        Serial.println("🔄 Disabling AP mode...");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        delay(3000);
      }

      connectToWebSocket();
    } else {
      Serial.println("❌ Failed to connect to WiFi.");
      server.send(200, "text/plain", "FAIL");
    }
  }
}

void checkResetButton() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    unsigned long start = millis();
    
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
      // blink slowly while the button is pressed
      digitalWrite(LED_PIN, (millis() / 300) % 2);

      if (millis() - start > 3000) {
        Serial.println("🔁 Long press detected — resetting WiFi config!");

        // flash the LED rapidally to signal wipe
        for (int i = 0; i < 10; i++) {
          digitalWrite(LED_PIN, HIGH);
          delay(100);
          digitalWrite(LED_PIN, LOW);
          delay(100);
        }

        // clear wifi ram + flash
        WiFi.disconnect(true, true);
        delay(100);

        // clear manually saved prefs
        prefs.begin("wifi", false);
        prefs.clear();
        prefs.end();

        delay(500);
        ESP.restart();
      }
      // reset LED state when button is released
      digitalWrite(LED_PIN, LOW);
    }
  }
}

void trySavedCredentials() {
  prefs.begin("wifi", true);
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("password", "");
  prefs.end();

  if (savedSSID.length() > 0) {
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    Serial.println("🔁 Trying saved WiFi credentials...");

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 15) {
      delay(500);
      Serial.print(".");
      tries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ Reconnected to WiFi!");

      // buzz
      digitalWrite(buzzerPin, HIGH);
      delay(500);
      digitalWrite(buzzerPin, LOW);

      connectToWebSocket();

      return;      
    } else {
      Serial.println("❌ Failed to connect. Starting AP mode...");
    }
  }

  // start ap mode if failed to reconnect or no saved credentials
  WiFi.softAP(apSSID, apPassword);
  Serial.println("📡 Access Point started: " + String(apSSID));
  Serial.println("🌐 Open browser at: http://" + WiFi.softAPIP().toString());
}

void handleDisconnect() {
  WiFi.disconnect(true);
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  server.send(200, "text/plain", "Disconnected");
}

void handleNotFound() {
  if (LittleFS.exists("/404.html")) {
    File file = LittleFS.open("/404.html", "r");
    String errorPage = file.readString();
    file.close();
    
    server.send(404, "text/html", errorPage);
  } else {
    server.send(404, "text/plain", "404 - Not found.");
  }
}

void setup() {
  Serial.begin(115200);

  // check if littlefs is mounted
  if (!LittleFS.begin()) {
    Serial.println("❌ Failed to mount LittleFS");
    return;
  }

  // setup pin mode for led pin to output
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // setup pin mode for buzzer to output
  pinMode(buzzerPin, OUTPUT);

  // connect using stored credentials if available
  trySavedCredentials();

  // handle requests
  server.on("/", handleRoot);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", handleStatus);
  server.on("/disconnect", HTTP_POST, handleDisconnect);
  server.onNotFound(handleNotFound);

  // load logo.png
  server.serveStatic("/logo.png", LittleFS, "/logo.png");

  // start the server
  server.begin();
}

void loop() {
  checkResetButton();
  
  server.handleClient();

  if (client.available()) {
    client.poll(); // keep socket alive
  }
}