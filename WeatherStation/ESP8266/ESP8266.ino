#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

/* ============================================================
  Wi-Fi Configuration
============================================================ */
constexpr char WIFI_SSID[]     = "RT-WiFi-17D9";
constexpr char WIFI_PASSWORD[] = "3UBeaUzZrE";

/* ============================================================
  Remote API Configuration
============================================================ */
constexpr char SERVER_URL[] =
    "https://garbage-pi-eight.vercel.app/api/sensor";

/* ============================================================
  NTP Configuration
============================================================ */
constexpr char NTP_SERVER[] = "pool.ntp.org";

constexpr long GMT_OFFSET_SEC     = 10800; // UTC+3 (Москва)
constexpr int DAYLIGHT_OFFSET_SEC = 0;

/* ============================================================
  Timeouts
============================================================ */
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RECONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t HTTPS_TIMEOUT_MS = 10000;

/* ============================================================
  Utility Functions
============================================================ */

/**
 * Подключение к Wi-Fi.
 * Возвращает true при успешном подключении.
 */
bool connectWiFi()
{
    Serial.print(F("Connecting to WiFi"));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t startTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print('.');

        if (millis() - startTime > WIFI_CONNECT_TIMEOUT_MS)
        {
            Serial.println(F("\nWiFi connection timeout."));
            return false;
        }
    }

    Serial.println();
    Serial.print(F("Connected. IP: "));
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

    Serial.println(F("WiFi disconnected. Reconnecting..."));

    WiFi.reconnect();

    uint32_t startTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);

        if (millis() - startTime > WIFI_RECONNECT_TIMEOUT_MS)
        {
            Serial.println(F("Reconnection failed."));
            return false;
        }
    }

    Serial.println(F("WiFi reconnected."));
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

    Serial.print(F("Synchronizing time"));

    time_t now = time(nullptr);

    while (now < 24 * 3600)
    {
        delay(500);
        Serial.print('.');

        now = time(nullptr);
    }

    Serial.println(F("\nTime synchronized."));
}

/**
 * Отправка показаний на сервер.
 */
void sendToServer(float temperature, float humidity)
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
                 "&hum=" + String(humidity, 1);

    Serial.println();
    Serial.print(F("Sending GET request: "));
    Serial.println(url);

    if (!http.begin(client, url))
    {
        Serial.println(F("HTTP begin failed."));
        return;
    }

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
        String response = http.getString();

        Serial.print(F("Response: "));
        Serial.println(response);

        /*
          ACK = пакет успешно дошёл
          и сервер вернул HTTP 200.
        */
        Serial.println(F("ACK"));
    }
    else if (httpCode > 0)
    {
        Serial.printf("HTTP code: %d\n", httpCode);

        /*
          Сервер ответил,
          но ответ не является успешным.
        */
        Serial.println(F("NACK"));
    }
    else
    {
        Serial.print(F("Request failed: "));
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
 * {"temp":25.4,"hum":61.2}
 */
void processIncomingJson(const String& jsonLine)
{
    JsonDocument doc;

    DeserializationError error =
        deserializeJson(doc, jsonLine);

    if (error)
    {
        Serial.print(F("JSON parse error: "));
        Serial.println(error.c_str());
        return;
    }

    if (!doc["temp"].is<float>() ||
        !doc["hum"].is<float>())
    {
        Serial.println(F("Missing temp/hum fields."));
        return;
    }

    float temperature = doc["temp"];
    float humidity    = doc["hum"];

    Serial.printf(
        "Temperature: %.1f °C | Humidity: %.1f %%\n",
        temperature,
        humidity
    );

    sendToServer(temperature, humidity);
}

/* ============================================================
  Arduino Setup
============================================================ */

void setup()
{
    Serial.begin(9600);

    delay(1000);

    Serial.println();
    Serial.println(F("ESP8266 Sensor Gateway Starting"));

    if (connectWiFi())
    {
        synchronizeTime();
    } else {
        Serial.println(F("Startup WiFi connection failed. Time sync skipped."));
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
            Serial.print(F("Received JSON: "));
            Serial.println(line);

            processIncomingJson(line);
        }
    }

    delay(10);
}