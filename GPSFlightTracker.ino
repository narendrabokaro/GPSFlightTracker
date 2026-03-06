#include <LittleFS.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// BMP280 Pins
#define SDA_PIN 4       // D2
#define SCL_PIN 5       // D1

// GPS Pins
#define GPS_TX_PIN 12   // D6
#define GPS_RX_PIN 13   // D7

#define LED_PIN 14      // D5
#define REC_SWITCH 0    // D3 (Log Switch)
#define WIFI_SWITCH 2   // D4 (WiFi Switch)

TinyGPSPlus gps;
SoftwareSerial ss(GPS_TX_PIN, GPS_RX_PIN);
Adafruit_BMP280 bmp;
ESP8266WebServer server(80);

File logFile;
bool isLogging = false;
bool wifiActive = false;
float altBaseline = 0;
unsigned long lastLogTime = 0;
// AP configuration [WiFi On: 192.168.4.1]
char accessPointName[] = "RC_FLIGHT_DATA";
char password[] = "12345678";

// In milliseconds
unsigned long debugMessagePrintFreq = 5000;
unsigned long loggingInterval = 500;

// --- WEB SERVER FUNCTIONS ---
void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;padding:20px;} table{width:100%;border-collapse:collapse;} ";
  html += "td,th{padding:10px;border-bottom:1px solid #ddd;} .del{color:red;}</style></head>";
  html += "<body><h1>Flight Logs</h1><table><tr><th>Filename</th><th>Action</th></tr>";

  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    if (dir.fileName().endsWith(".kml")) {
      html += "<tr><td>" + dir.fileName() + "</td><td>";
      html += "<a href='/get?file=" + dir.fileName() + "'>Download</a> | ";
      html += "<a href='/del?file=" + dir.fileName() + "' class='del'>Delete</a></td></tr>";
    }
  }
  html += "</table><br><a href='/'>Refresh</a></body></html>";
  server.send(200, "text/html", html);
}

void handleDownload() {
  String path = server.arg("file");
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    server.streamFile(f, "application/vnd.google-earth.kml+xml");
    f.close();
  }
}

void handleDelete() {
  String path = server.arg("file");
  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  ss.begin(9600);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(REC_SWITCH, INPUT_PULLUP);
  pinMode(WIFI_SWITCH, INPUT_PULLUP);

  if (!LittleFS.begin()) Serial.println("FS Error");
  if (!bmp.begin(0x76)) Serial.println("BMP Error");

  WiFi.mode(WIFI_OFF);
}

void loop() {
  // Check if we are receiving ANY bytes from the GPS
  while (ss.available() > 0) {
    char c = ss.read();
    gps.encode(c);
  }

  // Debugging message - Print status every 5 seconds
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > debugMessagePrintFreq) {
    lastDebug = millis();
    Serial.println("--- GPS Debug Status ---");
    Serial.print("Chars Processed: ");
    Serial.println(gps.charsProcessed());
    Serial.print("Satellites in View: ");
    Serial.println(gps.satellites.value());

    if (gps.charsProcessed() < 10) {
      Serial.println(">> ERROR: No data from GPS. Check TX/RX wiring!");
    } else if (gps.satellites.value() == 0) {
      Serial.println(">> SEARCHING: GPS is talking, but no satellites found. Go outside!");
    }
  }

  bool recSw = (digitalRead(REC_SWITCH) == LOW);
  bool wifiSw = (digitalRead(WIFI_SWITCH) == LOW);
  bool hasFix = (gps.location.isValid() && gps.satellites.value() >= 4);

  // WiFi Logic
  if (wifiSw && !wifiActive) {
    WiFi.softAP(accessPointName, password);
    server.on("/", handleRoot);
    server.on("/get", handleDownload);
    server.on("/del", handleDelete);
    server.begin();
    wifiActive = true;
    Serial.println("WiFi On: 192.168.4.1");
  } else if (!wifiSw && wifiActive) {
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
  }

  if (wifiActive) {
    server.handleClient();
  }

  // LED Logic
  if (!hasFix) {
    // Fast Flash - Searching for GPS fix
    digitalWrite(LED_PIN, (millis() / 200) % 2);
  } else if (isLogging) {
    // Slow Blink - Recording in progress
    digitalWrite(LED_PIN, (millis() / 1000) % 2);
  } else {
    // Solid Ready - GPS Fixed
    digitalWrite(LED_PIN, HIGH);
  }

  // Record Logic
  if (recSw && !isLogging && hasFix) {
    char path[25];
    sprintf(path, "/%02d%02d_%02d%02d.kml", gps.date.day(), gps.date.month(), gps.time.hour(), gps.time.minute());
    logFile = LittleFS.open(path, "w");
    if (logFile) {
      logFile.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?><kml xmlns=\"http://www.opengis.net\"><Document><Placemark><LineString><altitudeMode>relativeToGround</altitudeMode><coordinates>");
      // Sea level pressure in Austin, TX (in hectopascals hPa)
      altBaseline = bmp.readAltitude(1013.25);
      isLogging = true;
      Serial.println("Recording...");
    }
  }

  // Logging in progress
  if (isLogging && millis() - lastLogTime >= loggingInterval) {
    lastLogTime = millis();
    float relAlt = bmp.readAltitude(1013.25) - altBaseline;
    Serial.printf("Lat: %.6f Lng: %.6f Alt: %.1fm\n", gps.location.lat(), gps.location.lng(), relAlt);
    logFile.printf("%.6f,%.6f,%.1f\n", gps.location.lng(), gps.location.lat(), relAlt);
    logFile.flush();
  }

  // Record button off - Save the recorded file
  if (!recSw && isLogging) {
    logFile.println("</coordinates></LineString></Placemark></Document></kml>");
    logFile.close();
    isLogging = false;
    Serial.println("Saved.");
  }
}
