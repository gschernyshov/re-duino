#include <TroykaDHT.h>
#include <LiquidCrystal.h>
#include <ArduinoJson.h>

/* ============================================================
   Serial Interfaces

   Serial  -> ПК (USB)
   Serial1 -> ESP8266

   Arduino Leonardo поддерживает оба интерфейса одновременно.
============================================================ */

#define PC_SERIAL   Serial
#define WIFI_SERIAL Serial1

/* ============================================================
   Pin definitions
============================================================ */
namespace Pins {
    constexpr uint8_t DHT     = 4;
    constexpr uint8_t LCD_RS  = 6;
    constexpr uint8_t LCD_EN  = 7;
    constexpr uint8_t LCD_DB4 = 8;
    constexpr uint8_t LCD_DB5 = 9;
    constexpr uint8_t LCD_DB6 = 10;
    constexpr uint8_t LCD_DB7 = 11;
}

/* ============================================================
   Hardware objects
============================================================ */
DHT dht(Pins::DHT, DHT11);
LiquidCrystal lcd(Pins::LCD_RS, Pins::LCD_EN,
                  Pins::LCD_DB4, Pins::LCD_DB5,
                  Pins::LCD_DB6, Pins::LCD_DB7);

/* ============================================================
   Timing & constants
============================================================ */
constexpr uint32_t SENSOR_PERIOD_MS = 3000;
constexpr uint32_t SCREEN_DELAY_MS  = 2500;
constexpr uint32_t ACK_TIMEOUT_MS   = 5000;

/* ============================================================
   Global state
============================================================ */
bool sensorValid = false;
float currentTemperature = 0.0f;
float currentHumidity    = 0.0f;

enum class DisplayState : uint8_t
{
    TEMPERATURE,
    HUMIDITY,
    ERROR
};

DisplayState currentScreen = DisplayState::TEMPERATURE;

unsigned long lastSensorReadTime  = 0;
unsigned long lastScreenSwitchTime = 0;

// Состояние отправки данных в ESP8266
bool waitingAck = false;
unsigned long lastPacketTime = 0;

/* ============================================================
   Display strings
============================================================ */
namespace LCDtext {
    const char TEMP[] = "\x54\x65\xBC\xBE\x65\x70\x61\xBF\x79\x70\x61: ";
    const char HUM[]  = "\x42\xBB\x61\xB6\xBD\x6F\x63\xBF\xC4: ";
}

/* ============================================================
   Logging
============================================================ */

/*
   Пересылает всё, что пишет ESP8266,
   в монитор порта Arduino IDE.
*/
void processESPCommunication()
{
    static String line;

    while (WIFI_SERIAL.available())
    {
        char c = WIFI_SERIAL.read();

        // Показываем весь вывод ESP8266 в мониторе порта
        PC_SERIAL.write(c);

        if (c == '\n')
        {
            line.trim();

            /*
            ACK означает:
            - ESP8266 получил JSON
            - отправил данные на сервер
            - сервер вернул HTTP 200
            */
            if (line == F("ACK"))
            {
                waitingAck = false;

                PC_SERIAL.println(
                    F("[INFO] Packet delivered successfully")
                );
            }

            /*
            NACK означает ошибку:
            - битый JSON
            - ошибка сети
            - ошибка сервера
            - ошибка HTTP-запроса
            */
            else if (line == F("NACK"))
            {
                waitingAck = false;

                PC_SERIAL.println(
                    F("[WARNING] Packet delivery failed")
                );
            }

            line = "";
        }
        else
        {
            line += c;
        }
    }
}

/*
Проверяем, дождались ли подтверждения
от ESP8266 после отправки JSON.
*/
void checkAckTimeout()
{
    if (!waitingAck)
    {
    return;
    }


    if (millis() - lastPacketTime > ACK_TIMEOUT_MS)
    {
        PC_SERIAL.println(
            F("[WARNING] ACK timeout")
        );

        waitingAck = false;
    }
}

void logMessage(const __FlashStringHelper* msg)
{
    PC_SERIAL.println(msg);
}

/* ============================================================
   LCD Functions
============================================================ */

void showTemperature()
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(LCDtext::TEMP);

    lcd.setCursor(0, 1);

    lcd.print(currentTemperature, 0);
    lcd.print(F("\x99""C "));

    lcd.print(currentTemperature + 273.15f, 0);
    lcd.print(F("\x99""K "));
}

void showHumidity()
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(LCDtext::HUM);

    lcd.setCursor(0, 1);

    lcd.print(currentHumidity, 0);
    lcd.print('%');
}

void showError()
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(F("DHT Error"));
}

/* ============================================================
   JSON Communication
============================================================ */

void sendJSON(float temperature, float humidity)
{
    if (waitingAck)
    {
        return;
    }

    JsonDocument doc;

    doc["temp"] = temperature;
    doc["hum"]  = humidity;

    String payload;
    serializeJson(doc, payload);

    WIFI_SERIAL.println(payload);

    waitingAck = true;
    lastPacketTime = millis();

    PC_SERIAL.print(F("[ESP8266 TX] "));
    PC_SERIAL.println(payload);
}

/* ============================================================
   Validation
============================================================ */

bool isSensorDataValid(float temperature, float humidity)
{
    return (
        temperature >= -40.0f &&
        temperature <= 80.0f &&
        humidity >= 0.0f &&
        humidity <= 100.0f
    );
}

/* ============================================================
   Sensor Update
============================================================ */

void updateSensor()
{
    if (millis() - lastSensorReadTime < SENSOR_PERIOD_MS)
    {
        return;
    }

    lastSensorReadTime = millis();

    dht.read();

    switch (dht.getState())
    {
        case DHT_OK:
        {
            float temperature = dht.getTemperatureC();
            float humidity    = dht.getHumidity();

            if (!isSensorDataValid(temperature, humidity))
            {
                logMessage(F("Invalid sensor values"));

                sensorValid   = false;
                currentScreen = DisplayState::ERROR;

                return;
            }

            currentTemperature = temperature;
            currentHumidity    = humidity;

            sensorValid = true;

            PC_SERIAL.print(F("Temperature: "));
            PC_SERIAL.print(currentTemperature);
            PC_SERIAL.print(F(" C | Humidity: "));
            PC_SERIAL.print(currentHumidity);
            PC_SERIAL.println(F(" %"));

            sendJSON(currentTemperature, currentHumidity);

            break;
        }

        case DHT_ERROR_CHECKSUM:
            logMessage(F("DHT checksum error"));
            sensorValid   = false;
            currentScreen = DisplayState::ERROR;
            break;

        case DHT_ERROR_TIMEOUT:
            logMessage(F("DHT timeout error"));
            sensorValid   = false;
            currentScreen = DisplayState::ERROR;
            break;

        case DHT_ERROR_NO_REPLY:
            logMessage(F("DHT no reply"));
            sensorValid   = false;
            currentScreen = DisplayState::ERROR;
            break;

        default:
            logMessage(F("Unknown DHT error"));
            sensorValid   = false;
            currentScreen = DisplayState::ERROR;
            break;
    }
}

/* ============================================================
   Display Update
============================================================ */

void updateDisplay()
{
    if (millis() - lastScreenSwitchTime < SCREEN_DELAY_MS)
    {
        return;
    }

    lastScreenSwitchTime = millis();

    if (!sensorValid)
    {
        showError();
        return;
    }

    switch (currentScreen)
    {
        case DisplayState::TEMPERATURE:
            showTemperature();
            currentScreen = DisplayState::HUMIDITY;
            break;

        case DisplayState::HUMIDITY:
            showHumidity();
            currentScreen = DisplayState::TEMPERATURE;
            break;

        case DisplayState::ERROR:
            showError();
            break;
    }
}

/* ============================================================
   Setup
============================================================ */

void setup()
{
    PC_SERIAL.begin(9600);
    WIFI_SERIAL.begin(9600);

    delay(1000);

    dht.begin();

    lcd.begin(16, 2);

    lcd.clear();
    lcd.print(F("System Start..."));

    logMessage(F("================================"));
    logMessage(F("Leonardo Sensor Station Started"));
    logMessage(F("ESP8266 Log Bridge Enabled"));
    logMessage(F("================================"));

    delay(1500);

    lcd.clear();
}

/* ============================================================
   Main Loop
============================================================ */

void loop()
{
    /*
    Обрабатываем ответы ESP8266:
    ACK, NACK и диагностические сообщения.
    */
    processESPCommunication();

    /*
    Периодическое чтение датчика.
    */
    updateSensor();

    /*
    Обновление LCD.
    */
    updateDisplay();

    /*
    Контроль потери ACK.
    */
    checkAckTimeout();
}


