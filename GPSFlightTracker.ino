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
unsigned long loggingInterval = 200;

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
  
  // 1. Start GPS at default speed to send configuration commands
  ss.begin(9600);
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(REC_SWITCH, INPUT_PULLUP);
  pinMode(WIFI_SWITCH, INPUT_PULLUP);

  // 2. Set GPS to 5Hz Update Rate (UBX-CFG-RATE)
  // This tells the chip to measure position every 200ms
  byte set5Hz[] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0xC8, 0x00, 0x01, 0x00, 0x01, 0x00, 0xDE, 0x6A};
  ss.write(set5Hz, sizeof(set5Hz));
  delay(100);

  // 3. Optional but Recommended: Increase Baud Rate to 38400 (UBX-CFG-PRT)
  // 9600 can be too slow to transmit all NMEA sentences 5 times per second.
  byte set38400[] = {0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00, 0x00, 0x96, 0x00, 0x00, 0x07, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x92, 0x8D};
  ss.write(set38400, sizeof(set38400));
  delay(100);
  
  // Restart SoftwareSerial at the new speed
  ss.begin(38400);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed");
    return;
  }

  if (!bmp.begin(0x76)) { // Check your I2C address, usually 0x76 or 0x77
    Serial.println("Could not find a valid BMP280 sensor!");
  }

  // BMP280 settings for better altitude resolution during flight
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     
                  Adafruit_BMP280::SAMPLING_X2,     // Temp. oversampling
                  Adafruit_BMP280::SAMPLING_X16,    // Pressure oversampling
                  Adafruit_BMP280::FILTER_X16,      // Filtering
                  Adafruit_BMP280::STANDBY_MS_500); 

  Serial.println("Setup Complete. Waiting for GPS Fix...");
}

void loop() {
  while (ss.available() > 0)
    gps.encode(ss.read());

  bool recSw = digitalRead(REC_SWITCH) == LOW;
  bool wifiSw = digitalRead(WIFI_SWITCH) == LOW;
  bool hasFix = gps.location.isValid() && gps.location.age() < 2000;

  if (wifiActive) {
    digitalWrite(LED_PIN, (millis() / 200) % 2); 
  } else if (isLogging) {
    digitalWrite(LED_PIN, (millis() / 1000) % 2);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  if (recSw && !isLogging && hasFix) {
    char path[25];
    sprintf(path, "/%02d%02d_%02d%02d.kml", gps.date.day(), gps.date.month(), gps.time.hour(), gps.time.minute());
    logFile = LittleFS.open(path, "w");
    if (logFile) {
      logFile.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?><kml xmlns=\"http://www.opengis.net\"><Document><Placemark><LineString><altitudeMode>relativeToGround</altitudeMode><coordinates>");
      altBaseline = bmp.readAltitude(1013.25);
      isLogging = true;
      Serial.println("Recording...");
    }
  }

  // ONLY CHANGE IS HERE: Added gps.location.isUpdated()
  if (isLogging && millis() - lastLogTime >= loggingInterval) {
    if (gps.location.isUpdated()) { 
      lastLogTime = millis();
      float relAlt = bmp.readAltitude(1013.25) - altBaseline;
      Serial.printf("Lat: %.6f Lng: %.6f Alt: %.1fm\n", gps.location.lat(), gps.location.lng(), relAlt);
      if (logFile) {
        logFile.printf("%.6f,%.6f,%.1f ", gps.location.lng(), gps.location.lat(), relAlt);
      }
    }
  }

  if (!recSw && isLogging) {
    if (logFile) {
      logFile.println("\n</coordinates></LineString></Placemark></Document></kml>");
      logFile.close();
    }
    isLogging = false;
    Serial.println("Recording Stopped.");
  }

  if (wifiSw && !wifiActive) {
    WiFi.softAP(accessPointName, password);
    server.on("/", handleRoot);
    server.on("/download", handleDownload);
    server.on("/delete", handleDelete);
    server.begin();
    wifiActive = true;
    Serial.println("WiFi Active: 192.168.4.1");
  }

  if (wifiActive) {
    server.handleClient();
  }
}
