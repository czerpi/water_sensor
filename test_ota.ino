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

// EEPROM helper functions
String readStringFromEEPROM(int addr) {
  char buffer[65]; // 64 + 1 na null-terminator
  for (int i = 0; i < 64; i++) {
    char ch = EEPROM.read(addr + i);
    if (ch == 0) {
      buffer[i] = '\0';
      break;
    }
    buffer[i] = ch;
  }
  buffer[64] = '\0'; // zabezpieczenie, gdyby nie było wcześniejszego zera
  return String(buffer);
}

void saveStringToEEPROM(int addr, const String& data) {
  int len = data.length();
  for (int i = 0; i < 64; i++) {
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

// ======= KONFIGURACJA =======
#define WINDOW_SIZE 40          // liczba pomiarów do uśredniania
#define MIN_SAMPLES 3  // Minimalna liczba próbek, żeby liczyć i wysyłać
#define SEND_INTERVAL 60000      // co ile wysyłać dane (ms) — 1 minuta
#define RESTART_INTERVAL 86400000 // restart po 24h (ms)
#define TO_ZERO_LEVEL 237 // odleglosc do dna   

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

HardwareSerial mySerial(1);
WiFiMulti wifiMulti;


WiFiClient client;
WiFiClientSecure httpsClient;
HTTPClient https;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // EEPROM initialization for both ESP32 and ESP8266
  // EEPROM.begin(EEPROM_SIZE);
  // // ⚠️ Only run once to save your config
  // saveStringToEEPROM(0, "Elion_1B11");
  // saveStringToEEPROM(64, "25808A0DE0866");
  // saveStringToEEPROM(128, "12345678");
  // saveStringToEEPROM(192, "B0mb0w012345678");
  // saveStringToEEPROM(256, "https://6bb29091fb4a625ddf4eb8296097bdc1.czerpak.pl/api/states/sensor.water_level");
  // saveStringToEEPROM(512, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmMTY4MjBkYzUyZTk0NWUwYjFhYjIyNjE0MWY4ZmQ1OSIsImlhdCI6MTc2MDI1NDU5NiwiZXhwIjoyMDc1NjE0NTk2fQ.0YmItXQNccBBpeWlfqmJomy4X5FGnZYP7YIOtxGq-Qg");
  // saveStringToEEPROM(768, "A4Y84M4MKLTNPXWI");
  // EEPROM.write(960, 3108182);
  // EEPROM.commit();

  // Serial.println("✅ Saved EEPROM settings!");

  // Read credentials from EEPROM
  String ssid1 = readStringFromEEPROM(0);
  String pass1 = readStringFromEEPROM(64);
  String ssid2 = readStringFromEEPROM(128);
  String pass2 = readStringFromEEPROM(192);
  ha_url = readStringFromEEPROM(256);
  ha_token = readStringFromEEPROM(512);
  apiKey = readStringFromEEPROM(768);
  channel = EEPROM.read(960);

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
  Stream &serialPort = mySerial;

  while (serialPort.available() >= 4) {
    uint8_t buf[4];
    serialPort.readBytes(buf, 4);
    if (buf[0] == 0xFF) {
      uint16_t distance = (buf[1] << 8) | buf[2];
      uint8_t checksum = (buf[0] + buf[1] + buf[2]) & 0xFF;
      if (checksum == buf[3]) {

        // ⛔️ Ignoruj pomiary < 30 cm lub > 230 cm 
        if (distance < 300 || distance > 2300 ) {
          continue;
        }
        samples[sampleIndex] = distance;
        sampleIndex++;
        if (sampleIndex >= WINDOW_SIZE) {
          sampleIndex = 0;
          bufferFull = true;
        }
      }
    } else {
      serialPort.read(); // zły bajt
    }
  }

  // 2️⃣ Co 1 minutę: licz średnią i wysyłaj
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    int count = bufferFull ? WINDOW_SIZE : sampleIndex;
    if (count >= MIN_SAMPLES) {
      // Oblicz medianę z próbek
      uint16_t sorted[WINDOW_SIZE];
      for (int i = 0; i < count; i++) sorted[i] = samples[i];
      // Funkcja porównująca do qsort
      auto cmp_uint16 = [](const void* a, const void* b) -> int {
        uint16_t aa = *(const uint16_t*)a, bb = *(const uint16_t*)b;
        return (aa > bb) - (aa < bb);
      };
      qsort(sorted, count, sizeof(uint16_t), cmp_uint16);
      float median_mm;
      if (count % 2 == 0) {
        median_mm = (sorted[count/2 - 1] + sorted[count/2]) / 2.0;
      } else {
        median_mm = sorted[count/2];
      }
      float median_cm = TO_ZERO_LEVEL - (median_mm / 10.0);  // mm → cm
      Serial.println(median_cm);
      // send to thingspeak
      ThingSpeak.setField(1, median_cm);
      ThingSpeak.setField(2, WiFi.RSSI());
      ThingSpeak.setField(3, ESP.getFreeHeap());
      ThingSpeak.setField(4, millis() / 1000); // uptime w sekundach
      int code = ThingSpeak.writeFields(channel, apiKey.c_str());
      if (code == 200) {
        Serial.println("✅ Dane wysłane do ThingSpeak!");
      } else {
        Serial.printf("❌ Błąd: %d\n", code);
      }

      //send to home assistant
      if (median_cm) {
        bool started = https.begin(httpsClient, ha_url);
        if (started) {
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
    }
  }

  // 3️⃣ Automatyczny restart co 24h
  if (millis() - startTime >= RESTART_INTERVAL) {
    ESP.restart();
  }

}