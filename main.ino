#include <Wire.h>
#include <Arduino.h>
#include "DFRobot_SHT20.h"
#include <GyverOLED.h>
#include "WiFi.h"
#include "Button.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <JSON_Decoder.h>
#include <OpenWeather.h>
#include <ArduinoOTA.h>
#include <freertos/semphr.h>  // Для работы с мьютексами

#define DEFAULT_SENSOR_PERIOD 60000
#define HOLD_SENSOR_PERIOD 250
#define PERIOD_FORECAST 600000

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif
uint8_t temprature_sens_read();

const char *ssid = "";
const char *password = "";

String api_key = "";
String latitude = "";
String longitude = "";
String units = "metric";
String language = "en";

volatile int screen = 0;
uint32_t sensorTimer = 0;
uint32_t millisAtStart = 0;
uint32_t sensorPeriod = DEFAULT_SENSOR_PERIOD;

const float MAX_OFFSET = 5.05;
const unsigned long CORRECTION_TIME = 600000UL;

DFRobot_SHT20 sht20;
GyverOLED<SSH1106_128x64> oled;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 10800, 60000);
OW_Weather ow;

String tempHome, humidityHome;
OW_forecast *globalForecast = nullptr;
// Мьютекс для защиты доступа к globalForecast
SemaphoreHandle_t forecastMutex = NULL;

int lastUpdatedDay = -1;

unsigned long lastButtonPressTime = 0;
bool isDimmed = false;
const unsigned long BRIGHTNESS_TIMEOUT = 60000;
float esp32Temp;

float temperatureCorrection() {
  unsigned long elapsed = millis() - millisAtStart;
  return (elapsed < CORRECTION_TIME) ? (float)elapsed / CORRECTION_TIME * MAX_OFFSET : MAX_OFFSET;
}

static void onButtonSingleClickCbRight(void *b, void *u) {
  screen = (screen + 1) % 4;
  // Сброс яркости экрана
  lastButtonPressTime = millis();
  if (isDimmed) {
    oled.setContrast(255);  // максимальная яркость
    isDimmed = false;
  }
}
static void onButtonSingleClickCbLeft(void *b, void *u) {
  screen = (screen - 1 + 4) % 4;
  lastButtonPressTime = millis();
  if (isDimmed) {
    oled.setContrast(255);
    isDimmed = false;
  }
}

static void onButtonHoldCb(void *b, void *u) {
  sensorPeriod = HOLD_SENSOR_PERIOD;
}

static void onButtonReleaseCb(void *b, void *u) {
  sensorPeriod = DEFAULT_SENSOR_PERIOD;
}

// Функция для сброса таймера и установки максимальной яркости
void resetBrightness() {
  lastButtonPressTime = millis();
  if (isDimmed) {
    oled.setContrast(255);  // максимальная яркость
    isDimmed = false;
  }
}

// Задача для получения прогноза погоды (защита через мьютекс)
void fetchForecastTask(void *parameter) {
  // Ожидаем инициализацию
  delay(5000);
  for (;;) {
    OW_forecast *newForecast = new OW_forecast;
    // Запрашиваем прогноз
    ow.getForecast(newForecast, api_key, latitude, longitude, units, language);
    
    // Блокируем доступ к globalForecast
    if (xSemaphoreTake(forecastMutex, portMAX_DELAY) == pdTRUE) {
      if (globalForecast) {
        delete globalForecast;
      }
      globalForecast = newForecast;
      xSemaphoreGive(forecastMutex);
    }
    
    vTaskDelay(PERIOD_FORECAST / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  millisAtStart = millis();

  // Инициализация мьютекса
  forecastMutex = xSemaphoreCreateMutex();
  if (forecastMutex == NULL) {
    Serial.println("Ошибка создания мьютекса!");
    while (true)
      ; // Останавливаем работу
  }

  sht20.initSHT20();
  delay(100);
  sht20.checkSHT20();

  oled.init();
  oled.setScale(2);
  oled.autoPrintln(true);
  oled.setContrast(255);
  oled.clear();
  oled.home();
  oled.print("Weather station");
  oled.setCursor(0, 2);
  oled.print("Connecting");
  oled.setCursor(0, 4);
  oled.print(ssid);
  oled.update();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    oled.clear();
    oled.home();
    oled.print("Connecting to WiFi...");
    oled.update();
    Serial.println("Connecting...");
    delay(100);
  }

  oled.clear();
  oled.home();
  oled.print("Connected to WiFi");
  oled.setCursor(0, 2);
  oled.print(WiFi.localIP());
  oled.update();

  timeClient.begin();
  while (!timeClient.isTimeSet()) {
    timeClient.forceUpdate();
  }
  lastUpdatedDay = timeClient.getDay();

  ArduinoOTA.setHostname("Weather_Station");
  ArduinoOTA.setPassword("He89!on67@");
  ArduinoOTA.onStart([]() {
    Serial.println("OTA update started");
    oled.clear();
    oled.home();
    oled.print("OTA update...");
    oled.update();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA update finished");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("OTA Progress: %u%%\r\n", (p / (t / 100)));
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("OTA Error[%u]: ", err);
    if (err == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (err == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (err == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (err == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (err == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  delay(1000);

  // Инициализируем кнопки через динамическое выделение памяти (одноразовое выделение)
  Button *btnRight = new Button(GPIO_NUM_33, false);
  Button *btnLeft = new Button(GPIO_NUM_32, false);
  btnRight->attachSingleClickEventCb(&onButtonSingleClickCbRight, NULL);
  btnLeft->attachSingleClickEventCb(&onButtonSingleClickCbLeft, NULL);
  btnRight->attachLongPressHoldEventCb(&onButtonHoldCb, NULL);
  btnRight->attachLongPressUpEventCb(&onButtonReleaseCb, NULL);

  {  // Однократное получение показаний температуры при старте
    float tempVal = sht20.readTemperature();
    float offset = temperatureCorrection();
    tempHome = String(tempVal - offset, 2);
  }
  humidityHome = sht20.readHumidity();

  // Создаём задачу для обновления прогноза на ядре 1
  xTaskCreatePinnedToCore(fetchForecastTask, "ForecastTask", 8192, NULL, 1, NULL, 1);
  oled.autoPrintln(false);
}

void loop() {
  ArduinoOTA.handle();

  int currentDay = timeClient.getDay();
  if (timeClient.getHours() == 0 && timeClient.getMinutes() == 0 && lastUpdatedDay != currentDay) {
    Serial.println("Midnight, updating time from NTP...");
    timeClient.update();
    lastUpdatedDay = currentDay;
  }

  if ((millis() - lastButtonPressTime >= BRIGHTNESS_TIMEOUT) && !isDimmed) {
    oled.setContrast(10);  // минимальная яркость
    isDimmed = true;
  }

  if (millis() - sensorTimer >= sensorPeriod) {
    sensorTimer = millis();
    float tempVal = sht20.readTemperature();
    float offset = temperatureCorrection();
    tempHome = String(tempVal - offset, 2);
    humidityHome = sht20.readHumidity();

    Serial.print("Raw Temp: ");
    Serial.print(tempVal);
    Serial.print(" C, Offset: ");
    Serial.print(offset);
    Serial.print(" C, Corrected: ");
    Serial.print(tempHome);
    Serial.println(" C");
  }

  oled.clear();
  oled.home();
  switch (screen) {
    case 0:
      oled.print("Kitchen");
      oled.setCursor(0, 2);
      oled.print("Temp: ");
      oled.print(tempHome);
      oled.setCursor(0, 4);
      oled.print("Humid:");
      oled.print(humidityHome);
      oled.setCursor(0, 6);
      oled.print(timeClient.getFormattedTime());
      break;
    case 1: {
      // Защищаем доступ к forecast через мьютекс
      if (xSemaphoreTake(forecastMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
        if (!globalForecast) {
          oled.autoPrintln(true);
          oled.print("Forecast loading...");
          oled.autoPrintln(false);
        } else {
          oled.print("SBP");
          oled.setCursor(0, 2);
          oled.print("Temp: ");
          oled.print(globalForecast->temp[0]);
          oled.setCursor(0, 4);
          oled.print("Press: ");
          oled.print(globalForecast->pressure[0] * 0.75);
          oled.setCursor(0, 6);
          oled.print("Humid: ");
          oled.print(globalForecast->humidity[0]);
        }
        xSemaphoreGive(forecastMutex);
      }
      break;
    }
    case 2: {
      if (xSemaphoreTake(forecastMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
        if (!globalForecast) {
          oled.autoPrintln(true);
          oled.print("Forecast loading...");
          oled.autoPrintln(false);
        } else {
          oled.print("Rain: ");
          oled.print(globalForecast->pop[0]);
          oled.print("%");
          oled.setCursor(0, 2);
          oled.print("Vis: ");
          oled.print(globalForecast->visibility[0]);
          oled.setCursor(0, 4);
          oled.print("Clouds: ");
          oled.print(globalForecast->clouds_all[0]);
        }
        xSemaphoreGive(forecastMutex);
      }
      break;
    }
    case 3: {
      if (xSemaphoreTake(forecastMutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
        if (!globalForecast) {
          oled.autoPrintln(true);
          oled.print("Forecast loading...");
          oled.autoPrintln(false);
        } else {
          oled.print("Next hour");
          oled.setCursor(0, 2);
          oled.print("Temp: ");
          oled.print(globalForecast->temp[1]);
          oled.setCursor(0, 4);
          oled.print("Humid: ");
          oled.print(globalForecast->humidity[1]);
          oled.setCursor(0, 6);
          oled.print("Rain: ");
          oled.print(globalForecast->pop[1]);
          oled.print("%");
          oled.setCursor(0, 8);
          oled.print("Press: ");
          oled.print(globalForecast->pressure[1] * 0.75);
        }
        xSemaphoreGive(forecastMutex);
      }
      break;
    }
    default:
      oled.print("Screen err");
  }
  oled.update();
  delay(10);
}
