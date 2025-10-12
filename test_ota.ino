#include <WiFi.h>
#include <WiFiMulti.h>
#include <Preferences.h>
#include <ThingSpeak.h>
#include <ArduinoOTA.h>
#include <HardwareSerial.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#define RXD2 16
#define TXD2 17

// ======= KONFIGURACJA =======
#define WINDOW_SIZE 100          // liczba pomiarów do uśredniania
#define SEND_INTERVAL 60000      // co ile wysyłać dane (ms) — 1 minuta
#define RESTART_INTERVAL 86400000 // restart po 24h (ms)
#define TO_ZERO_LEVEL 250 // odleglosc do dna   

uint16_t samples[WINDOW_SIZE];
int sampleIndex = 0;
bool bufferFull = false;

unsigned long lastSend = 0;
unsigned long startTime = 0;
// 📡 GLOBALNE zmienne:
String apiKey;
String ha_url;
String ha_token;
int channel;

// ======= ZMIENNE =======
HardwareSerial mySerial(1);

WiFiMulti wifiMulti;
Preferences prefs;
WiFiClient client;
WiFiClientSecure httpsClient;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // zapis
  prefs.begin("config", false);
  // prefs.putString("ha_url", "");
  // prefs.putString("ha_token", "");
  prefs.end();

  // 📡 Odczyt z NVS
  prefs.begin("config", true);
  String ssid1 = prefs.getString("ssid1", "");
  String pass1 = prefs.getString("pass1", "");
  String ssid2 = prefs.getString("ssid2", "");
  String pass2 = prefs.getString("pass2", "");
  ha_url = prefs.getString("ha_url", "");
  ha_token = prefs.getString("ha_token", "");
  apiKey = prefs.getString("ts_api", "");
  channel = prefs.getInt("ts_channel", 0);
  prefs.end();

  // 📶 Dodaj sieci do WiFiMulti
  if (ssid1.length() > 0) wifiMulti.addAP(ssid1.c_str(), pass1.c_str());
  if (ssid2.length() > 0) wifiMulti.addAP(ssid2.c_str(), pass2.c_str());

  Serial.println("📶 Łączenie z jedną z zapisanych sieci...");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ Połączono z Wi-Fi:");
  Serial.println(WiFi.SSID());
  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setPassword("admin");
  ArduinoOTA.begin();

  ThingSpeak.begin(client);
  mySerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  httpsClient.setInsecure();

  startTime = millis();
}

void loop() {
  wifiMulti.run();
  ArduinoOTA.handle();

  // 1️⃣ Odczyt z czujnika (ciągły)
  while (mySerial.available() >= 4) {
    uint8_t buf[4];
    mySerial.readBytes(buf, 4);
    if (buf[0] == 0xFF) {
      uint16_t distance = (buf[1] << 8) | buf[2];
      uint8_t checksum = (buf[0] + buf[1] + buf[2]) & 0xFF;
      if (checksum == buf[3]) {
        samples[sampleIndex] = distance;
        sampleIndex++;
        if (sampleIndex >= WINDOW_SIZE) {
          sampleIndex = 0;
          bufferFull = true;
        }
      }
    } else {
      mySerial.read(); // zły bajt
    }
  }

  // 2️⃣ Co 1 minutę: licz średnią i wysyłaj
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    int count = bufferFull ? WINDOW_SIZE : sampleIndex;
    if (count > 0) {
      uint32_t sum = 0;
      for (int i = 0; i < count; i++) sum += samples[i];
      float avg_cm = TO_ZERO_LEVEL - ((sum / (float)count) / 10.0);  // mm → cm
      Serial.println(avg_cm);
      // send to thingspeak
      ThingSpeak.setField(1, avg_cm);
      int code = ThingSpeak.writeFields(channel, apiKey.c_str());
      if (code == 200) {
        Serial.println("✅ Dane wysłane do ThingSpeak!");
      } else {
        Serial.printf("❌ Błąd: %d\n", code);
      }

      //send to home assistant
      if (avg_cm) {

        HTTPClient https;
        https.begin(httpsClient, ha_url); 
        https.addHeader("Content-Type", "application/json");
        https.addHeader("Authorization", String("Bearer ") + ha_token);

        String payload = "{\"state\": \"" + String(avg_cm) + "\", \"attributes\": {\"unit_of_measurement\": \"cm\"}}";

        int httpCode = https.POST(payload);

        if (httpCode > 0) {
          Serial.printf("📤 Wysłano do Home Assistant! Odpowiedź: %d\n", httpCode);
        } else {
          Serial.printf("❌ Błąd wysyłania: %s\n", https.errorToString(httpCode).c_str());
        }

        https.end();
      }
    }
  }

  // 3️⃣ Automatyczny restart co 24h
  if (millis() - startTime >= RESTART_INTERVAL) {
    ESP.restart();
  }

  delay(5);
}