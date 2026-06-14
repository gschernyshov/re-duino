#include <TroykaLight.h>
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
namespace Pins 
{
    constexpr uint8_t ESP_SLEEP  = 2;
    constexpr uint8_t GL         = A0;
    constexpr uint8_t DHT        = 5;
    constexpr uint8_t BUZZER     = 4;
    constexpr uint8_t LED_YELLOW = A3;
    constexpr uint8_t LED_GREEN  = A4;
    constexpr uint8_t LED_RED    = A5;
    constexpr uint8_t LCD_RS     = 6;
    constexpr uint8_t LCD_EN     = 7;
    constexpr uint8_t LCD_DB4    = 8;
    constexpr uint8_t LCD_DB5    = 9;
    constexpr uint8_t LCD_DB6    = 10;
    constexpr uint8_t LCD_DB7    = 11;
}

/* ============================================================
   Hardware objects
============================================================ */
TroykaLight sensorLight(Pins::GL);
DHT dht(Pins::DHT, DHT11);
LiquidCrystal lcd(Pins::LCD_RS,  Pins::LCD_EN,
                  Pins::LCD_DB4, Pins::LCD_DB5,
                  Pins::LCD_DB6, Pins::LCD_DB7);

/* ============================================================
   Timing & constants
============================================================ */
constexpr uint32_t SENSOR_PERIOD_MS = 1500;
constexpr uint32_t SCREEN_PERIOD_MS = 3000; 
constexpr uint32_t SEND_PERIOD_MS   = 1200000; 
constexpr uint32_t ACK_TIMEOUT_MS   = 30000; 
constexpr uint32_t RETRY_DELAY_MS   = 40000; 
constexpr uint8_t MAX_RETRIES       = 3;

// Параметры звуковых сигналов (частота, длительность, пауза).
constexpr uint16_t SENDING_FREQ  = 1000;
constexpr uint16_t SENDING_DUR   = 50;
constexpr uint16_t SUCCESS_FREQ  = 1500;
constexpr uint16_t SUCCESS_DUR   = 40;
constexpr uint16_t SUCCESS_PAUSE = 80;
constexpr uint16_t ERROR_FREQ    = 800;
constexpr uint16_t ERROR_DUR     = 500;

/* ============================================================
   Global state
============================================================ */
float currentIllumination = 0.0f;
float currentTemperature  = 0.0f;
float currentHumidity     = 0.0f;

unsigned long lastSensorReadTime  = 0;
unsigned long lastScreenSwitchTime = 0;
unsigned long lastSendTime = 0;

// Состояние отправки данных в ESP8266.
bool waitingAck = false;

// Текущее количество попыток.
uint8_t retryCount = 0;           

enum class DisplayState: uint8_t
{
    TEMPERATURE,
    HUMIDITY,
    ILLUMINATION,
    ERROR
};

DisplayState currentScreen = DisplayState::TEMPERATURE;

// Конечный автомат для управления зуммером и светодиодом.
enum class BuzzerState : uint8_t 
{
    BUZZER_IDLE,
    BUZZER_SENDING,
    BUZZER_SUCCESS_WAIT,
    BUZZER_SUCCESS_SECOND,
    BUZZER_SUCCESS_END,
    BUZZER_ERROR,
};

BuzzerState buzzerState = BuzzerState::BUZZER_IDLE;

// Время окончания текущего действия.
unsigned long buzzerTimer = 0;  

/* ============================================================
   Display strings
============================================================ */
namespace LCDtext 
{
    const char TEMP[]  = "\x54\x65\xBC\xBE\x65\x70\x61\xBF\x79\x70\x61: ";
    const char HUM[]   = "\x42\xBB\x61\xB6\xBD\x6F\x63\xBF\xC4: ";
    const char ILLUM[] = "\x4F\x63\xB3\x65\xE6\x65\xBD\xB8\x65: ";
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

        if (c == '\n')
        {
            line.trim();

            PC_SERIAL.print(F("[Leonardo] RX: "));
            PC_SERIAL.println(line);

            if (line == F("ACK"))
            {
                waitingAck = false;
                retryCount = 0;  

                startSuccessSignal();
                logMessage(F("[Leonardo] Packet delivered successfully"));
            }
            else if (line == F("NACK"))
            {
                waitingAck = false;

                startErrorSignal();
                logMessage(F("[Leonardo] Packet delivery failed"));
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

    if (millis() - lastSendTime > ACK_TIMEOUT_MS)
    {
        waitingAck = false;

        startErrorSignal();
        logMessage(
            F("[Leonardo] ACK timeout")
        );
    }
}

void logMessage(const __FlashStringHelper* msg)
{
    PC_SERIAL.print(F("[Leonardo] "));
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
    lcd.print(F(" \x99""C "));

    lcd.print(currentTemperature + 273.15f, 0);
    lcd.print(F(" K"));
}

void showHumidity()
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(LCDtext::HUM);

    lcd.setCursor(0, 1);

    lcd.print(currentHumidity, 0);
    lcd.print(F(" %"));
}

void showIllumination()
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(LCDtext::ILLUM);

    lcd.setCursor(0, 1);

    lcd.print(currentIllumination, 0);
    lcd.print(F(" Lx"));
}

void showError()
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(F("Sensor Error"));
}

/* ============================================================
   JSON Communication
============================================================ */

void sendJSON(float temperature, float humidity, float illumination)
{
    JsonDocument doc;

    doc["temp"] = temperature;
    doc["hum"]  = humidity;
    doc["illum"] = illumination;

    String payload;
    serializeJson(doc, payload);

    WIFI_SERIAL.println(payload);

    PC_SERIAL.print(F("[Leonardo] TX: "));
    PC_SERIAL.println(payload);
}

void handleSending()
{
    if (waitingAck)
    {
        return;
    }

    if (retryCount >= MAX_RETRIES) 
    {
        retryCount = 0;
        logMessage(F("[Leonardo] Max retries reached, resetting"));
        return;
    }

    unsigned long now = millis();
    uint32_t interval = (retryCount > 0) ? RETRY_DELAY_MS : SEND_PERIOD_MS;

    if (now - lastSendTime < interval)
    {
        return;
    }

    lastSendTime = now;
    startSendingSignal();

    sendJSON(currentTemperature, currentHumidity, currentIllumination);
    
    waitingAck = true;

    retryCount++;
}

/* ============================================================
   Validation
============================================================ */

bool isDHTDataValid(float temperature, float humidity)
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
    unsigned long now = millis();

    if (now - lastSensorReadTime < SENSOR_PERIOD_MS)
    {
        return;
    }

    lastSensorReadTime = now;

    dht.read();    

    switch (dht.getState())
    {
        case DHT_OK:
        {
            float temperature = dht.getTemperatureC();
            float humidity    = dht.getHumidity();

            if (!isDHTDataValid(temperature, humidity))
            {
                currentScreen = DisplayState::ERROR;

                logMessage(F("Invalid sensor values"));
            } else 
            {
                currentTemperature = temperature;
                currentHumidity    = humidity;

                if (currentScreen == DisplayState::ERROR)
                {
                    currentScreen = DisplayState::TEMPERATURE;
                }

                // PC_SERIAL.print(F("[Leonardo] Temperature: "));
                // PC_SERIAL.print(currentTemperature);
                // PC_SERIAL.print(F(" C | Humidity: "));
                // PC_SERIAL.print(currentHumidity);
                // PC_SERIAL.println(F(" %"));
            }

            break;
        }

        case DHT_ERROR_CHECKSUM:
            logMessage(F("DHT checksum error"));
            currentScreen = DisplayState::ERROR;
            break;

        case DHT_ERROR_TIMEOUT:
            logMessage(F("DHT timeout error"));
            currentScreen = DisplayState::ERROR;
            break;

        case DHT_ERROR_NO_REPLY:
            logMessage(F("DHT no reply"));
            currentScreen = DisplayState::ERROR;
            break;

        default:
            logMessage(F("Unknown DHT error"));
            currentScreen = DisplayState::ERROR;
            break;
    }

    sensorLight.read();

    currentIllumination = sensorLight.getLightLux();

    // PC_SERIAL.print(F("[Leonardo] Light: "));
    // PC_SERIAL.print(currentIllumination);
    // PC_SERIAL.println(F(" Lx"));
}

/* ============================================================
   Display Update
============================================================ */

void updateDisplay()
{
    unsigned long now = millis();

    if (now - lastScreenSwitchTime < SCREEN_PERIOD_MS)
    {
        return;
    }

    lastScreenSwitchTime = now;

    switch (currentScreen)
    {
        case DisplayState::TEMPERATURE:
            showTemperature();
            currentScreen = DisplayState::HUMIDITY;
            break;

        case DisplayState::HUMIDITY:
            showHumidity();
            currentScreen = DisplayState::ILLUMINATION;
            break;

        case DisplayState::ILLUMINATION:
            showIllumination();
            currentScreen = DisplayState::TEMPERATURE;
            break;

        case DisplayState::ERROR:
            showError();
            break;
    }
}

/* ============================================================
   Sound & Light Indication 
============================================================ */

void startSendingSignal() 
{
    if (buzzerState == BuzzerState::BUZZER_IDLE)
    {
        tone(Pins::BUZZER, SENDING_FREQ, SENDING_DUR);
        digitalWrite(Pins::LED_YELLOW, HIGH);  

        buzzerState = BuzzerState::BUZZER_SENDING;
        buzzerTimer = millis() + SENDING_DUR;
    }
}

void startSuccessSignal() 
{
    if (buzzerState == BuzzerState::BUZZER_IDLE) 
    {
        tone(Pins::BUZZER, SUCCESS_FREQ, SUCCESS_DUR);
        digitalWrite(Pins::LED_GREEN, HIGH);        

        buzzerState = BuzzerState::BUZZER_SUCCESS_WAIT;
        buzzerTimer = millis() + SUCCESS_DUR;
    }
}

void startErrorSignal() 
{
    if (buzzerState == BuzzerState::BUZZER_IDLE) 
    {
        tone(Pins::BUZZER, ERROR_FREQ, ERROR_DUR);
        digitalWrite(Pins::LED_RED, HIGH);

        buzzerState = BuzzerState::BUZZER_ERROR;
        buzzerTimer = millis() + ERROR_DUR;
    }
}

void updateIndication() 
{
    unsigned long now = millis();
    
    switch (buzzerState) 
    {
        case BuzzerState::BUZZER_SENDING:
            if (now >= buzzerTimer) 
            {
                noTone(Pins::BUZZER);
                digitalWrite(Pins::LED_YELLOW, LOW);

                buzzerState = BuzzerState::BUZZER_IDLE;
            }
            break;
            
        case BuzzerState::BUZZER_SUCCESS_WAIT:
            if (now >= buzzerTimer) 
            {
                noTone(Pins::BUZZER);
                digitalWrite(Pins::LED_GREEN, LOW);   
               
                buzzerState = BuzzerState::BUZZER_SUCCESS_SECOND;
                buzzerTimer = now + SUCCESS_PAUSE;
            }
            break;
            
        case BuzzerState::BUZZER_SUCCESS_SECOND:
            if (now >= buzzerTimer) 
            {
                tone(Pins::BUZZER, SUCCESS_FREQ, SUCCESS_DUR);
                digitalWrite(Pins::LED_GREEN, HIGH);  

                buzzerState = BuzzerState::BUZZER_SUCCESS_END;
                buzzerTimer = now + SUCCESS_DUR;
            }
            break;
            
        case BuzzerState::BUZZER_SUCCESS_END:
            if (now >= buzzerTimer) 
            {
                noTone(Pins::BUZZER);
                digitalWrite(Pins::LED_GREEN, LOW);   

                buzzerState = BuzzerState::BUZZER_IDLE;
            }
            break;

        case BuzzerState::BUZZER_ERROR:
            if (now >= buzzerTimer) 
            {
                noTone(Pins::BUZZER);
                digitalWrite(Pins::LED_RED, LOW);

                buzzerState = BuzzerState::BUZZER_IDLE;
            }
            break;

        default:
            break;
    }
}

/* ============================================================
   Setup
============================================================ */

void setup()
{
    pinMode(Pins::ESP_SLEEP, OUTPUT);
    digitalWrite(Pins::ESP_SLEEP, LOW);

    pinMode(Pins::BUZZER, OUTPUT);
    digitalWrite(Pins::BUZZER, LOW);

    pinMode(Pins::LED_YELLOW, OUTPUT);
    digitalWrite(Pins::LED_YELLOW, LOW);
    pinMode(Pins::LED_GREEN, OUTPUT);
    digitalWrite(Pins::LED_GREEN, LOW);
    pinMode(Pins::LED_RED, OUTPUT);
    digitalWrite(Pins::LED_RED, LOW);

    PC_SERIAL.begin(9600);
    WIFI_SERIAL.begin(9600);
    delay(1000);

    dht.begin();

    lcd.begin(16, 2);

    lcd.clear();
    lcd.print(F("System Start..."));

    logMessage(F("================================"));
    logMessage(F("System Start..."));
    logMessage(F("================================"));

    delay(1500);
}

/* ============================================================
   Main Loop
============================================================ */

void loop()
{
    // Периодическое чтение датчиков.
    updateSensor();

    // Периодическое обновление LCD.
    updateDisplay();  

    // Периодическая отправка данных.
    handleSending(); 

    // Обработка ответов ESP8266: ACK, NACK и диагностические сообщения.
    processESPCommunication();

    // Контроль потери ACK.
    checkAckTimeout(); 

    // Обновление индикации (зуммер + светодиод).
    updateIndication(); 
}


