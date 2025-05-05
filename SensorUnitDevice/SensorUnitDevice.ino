#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoWebsockets.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "DHT.h"
#define RESET_BUTTON_PIN 0
#define LED_PIN 2
#define DHTPIN 15  
#define DHTTYPE DHT11


using namespace websockets;

// declar and assign buzzer pin
const int buzzerPin = 14;

DHT dht(DHTPIN, DHTTYPE);

// declare and assign the websocket url
const char* websocketUrl = "wss://agrivision.tryasp.net/SensorDeviceWs";

// declare a websocket client
WebsocketsClient client;

// declare and assign the provisioning key
const String provisioningKey = "SENSOR-X7Q4-MOIST-KEY9-Z3C4U";

// webserver setup
Preferences prefs;
WebServer server(80); // listen on port 80 for http requests 
const char* apSSID = "SENSOR-ESP32-01";
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

// WebSocket stuff
void onWebSocketMessage(WebsocketsMessage message) {
  String msg = message.data();
  Serial.println("📩 WebSocket Message: " + msg);

  // Prepare JSON doc (adjust size if needed later)
  StaticJsonDocument<200> doc;

  DeserializationError error = deserializeJson(doc, msg);
  if (error) {
    Serial.print("❌ Failed to parse WebSocket JSON: ");
    Serial.println(error.c_str());
    return;
  }

  // Extract fields
  String type = doc["type"] | "";
  String command = doc["command"] | "";
  String cid = doc["cid"] | "";
  bool toState = doc["toState"] | false;

  if (type == "ping") {
    Serial.println("🏓 Ping received — sending Pong...");

    String pongMsg = "{";
    pongMsg += "\"type\":\"pong\"";
    pongMsg += "}";

    client.send(pongMsg);
  }
}

void onWebSocketEvent(WebsocketsEvent event, String data) {
  Serial.print("📡 WS Event - "); Serial.println(data);

  if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("🛑 WebSocket connection closed");
    analogWrite(LED_PIN, 0);
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
    analogWrite(LED_PIN, 50);

    String mac = WiFi.macAddress();
    String ip = WiFi.localIP().toString();

    String jsonMsg = "{";
    jsonMsg += "\"type\":\"connect\",";
    jsonMsg += "\"macAddress\":\"" + mac + "\",";
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
    digitalWrite(LED_PIN, 0);
    unsigned long start = millis();
    
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
      // blink slowly while the button is pressed
      digitalWrite(LED_PIN, 0);
      digitalWrite(LED_PIN, (millis() / 300) % 2);

      if (millis() - start > 3000) {
        Serial.println("🔁 Long press detected — resetting WiFi config!");

        // flash the LED rapidally to signal wipe
        for (int i = 0; i < 10; i++) {
          digitalWrite(LED_PIN, 50);
          delay(100);
          digitalWrite(LED_PIN, 0);
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
      digitalWrite(LED_PIN, 0);
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
  server.send(200, "text/plain", "DISCONNECTED");
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

unsigned long lastReadingTime = 0;
const unsigned long readingInterval = 1000 * 10;

void SendReadings() {
  int analogValue = analogRead(35);
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  String mac = WiFi.macAddress();
  
  String jsonMsg = "{";
  jsonMsg += "\"type\":\"readings\",";
  jsonMsg += "\"macAddress\":\"" + mac + "\",";
  jsonMsg += "\"provisioningKey\":\"" + provisioningKey + "\",";
  jsonMsg += "\"readings\":{";
  jsonMsg += "\"moisture\":\"" + String(analogValue) + "\",";
  jsonMsg += "\"temperature\":\"" + String(temperature, 1) + "\",";
  jsonMsg += "\"humidity\":\"" + String(humidity, 1) + "\"";
  jsonMsg += "}";
  jsonMsg += "}";

  client.send(jsonMsg);

  Serial.printf("Sent readings:\nMoisture: %d\nTemp: %.2f°C\nHumidity: %.2f%%\n", analogValue, temperature, humidity);
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

  prefs.begin("state", true);
  bool isOn = prefs.getBool("isOn", false); // false as default
  prefs.end();

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

  dht.begin();
}

void loop() {
  checkResetButton();
  
  server.handleClient();


  if (client.available()) {
    client.poll(); // keep socket alive
  }

  if (millis() - lastReadingTime >= readingInterval) {
    SendReadings();
    lastReadingTime = millis(); // reset the timer
  }
}
