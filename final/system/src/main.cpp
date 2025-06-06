#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <ArduinoJson.h>

// Pin Definitions for ESP32
#define ECHO_PIN 25  // Changed from D2
#define TRIG_PIN 4   // Changed from D1 (moved from 26 since 26 is used for DFPlayer)
#define LED_RED 14   // Changed from D5
#define LED_YELLOW 12  // Changed from D6
#define LED_GREEN 13   // Changed from D7

// DFPlayer pins - Using Hardware Serial2 on ESP32
#define MP3_RX 27  // ESP32 RX pin - connect to TX of DFPlayer
#define MP3_TX 26  // ESP32 TX pin - connect to RX of DFPlayer

// Sound file definitions
#define STARTUP_SOUND 1  // File 001.mp3 on the SD card - System ready
#define WIFI_FAIL_SOUND 2  // File 002.mp3 on the SD card - WiFi connection failed
#define WIFI_SUCCESS_SOUND 3  // File 003.mp3 on the SD card - WiFi connected successfully
#define WARNING_SOUND 4  // File 004.mp3 on the SD card
#define DANGER_SOUND 5   // File 005.mp3 on the SD card

const long interval = 1000; // Interval for distance measurements
bool sensorActive = false;
bool dfPlayerSleep = false; // New variable to track DFPlayer sleep state
const int MAXIMUM_HEIGHT_CM = 183; // Maximum height in cm (6 feet = 183 cm)

WebServer server(80); // Web server instance using ESP32's WebServer
HardwareSerial dfPlayerSerial(2); // Using Hardware Serial2 for ESP32 (UART2)
DFRobotDFPlayerMini player; // Changed variable name to match your working example

enum Zone { SAFE, WARNING, DANGER };
Zone currentZone = SAFE; // Keep track of current zone to avoid repeated sound playback

// Function declarations
void handleIndex();
void handleDistance();
void handleAdmin();
void handleToggleSensor();
void handleSensorStatus();
void handleToggleSleepMode(); // New function to toggle sleep mode
void handleSleepStatus(); // New function to get sleep status
int readDistance(); // Changed to return int for cm
Zone determineZone(int distanceCm);
void handleZone(Zone zone);
void printDetail(uint8_t type, int value);

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  
  // Initialize all LEDs to OFF
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_GREEN, LOW);

  // Initialize DFPlayer with Hardware Serial - MODIFIED TO MATCH WORKING EXAMPLE
  dfPlayerSerial.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX);
  Serial.println("Initializing DFPlayer...");
  
  if (!player.begin(dfPlayerSerial)) {
    Serial.println("Unable to begin DFPlayer Mini!");
    Serial.println("Please check the connection and SD card.");
    // Consider adding a delay or retry mechanism here instead of halting
    delay(3000);
  } else {
    Serial.println("DFPlayer Mini online.");
    player.setTimeOut(500); // Set serial communication timeout to 500ms
    player.volume(30);      // Set volume (0-30) - increased to match your working example
    player.EQ(DFPLAYER_EQ_NORMAL);
    player.outputDevice(DFPLAYER_DEVICE_SD);
  }

  // Initialize SPIFFS - ESP32 version
  if (!SPIFFS.begin(true)) {  // true parameter formats SPIFFS if needed
    Serial.println("Failed to mount file system");
    return;
  }

  // WiFi Manager setup - ESP32 compatible
  WiFiManager wifiManager;
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println("Entered AP mode, waiting for Wi-Fi configuration...");
        // Play WiFi failure sound
    delay(1000);
    player.play(WIFI_FAIL_SOUND); // Play 002.mp3
    Serial.println("Playing WiFi failure sound");
    delay(2000); // Give time for the sound to start playing  
  });

  if (!wifiManager.autoConnect("ESP32_FloodMonitor")) {
    Serial.println("Failed to connect and hit timeout");
    // Play WiFi failure sound
    player.play(WIFI_FAIL_SOUND); // Play 002.mp3
    Serial.println("Playing WiFi failure sound");
    delay(2000); // Give time for the sound to start playing  
  }

  Serial.println("Connected to Wi-Fi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  // Play WiFi success sound
  player.play(WIFI_SUCCESS_SOUND); // Play 003.mp3
  Serial.println("Playing WiFi success sound");
  delay(1500); // Give time for the sound to play

  // MDNS setup
  if (!MDNS.begin("flood-monitoring")) {
    Serial.println("Error starting mDNS");
  } else {
    Serial.println("mDNS started");
    MDNS.addService("http", "tcp", 80); // Advertise HTTP service
  }

  // Server setup
  server.on("/", handleIndex);
  server.on("/distance", handleDistance);
  server.on("/admin.html", handleAdmin);
  server.on("/toggle_sensor", handleToggleSensor);
  server.on("/sensor_status", handleSensorStatus); // Endpoint to check sensor status
  server.on("/toggle_sleep", handleToggleSleepMode); // New endpoint to toggle sleep mode
  server.on("/sleep_status", handleSleepStatus); // New endpoint to check sleep status
  
  // Handle static files from SPIFFS
  server.serveStatic("/", SPIFFS, "/");
  
  // Add a not found handler
  server.onNotFound([]() {
    Serial.print("Request not found: ");
    Serial.println(server.uri());
    
    // Check if the file exists in SPIFFS
    String path = server.uri();
    if (path.endsWith("/")) path += "index.html";
    
    if (SPIFFS.exists(path)) {
      File file = SPIFFS.open(path, "r");
      if (file) {
        String contentType = "text/plain";
        if (path.endsWith(".html")) contentType = "text/html";
        else if (path.endsWith(".css")) contentType = "text/css";
        else if (path.endsWith(".js")) contentType = "application/javascript";
        else if (path.endsWith(".png")) contentType = "image/png";
        else if (path.endsWith(".jpg")) contentType = "image/jpeg";
        else if (path.endsWith(".ico")) contentType = "image/x-icon";
        else if (path.endsWith(".json")) contentType = "application/json";
        
        server.streamFile(file, contentType);
        file.close();
        return;
      }
    }
    
    server.send(404, "text/plain", "File Not Found");
  });
  
  server.begin();
  Serial.println("HTTP server started");
  
  // Play startup sound to indicate system is fully ready
  delay(1000); // Short delay to ensure everything is initialized
  player.play(STARTUP_SOUND); // Play 001.mp3
  Serial.println("Playing startup sound - System ready");
}

void loop() {
  static unsigned long previousMillis = 0;
  unsigned long currentMillis = millis();

  server.handleClient();
  
  // Handle DFPlayer notifications and keep alive
  if (player.available()) {
    printDetail(player.readType(), player.read());
  }

  // Only measure distance when the sensor is active and DFPlayer is not in sleep mode
  if (sensorActive && !dfPlayerSleep && (currentMillis - previousMillis >= interval)) {
    previousMillis = currentMillis;
    int distanceCm = readDistance();
    
    // Print distance information for debugging
    Serial.print("Distance: ");
    Serial.print(distanceCm);
    Serial.println(" cm");
    
    // Process the zone and update LEDs
    Zone zone = determineZone(distanceCm);
    handleZone(zone);
  }
}

int readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  int duration = pulseIn(ECHO_PIN, HIGH);
  // Calculate distance in cm using speed of sound (343 m/s or 0.0343 cm/µs)
  // Time is round-trip, so divide by 2
  int distanceCm = duration * 0.0343 / 2;
  
  return distanceCm;
}

Zone determineZone(int distanceCm) {
  // Values converted from inches to cm
  // 60 inches = ~152 cm
  // 36 inches = ~91 cm
  if (distanceCm <= 5) return DANGER;    // Less than 5cm is danger
  if (distanceCm <= 17) return WARNING;   // Less than 17cm is warning
  return SAFE;                            // Greater than 90cm is safe
}

void handleZone(Zone zone) {
  // Only act if zone has changed AND sensor is active AND DFPlayer is not sleeping
  if (zone != currentZone && sensorActive && !dfPlayerSleep) {
    currentZone = zone;
    
    // Play sounds immediately without cooldown checks
    if (zone == WARNING) {
      player.play(WARNING_SOUND); // 004.mp3
      Serial.println("Playing warning sound");
    } else if (zone == DANGER) {
      player.play(DANGER_SOUND); // 005.mp3
      Serial.println("Playing danger sound");
    }
  } else if (zone != currentZone && (!sensorActive || dfPlayerSleep)) {
    // Just update the current zone without playing sounds when sensor is inactive or DFPlayer is sleeping
    currentZone = zone;
    Serial.println("Zone changed but sensor is inactive or DFPlayer is sleeping - no sound played.");
  }
  
  // Set LEDs based on zone (only if sensor is active)
  if (sensorActive) {
    switch (zone) {
      case SAFE:
        digitalWrite(LED_GREEN, HIGH);
        digitalWrite(LED_YELLOW, LOW);
        digitalWrite(LED_RED, LOW);
        break;
      case WARNING:
        digitalWrite(LED_GREEN, LOW);
        digitalWrite(LED_YELLOW, HIGH);
        digitalWrite(LED_RED, LOW);
        break;
      case DANGER:
        digitalWrite(LED_GREEN, LOW);
        digitalWrite(LED_YELLOW, LOW);
        digitalWrite(LED_RED, HIGH);
        break;
    }
  }
}

void handleIndex() {
  File file = SPIFFS.open("/index.html", "r");
  if (!file) {
    server.send(404, "text/plain", "File Not Found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleDistance() {
  if (!sensorActive) {
    // Return an inactive sensor response
    server.send(200, "application/json", "{\"error\": \"Sensor is off\", \"status\": false}");
    return;
  }
  
  // Only read sensor data when active
  int distanceCm = readDistance();
  
  Zone zone = determineZone(distanceCm);

  String ledStatus = (zone == SAFE) ? "green" : (zone == WARNING) ? "yellow" : "red";
  String message = "";
  
  // Add alert messages based on zone
  if (zone == WARNING) {
    message = "Warning: Object detected in warning zone.";
  } else if (zone == DANGER) {
    message = "Danger: Object too close!";
  }
  
  String jsonResponse = String("{\"distance\":") + String(distanceCm) +
                        String(", \"unit\": \"cm\"") +
                        String(", \"led\": \"") + ledStatus + 
                        String("\", \"message\": \"") + message + 
                        String("\", \"status\": true}");
  server.send(200, "application/json", jsonResponse);
}

void handleAdmin() {
  File file = SPIFFS.open("/admin.html", "r");
  if (!file) {
    server.send(404, "text/plain", "File Not Found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleToggleSensor() {
  sensorActive = !sensorActive; // Toggle sensor state
  
  // Turn off all LEDs when sensor is deactivated
  if (!sensorActive) {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_GREEN, LOW);
    currentZone = SAFE; // Reset zone when sensor is turned off
  }
  
  String status = sensorActive ? "Sensor activated" : "Sensor deactivated";
  server.send(200, "text/plain", status);
}

// New handler to toggle DFPlayer sleep mode
void handleToggleSleepMode() {
  dfPlayerSleep = !dfPlayerSleep; // Toggle sleep state
  
  if (dfPlayerSleep) {
    // Enter sleep mode
    player.sleep();
    Serial.println("DFPlayer entered sleep mode");
  } else {
    // Wake up from sleep mode
    player.reset();  // Reset player to wake it up
    delay(100);      // Short delay to allow reset
    // Restore previous settings
    player.volume(30);
    player.EQ(DFPLAYER_EQ_NORMAL);
    player.outputDevice(DFPLAYER_DEVICE_SD);
    Serial.println("DFPlayer woke up from sleep mode");
  }
  
  String status = dfPlayerSleep ? "DFPlayer sleeping" : "DFPlayer awake";
  server.send(200, "text/plain", status);
}

// Handler to get the current sleep status
void handleSleepStatus() {
  String jsonResponse = String("{\"sleeping\":") + (dfPlayerSleep ? "true" : "false") + String("}");
  server.send(200, "application/json", jsonResponse);
}

// Handler to get the current sensor status
void handleSensorStatus() {
  String jsonResponse = String("{\"status\":") + (sensorActive ? "true" : "false") + String("}");
  server.send(200, "application/json", jsonResponse);
}

// Implementation of the missing printDetail function
void printDetail(uint8_t type, int value) {
  switch (type) {
    case TimeOut:
      Serial.println(F("Time Out!"));
      break;
    case WrongStack:
      Serial.println(F("Stack Wrong!"));
      break;
    case DFPlayerCardInserted:
      Serial.println(F("Card Inserted!"));
      break;
    case DFPlayerCardRemoved:
      Serial.println(F("Card Removed!"));
      break;
    case DFPlayerCardOnline:
      Serial.println(F("Card Online!"));
      break;
    case DFPlayerUSBInserted:
      Serial.println("USB Inserted!");
      break;
    case DFPlayerUSBRemoved:
      Serial.println("USB Removed!");
      break;
    case DFPlayerPlayFinished:
      Serial.print(F("Number:"));
      Serial.print(value);
      Serial.println(F(" Play Finished!"));
      break;
    case DFPlayerError:
      Serial.print(F("DFPlayerError:"));
      switch (value) {
        case Busy:
          Serial.println(F("Card not found"));
          break;
        case Sleeping:
          Serial.println(F("Sleeping"));
          break;
        case SerialWrongStack:
          Serial.println(F("Get Wrong Stack"));
          break;
        case CheckSumNotMatch:
          Serial.println(F("Check Sum Not Match"));
          break;
        case FileIndexOut:
          Serial.println(F("File Index Out of Bound"));
          break;
        case FileMismatch:
          Serial.println(F("Cannot Find File"));
          break;
        case Advertise:
          Serial.println(F("In Advertise"));
          break;
        default:
          Serial.println(F("Unknown error"));
          break;
      }
      break;
    default:
      break;
  }
}