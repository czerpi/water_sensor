// python espota.py -i 192.168.1.34 -p 3232 -f ../Documents/Arduino/test_ota/build/esp32.esp32.esp32da/test_ota.ino.bin -a admin
#include <stdlib.h>  // potrzebne do qsort
#include <WiFi.h>
#include <WiFiMulti.h>
#include <EEPROM.h>
#include <ThingSpeak.h>
#include <ArduinoOTA.h>
#include <HardwareSerial.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#define EEPROM_SIZE 1024

#define SSID1_ADDR 0
#define PASS1_ADDR 64
#define SSID2_ADDR 128
#define PASS2_ADDR 192
#define HA_URL_ADDR 256
#define HA_TOKEN_ADDR 512
#define API_KEY_ADDR 768
#define CHANNEL_ADDR 960

#define SHORT_FIELD_SIZE 64
#define LONG_FIELD_SIZE 256

// EEPROM helper functions
String readStringFromEEPROM(int addr, int maxLen) {
  String result;
  result.reserve(maxLen);
  for (int i = 0; i < maxLen; i++) {
    uint8_t value = EEPROM.read(addr + i);
    if (value == 0 || value == 0xFF) {
      break;
    }
    result += (char)value;
  }
  return result;
}

void saveStringToEEPROM(int addr, const String& data, int maxLen) {
  int len = data.length();
  for (int i = 0; i < maxLen; i++) {
    if (i < len) {
      EEPROM.write(addr + i, data[i]);
    } else {
      EEPROM.write(addr + i, 0);
    }
  }
  EEPROM.commit();
}



#define RXD2 16
#define TXD2 17

// Definicja pinu trybu pracy (MODE_PIN)
#define MODE_PIN 4  // Możesz zmienić numer GPIO na pasujący

// ======= KONFIGURACJA =======
#define MIN_SAMPLES 3  // Minimalna liczba próbek, żeby liczyć i wysyłać
#define SEND_INTERVAL 60000      // co ile wysyłać dane (ms) — 1 minuta
#define RESTART_INTERVAL 86400000 // restart po 24h (ms)
#define WIFI_CONNECT_TIMEOUT 30000 // maksymalny czas laczenia z Wi-Fi (ms)
#define TO_ZERO_LEVEL 237 // odleglosc do dna   
#define REQUIRED_SAMPLES 5
#define SENSOR_MIN_MM 300
#define SENSOR_MAX_MM 2300

unsigned long lastSend = 0;
unsigned long startTime = 0;
// 📡 GLOBALNE zmienne:
String apiKey;
String ha_url;
String ha_token;
unsigned long channel;

uint16_t validMeasurements[REQUIRED_SAMPLES];
int collected = 0;

HardwareSerial mySerial(1);
WiFiMulti wifiMulti;


WiFiClient client;
WiFiClientSecure httpsClient;
HTTPClient https;

bool readDistanceFrame(uint16_t &distance) {
  static uint8_t frame[4];
  static int pos = 0;

  while (mySerial.available() > 0) {
    uint8_t byteRead = mySerial.read();

    if (pos == 0 && byteRead != 0xFF) {
      continue;
    }

    frame[pos++] = byteRead;

    if (pos == 4) {
      pos = 0;
      uint8_t checksum = (frame[0] + frame[1] + frame[2]) & 0xFF;
      if (checksum == frame[3]) {
        distance = (frame[1] << 8) | frame[2];
        return true;
      }
    }
  }

  return false;
}

float calculateMedianCm() {
  uint16_t sorted[REQUIRED_SAMPLES];
  for (int i = 0; i < collected; i++) sorted[i] = validMeasurements[i];

  auto cmp_uint16 = [](const void* a, const void* b) -> int {
    uint16_t aa = *(const uint16_t*)a, bb = *(const uint16_t*)b;
    return (aa > bb) - (aa < bb);
  };
  qsort(sorted, collected, sizeof(uint16_t), cmp_uint16);

  float median_mm;
  if (collected % 2 == 0) {
    median_mm = (sorted[collected / 2 - 1] + sorted[collected / 2]) / 2.0;
  } else {
    median_mm = sorted[collected / 2];
  }

  return TO_ZERO_LEVEL - (median_mm / 10.0);  // mm -> cm
}

void sendMeasurements(float median_cm) {
  Serial.println(median_cm);

  // send to thingspeak
  ThingSpeak.setField(1, median_cm);
  ThingSpeak.setField(2, WiFi.RSSI());
  ThingSpeak.setField(3, (long)ESP.getFreeHeap());
  ThingSpeak.setField(4, (long)(millis() / 1000));  // uptime w sekundach
  int code = ThingSpeak.writeFields(channel, apiKey.c_str());
  if (code == 200) {
    Serial.println("✅ Dane wysłane do ThingSpeak!");
  } else {
    Serial.printf("❌ Błąd: %d\n", code);
  }

  //send to home assistant
  bool started = https.begin(httpsClient, ha_url);
  if (started) {
    https.setTimeout(5000);  // ⏱️ ustawienie timeoutu (5s)
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", String("Bearer ") + ha_token);

    String payload = "{\"state\": \"" + String(median_cm) + "\", \"attributes\": {\"unit_of_measurement\": \"cm\"}}";

    int httpCode = https.POST(payload);

    if (httpCode > 0) {
      Serial.printf("📤 Wysłano do Home Assistant! Odpowiedź: %d\n", httpCode);
    } else {
      Serial.printf("❌ Błąd wysyłania: %s\n", https.errorToString(httpCode).c_str());
    }

    https.end();
  } else {
    Serial.println("❌ Nie udało się zainicjować połączenia HTTPS z Home Assistant.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Ustawienie pinu trybu pracy — HIGH = tryb przetworzony, LOW = real-time
  pinMode(MODE_PIN, OUTPUT);
  digitalWrite(MODE_PIN, HIGH); // HIGH = tryb przetworzony

  // EEPROM initialization for both ESP32 and ESP8266
  EEPROM.begin(EEPROM_SIZE);
  // // ⚠️ Only run once to save your config
  // saveStringToEEPROM(SSID1_ADDR, "Elion_1B11", SHORT_FIELD_SIZE);
  // saveStringToEEPROM(PASS1_ADDR, "25808A0DE0866", SHORT_FIELD_SIZE);
  // saveStringToEEPROM(SSID2_ADDR, "12345678", SHORT_FIELD_SIZE);
  // saveStringToEEPROM(PASS2_ADDR, "B0mb0w012345678", SHORT_FIELD_SIZE);
  // saveStringToEEPROM(HA_URL_ADDR, "https://6bb29091fb4a625ddf4eb8296097bdc1.czerpak.pl/api/states/sensor.water_level", LONG_FIELD_SIZE);
  // saveStringToEEPROM(HA_TOKEN_ADDR, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmMTY4MjBkYzUyZTk0NWUwYjFhYjIyNjE0MWY4ZmQ1OSIsImlhdCI6MTc2MDI1NDU5NiwiZXhwIjoyMDc1NjE0NTk2fQ.0YmItXQNccBBpeWlfqmJomy4X5FGnZYP7YIOtxGq-Qg", LONG_FIELD_SIZE);
  // saveStringToEEPROM(API_KEY_ADDR, "A4Y84M4MKLTNPXWI", SHORT_FIELD_SIZE);
  // channel = 3108182;
  // EEPROM.put(CHANNEL_ADDR, channel);
  // EEPROM.commit();

  // Serial.println("✅ Saved EEPROM settings!");

  // Read credentials from EEPROM
  String ssid1 = readStringFromEEPROM(SSID1_ADDR, SHORT_FIELD_SIZE);
  String pass1 = readStringFromEEPROM(PASS1_ADDR, SHORT_FIELD_SIZE);
  String ssid2 = readStringFromEEPROM(SSID2_ADDR, SHORT_FIELD_SIZE);
  String pass2 = readStringFromEEPROM(PASS2_ADDR, SHORT_FIELD_SIZE);
  ha_url = readStringFromEEPROM(HA_URL_ADDR, LONG_FIELD_SIZE);
  ha_token = readStringFromEEPROM(HA_TOKEN_ADDR, LONG_FIELD_SIZE);
  apiKey = readStringFromEEPROM(API_KEY_ADDR, SHORT_FIELD_SIZE);
  EEPROM.get(CHANNEL_ADDR, channel);

  // 📶 Dodaj sieci do WiFiMulti
  wifiMulti.addAP(ssid1.c_str(), pass1.c_str());
  wifiMulti.addAP(ssid2.c_str(), pass2.c_str());

  Serial.println("📶 Łączenie z jedną z zapisanych sieci...");
  unsigned long wifiStart = millis();
  while (wifiMulti.run() != WL_CONNECTED && (millis() - wifiStart) < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ Nie udało się połączyć z Wi-Fi. Restart...");
    ESP.restart();
  }

  Serial.println("\n✅ Połączono z Wi-Fi:");
  Serial.println(WiFi.SSID());
  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setPassword("admin");
  ArduinoOTA.begin();

  ThingSpeak.begin(client);
  client.setTimeout(5000);  // Timeout dla ThingSpeak
  mySerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  httpsClient.setInsecure();

  startTime = millis();
}

void loop() {
  wifiMulti.run();
  ArduinoOTA.handle();

  uint16_t distance;
  while (collected < REQUIRED_SAMPLES && readDistanceFrame(distance)) {
    if (distance >= SENSOR_MIN_MM && distance <= SENSOR_MAX_MM) {
      validMeasurements[collected++] = distance;
    }
  }

  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();
    if (collected >= MIN_SAMPLES) {
      sendMeasurements(calculateMedianCm());
    } else {
      Serial.println("❌ Za mało poprawnych próbek do wysłania.");
    }
    collected = 0;
  }

  // 3️⃣ Automatyczny restart co 24h
  if (millis() - startTime >= RESTART_INTERVAL) {
    ESP.restart();
  }

}
