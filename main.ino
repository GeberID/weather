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
#include <ArduinoOTA.h>  // OTA обновление

// Настройки периодичности обновления данных
#define PERIOD_SENSOR 60000       // обновление показаний с датчика (60 секунд)
#define PERIOD_FORECAST 600000      // период обновления прогноза (600 секунд = 10 минут)

// WiFi настройки
const char *ssid = "";
const char *password = "";

// OpenWeather API настройки
String api_key   = "";
String latitude  = "59.57";   // широта
String longitude = "30.19";   // долгота
String units     = "metric";  // "metric" для °C, "imperial" для °F
String language  = "en";      // язык ответов

// Глобальная переменная для переключения экранов:
// 0 – домашние датчики,
// 1 – прогноз погоды (основной, температура, давление и влажность),
// 2 – расширенный прогноз (ветер, облачность),
// 3 – прогноз на ближайшие 3 часа (температура, влажность и вероятность осадков)
volatile int screen = 0;

// Таймер для обновления сенсорных данных
uint32_t sensorTimer = 0;

// Сохраним время старта устройства (в миллисекундах)
uint32_t millisAtStart = 0;

// Параметры коррекции нагрева датчика
const float MAX_OFFSET = 5.53;    // максимальное смещение (°C), достигнуто через 30 минут
const unsigned long CORRECION_TIME = 1800UL * 1000;  // 30 минут в миллисекундах

// Объекты библиотек
DFRobot_SHT20 sht20;
GyverOLED<SSH1106_128x64> oled;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 10800, 60000);
OW_Weather ow;  // работа с OpenWeather

// Глобальные переменные для хранения данных локального датчика
String tempHome;
String humidityHome;

// Глобальная переменная для хранения прогноза погоды.
volatile OW_forecast *globalForecast = nullptr;

// Функция расчета коррекции нагрева для датчика
float temperatureCorrection() {
  // Определяем время работы устройства в мс
  unsigned long elapsed = millis() - millisAtStart;
  
  // Если прошло меньше CORRECION_TIME, линейный рост смещения, иначе максимальное смещение
  float offset;
  if (elapsed < CORRECION_TIME) {
    offset = (float)elapsed / CORRECION_TIME * MAX_OFFSET;
  } else {
    offset = MAX_OFFSET;
  }
  return offset;
}

// Обработчик одиночного нажатия кнопки для переключения экранов
static void onButtonSingleClickCb(void *button_handle, void *usr_data) {
  screen = (screen + 1) % 4;
}

// Задача для получения прогноза погоды (блокирующие вызовы происходят в отдельном потоке)
void fetchForecastTask(void *parameter) {
  delay(5000); // Ждем, чтобы система и WiFi успели установиться

  for (;;) {
    OW_forecast *newForecast = new OW_forecast;
    // Получение прогноза (блокирующий вызов)
    ow.getForecast(newForecast, api_key, latitude, longitude, units, language);

    // Обновление глобального указателя с прогнозом.
    if (globalForecast != nullptr) {
      delete globalForecast;
    }
    globalForecast = newForecast;

    // Ждем до следующего обновления прогноза
    vTaskDelay(PERIOD_FORECAST / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Сохраняем время старта для вычисления коррекции нагрева
  millisAtStart = millis();

  // Инициализация датчика температуры и влажности
  sht20.initSHT20();
  delay(100);
  sht20.checkSHT20();

  // Инициализация дисплея
  oled.init();
  oled.setScale(2);
  oled.clear();
  oled.home();
  oled.print("Weather station");
  oled.setCursor(0, 2);
  oled.print("Connecting");
  oled.setCursor(0, 4);
  oled.print(ssid);
  oled.update();

  // Подключение к WiFi
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

  // Инициализация NTP клиента для получения времени
  timeClient.begin();

  // Инициализация OTA
  ArduinoOTA.setHostname("Weather_Station");
  ArduinoOTA.setPassword("");

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
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r\n", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");

  delay(1000); // Небольшая задержка для стабильного запуска OTA

  // Инициализация кнопки для переключения экранов
  Button *btnRight = new Button(GPIO_NUM_33, false);
  btnRight->attachSingleClickEventCb(&onButtonSingleClickCb, NULL);

  // Первичное считывание данных с локальных датчиков с учетом начальной коррекции
  {
    String rawTemp = sht20.readTemperature();
    float tempVal = rawTemp.toFloat();
    float offset = temperatureCorrection();
    float correctedTemp = tempVal - offset;
    // Преобразуем обратно в строку для удобного вывода
    tempHome = String(correctedTemp, 2);
  }
  humidityHome = sht20.readHumidity();

  // Создание задачи для получения прогноза погоды
  xTaskCreatePinnedToCore(
    fetchForecastTask,   // функция задачи
    "ForecastTask",      // название задачи
    8192,                // размер стека
    NULL,                // параметры задачи
    1,                   // приоритет
    NULL,                // дескриптор задачи
    1                    // привязка к ядру (при необходимости можно изменить)
  );
}

void loop() {
  ArduinoOTA.handle();

  // Обновляем время по NTP
  timeClient.update();

  // Обновление локальных датчиков каждые PERIOD_SENSOR мс
  if (millis() - sensorTimer >= PERIOD_SENSOR) {
    sensorTimer = millis();
    // Чтение температуры и применение коррекции нагрева.
    String rawTemp = sht20.readTemperature();
    float tempVal = rawTemp.toFloat();
    float offset = temperatureCorrection();
    float correctedTemp = tempVal - offset;
    tempHome = String(correctedTemp, 2);

    humidityHome = sht20.readHumidity();

    // Вывод отладочной информации в Serial
    Serial.print("Raw Temp: ");
    Serial.print(tempVal);
    Serial.print(" C, Offset: ");
    Serial.print(offset);
    Serial.print(" C, Corrected Temp: ");
    Serial.print(correctedTemp);
    Serial.println(" C");
  }

  // Подготовка дисплея к обновлению экрана
  oled.clear();
  oled.home();

  switch (screen) {
    case 0: {  // Экран локальных датчиков
      oled.print("Kitchen");
      oled.setCursor(0, 2);
      oled.print("Temp: ");
      oled.print(tempHome);
      oled.setCursor(0, 4);
      oled.print("Humid: ");
      oled.print(humidityHome);
      oled.setCursor(0, 6);
      oled.print(timeClient.getFormattedTime());
      break;
    }
    case 1: {  // Экран прогноза (основной)
      if (globalForecast == nullptr) {
        oled.print("Forecast loading...");
      } else {
        oled.print("Saint Petersburg");
        oled.setCursor(0, 2);
        oled.print("Temp: ");
        oled.print(globalForecast->temp[0]);
        oled.setCursor(0, 4);
        oled.print("Press: ");
        oled.print(globalForecast->pressure[0] * 0.75);  // давление в мм рт.ст.
        oled.setCursor(0, 6);
        oled.print("Humid: ");
        oled.print(globalForecast->humidity[0]);
      }
      break;
    }
    case 2: {  // Расширенный прогноз
      if (globalForecast == nullptr) {
        oled.print("Forecast loading...");
      } else {
        oled.print("Saint Petersburg");
        oled.setCursor(0, 2);
        oled.print("Wind: ");
        oled.print(globalForecast->wind_speed[0]);
        oled.setCursor(0, 4);
        oled.print("Clouds: ");
        oled.print(globalForecast->clouds_all[0]);
      }
      break;
    }
    case 3: {  // Прогноз на ближайшие 3 часа
      if (globalForecast == nullptr) {
        oled.print("Forecast loading...");
      } else {
        oled.print("Next 3h Forecast");
        oled.setCursor(0, 2);
        oled.print("Temp: ");
        oled.print(globalForecast->temp[0]);
        oled.setCursor(0, 4);
        oled.print("Humid: ");
        oled.print(globalForecast->humidity[0]);
        oled.setCursor(0, 6);
        oled.print("Rain: ");
        float pop = (globalForecast->pop[0] != 0 ? globalForecast->pop[0] 
                      : (float)globalForecast->clouds_all[0] / 100.0);
        oled.print(pop * 100, 0);
        oled.print("%");
      }
      break;
    }
    default: {
      oled.print("Screen err");
      break;
    }
  }

  oled.update();

  delay(10);
}
