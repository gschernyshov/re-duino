#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

/* ============================================================
  Wi-Fi Configuration
============================================================ */
constexpr char WIFI_SSID[]     = "YOUR_WIFI_SSID";
constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

/* ============================================================
  Remote API Configuration
============================================================ */
constexpr char SERVER_URL[] =
    "YOUR_SERVER_URL";

constexpr char API_KEY[] = "YOUR_API_KEY"; 

/* ============================================================
  NTP Configuration
============================================================ */
constexpr char NTP_SERVER[] = "pool.ntp.org";

constexpr uint32_t GMT_OFFSET_SEC      = 10800; // UTC+3 (Москва)
constexpr uint32_t DAYLIGHT_OFFSET_SEC = 0;

/* ============================================================
  Timeouts
============================================================ */
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS   = 15000;
constexpr uint32_t WIFI_RECONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t HTTPS_TIMEOUT_MS          = 10000;

/* ============================================================
  Utility Functions
============================================================ */

/**
 * Подключение к Wi-Fi.
 * Возвращает true при успешном подключении.
 */
bool connectWiFi()
{
    Serial.print(F("[ESP8266] Connecting to WiFi"));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t startTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print('.');

        if (millis() - startTime > WIFI_CONNECT_TIMEOUT_MS)
        {
            Serial.println(F("\n[ESP8266] WiFi connection timeout"));
            return false;
        }
    }

    Serial.print(F("\n[ESP8266] Connected. IP: "));
    Serial.println(WiFi.localIP());

    return true;
}

/**
 * Проверка подключения к Wi-Fi.
 * Если связь потеряна — пытаемся восстановить.
 */
bool ensureWiFiConnection()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return true;
    }

    Serial.println(F("[ESP8266] WiFi disconnected. Reconnecting..."));

    WiFi.reconnect();

    uint32_t startTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);

        if (millis() - startTime > WIFI_RECONNECT_TIMEOUT_MS)
        {
            Serial.println(F("[ESP8266] Reconnection failed"));
            return false;
        }
    }

    Serial.println(F("[ESP8266] WiFi reconnected"));

    return true;
}

/**
 * Ожидание синхронизации времени через NTP.
 * TLS-соединения часто требуют корректного времени.
 */
void synchronizeTime()
{
    configTime(
        GMT_OFFSET_SEC,
        DAYLIGHT_OFFSET_SEC,
        NTP_SERVER
    );

    Serial.print(F("[ESP8266] Synchronizing time"));

    time_t now = time(nullptr);

    while (now < 24 * 3600)
    {
        delay(500);
        Serial.print('.');

        now = time(nullptr);
    }

    Serial.println(F("\n[ESP8266] Time synchronized"));
}

/**
 * Отправка показаний на сервер.
 */
void sendToServer(float temperature, float humidity, float illumination)
{
    if (!ensureWiFiConnection())
    {
        return;
    }

    WiFiClientSecure client;

    // Отключаем проверку SSL-сертификата.
    client.setInsecure();
    client.setTimeout(HTTPS_TIMEOUT_MS);

    HTTPClient http;

    String url = String(SERVER_URL) +
                 "?temp=" + String(temperature, 1) +
                 "&hum=" + String(humidity, 1) +
                 "&illum=" + String(illumination, 1); 

    Serial.print(F("[ESP8266] Sending GET request: "));
    Serial.println(url);

    if (!http.begin(client, url))
    {
        Serial.println(F("[ESP8266] HTTP begin failed"));
        return;
    }

    // Добавляем заголовок идентификации.
    http.addHeader("X-API-Key", API_KEY);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
        String response = http.getString();

        Serial.print(F("[ESP8266] Response: "));
        Serial.println(response);

        /*
          ACK = пакет успешно дошёл
          и сервер вернул HTTP 200.
        */
        Serial.println(F("ACK"));
    }
    else if (httpCode > 0)
    {
        Serial.printf("[ESP8266] HTTP code: %d\n", httpCode);

        /*
          Сервер ответил,
          но ответ не является успешным.
        */
        Serial.println(F("NACK"));
    }
    else
    {
        Serial.print(F("[ESP8266] Request failed: "));
        Serial.println(http.errorToString(httpCode));

        /*
          Ошибка сети, DNS,
          TLS или соединения.
        */
        Serial.println(F("NACK"));
    }


    http.end();
    client.stop();
}

/**
 * Обработка входящего JSON из UART.
 *
 * Ожидаемый формат:
 * {"temp":25.4,"hum":61.2,"illum":12.3}
 */
void processIncomingJson(const String& jsonLine)
{
    JsonDocument doc;

    DeserializationError error =
        deserializeJson(doc, jsonLine);

    if (error)
    {
        Serial.print(F("[ESP8266] JSON parse error: "));
        Serial.println(error.c_str());
        return;
    }

    if (!doc["temp"].is<float>() ||
        !doc["hum"].is<float>()  || 
        !doc["illum"].is<float>())
    {
        Serial.println(F("[ESP8266] Missing temp/hum fields"));
        return;
    }

    float temperature  = doc["temp"];
    float humidity     = doc["hum"];
    float illumination = doc["illum"];

    sendToServer(temperature, humidity, illumination);
}

/* ============================================================
  Arduino Setup
============================================================ */

void setup()
{
    Serial.begin(9600);

    delay(1000);

    Serial.println();
    Serial.println(F("[ESP8266] Sensor Gateway Starting"));

    if (connectWiFi())
    {
        synchronizeTime();
    } else 
    {
        Serial.println(F("[ESP8266] Startup WiFi connection failed. Time sync skipped"));
    }
}

/* ============================================================
  Main Loop
============================================================ */

void loop()
{
    if (Serial.available())
    {
        String line = Serial.readStringUntil('\n');

        line.trim();

        if (!line.isEmpty())
        {
            Serial.print(F("[ESP8266] Received JSON: "));
            Serial.println(line);

            processIncomingJson(line);
        }
    }

    delay(10);
}