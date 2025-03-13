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

// Настройки периодичности обновления данных
#define PERIOD_SENSOR   60000   // обновление показаний с датчика (60 секунд)
#define PERIOD_FORECAST 600000  // период обновления прогноза (600 секунд = 10 минут)

// WiFi настройки
const char *ssid = "";
const char *password = "";

// OpenWeather API настройки
String api_key   = "";
String latitude  = "";   // широта
String longitude = "";   // долгота
String units     = "metric";  // "metric" для °C, "imperial" для °F
String language  = "en";      // язык ответов

// Глобальная переменная для переключения экрана:
// 0 – домашние датчики, 1 – прогноз (основной), 2 – прогноз (расширенный)
volatile int screen = 0;

// Таймер для датчиков
uint32_t sensorTimer = 0;

// Объекты библиотек
DFRobot_SHT20 sht20;
GyverOLED<SSH1106_128x64> oled;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 10800, 60000);
OW_Weather ow;  // объект для работы с OpenWeather

// Глобальные переменные для хранения данных домашнего датчика
String tempHome;
String humidityHome;

// Глобальная переменная для хранения данных прогноза погоды.
// Для упрощения динамического выделения памяти будем использовать указатель,
// который обновляется в отдельной задаче.
volatile OW_forecast *globalForecast = nullptr;

// Для защиты доступа можно использовать мьютекс (но в простом примере это не критично).

// Обработчик одиночного нажатия кнопки для переключения экранов.
// Используем операцию по модулю для цикличного переключения.
static void onButtonSingleClickCb(void *button_handle, void *usr_data) {
  screen = (screen + 1) % 3;
}

// Задача для получения прогноза погоды (работает в отдельном потоке)
void fetchForecastTask(void * parameter) {
  // Задержка перед первым запросом, чтобы WiFi и NTP успели установиться
  delay(5000);
  
  for (;;) {
    OW_forecast *newForecast = new OW_forecast;
    // Получить прогноз. Функция getForecast является блокирующей,
    // но поскольку она вызывается в отдельной задаче, основной цикл не "замерзнет".
    ow.getForecast(newForecast, api_key, latitude, longitude, units, language);
    
    // Обновляем глобальный указатель.
    // Освобождаем старую память, если данные уже были получены.
    if (globalForecast != nullptr) {
      delete globalForecast;
    }
    globalForecast = newForecast;
    
    // Ждем заданное время до следующего обновления.
    // Здесь можно регулировать частоту обновления прогноза.
    vTaskDelay(PERIOD_FORECAST / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Инициализация датчика температуры и влажности
  sht20.initSHT20();
  delay(100);  // время на инициализацию
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

  // Инициализация кнопки. Обратите внимание, что используем динамическое выделение памяти.
  Button *btnRight = new Button(GPIO_NUM_33, false);
  btnRight->attachSingleClickEventCb(&onButtonSingleClickCb, NULL);

  // Первоначальное считывание данных с локальных датчиков
  tempHome = sht20.readTemperature();
  humidityHome = sht20.readHumidity();

  // Создаем задачу для получения прогноза погоды.
  xTaskCreatePinnedToCore(
    fetchForecastTask,   // функция задачи
    "ForecastTask",      // название задачи
    8192,                // объем памяти (стека) для задачи
    NULL,                // параметр задачи
    1,                   // приоритет
    NULL,                // дескриптор задачи
    1                    // закрепляем за ядром 1 (текущее ядро можно изменить)
  );
}

void loop() {
  // Обновляем NTP время
  timeClient.update();

  // Обновление локальных датчиков каждые PERIOD_SENSOR мс
  if (millis() - sensorTimer >= PERIOD_SENSOR) {
    sensorTimer = millis();
    tempHome = sht20.readTemperature();
    humidityHome = sht20.readHumidity();
  }

  // Отображение экранов в зависимости от значения переменной screen
  oled.clear();
  oled.home();

  switch (screen) {
    case 0: { // Экран с локальными показаниями
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
    case 1: { // Экран прогноза (основной)
      // При отсутствии полученных данных, выводим сообщение
      if (globalForecast == nullptr) {
        oled.print("Forecast loading...");
      } else {
        oled.print("Saint Petersburg");
        oled.setCursor(0, 2);
        oled.print("Temp: ");
        oled.print(globalForecast->temp[0]);
        oled.setCursor(0, 4);
        oled.print("Press: ");
        // Перевод давления из гПа в мм рт.ст. (приблизительно)
        oled.print(globalForecast->pressure[0] * 0.75);
        oled.setCursor(0, 6);
        oled.print("Humid: ");
        oled.print(globalForecast->humidity[0]);
      }
      break;
    }
    case 2: { // Экран прогноза (расширенный)
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
        oled.setCursor(0, 6);
        oled.print("Visibility: ");
        oled.print(globalForecast->visibility[0]);
      }
      break;
    }
    default:
      oled.print("Screen err");
  }

  oled.update();

  // Для отладки вывод в Serial
  Serial.print("Local Temp: ");
  Serial.print(tempHome);
  Serial.println(" C");

  // Короткая задержка для предотвращения перегрузки цикла
  delay(10);
}
