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

#define DEFAULT_SENSOR_PERIOD 60000       // 60 секунд
#define HOLD_SENSOR_PERIOD     1000         // 1 секунда
#define PERIOD_FORECAST        600000       // 10 минут

const char *ssid = "";
const char *password = "";

String api_key   = "";
String latitude  = "59.57";     
String longitude = "30.19";    
String units     = "metric";       
String language  = "en";        

volatile int screen = 0;
uint32_t sensorTimer = 0;
uint32_t millisAtStart = 0;
uint32_t sensorPeriod = DEFAULT_SENSOR_PERIOD;  // текущий период опроса сенсоров

// Для коррекции нагрева датчика
const float MAX_OFFSET = 5.53;                   // Максимальное смещение (°C)
const unsigned long CORRECTION_TIME = 1800000UL; // 30 минут (мс)

DFRobot_SHT20 sht20;
GyverOLED<SSH1106_128x64> oled;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 10800, 60000);
OW_Weather ow;

String tempHome, humidityHome;
volatile OW_forecast *globalForecast = nullptr;

// Для обновления времени в 00:00 (сохраняем последний день недели)
int lastUpdatedDay = -1;

float temperatureCorrection() {
  unsigned long elapsed = millis() - millisAtStart;
  return (elapsed < CORRECTION_TIME) ? (float)elapsed / CORRECTION_TIME * MAX_OFFSET : MAX_OFFSET;
}

// Обработчик одиночного нажатия (переключение экранов)
static void onButtonSingleClickCb(void *b, void *u) {
  screen = (screen + 1) % 4;
}

// Обработчик удержания кнопки – включаем режим быстрого обновления датчиков
static void onButtonHoldCb(void *b, void *u) {
  sensorPeriod = HOLD_SENSOR_PERIOD;
}

// Обработчик отпускания кнопки – сбрасываем период опроса датчиков
static void onButtonReleaseCb(void *b, void *u) {
  sensorPeriod = DEFAULT_SENSOR_PERIOD;
}

void fetchForecastTask(void *parameter) {
  delay(5000);
  for (;;) {
    OW_forecast *newForecast = new OW_forecast;
    ow.getForecast(newForecast, api_key, latitude, longitude, units, language);
    if (globalForecast) delete globalForecast;
    globalForecast = newForecast;
    vTaskDelay(PERIOD_FORECAST / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  millisAtStart = millis();
  
  sht20.initSHT20();
  delay(100);
  sht20.checkSHT20();

  oled.init();
  oled.setScale(2);
  oled.clear();
  oled.home();
  oled.print("Weather station");
  oled.setCursor(0,2);
  oled.print("Connecting");
  oled.setCursor(0,4);
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
  oled.setCursor(0,2);
  oled.print(WiFi.localIP());
  oled.update();

  // Подключение NTP – здесь выполняется первоначальное обновление времени.
  timeClient.begin();
  timeClient.update();
  lastUpdatedDay = timeClient.getDay();

  ArduinoOTA.setHostname("Weather_Station");
  ArduinoOTA.setPassword("");
  ArduinoOTA.onStart([](){
    Serial.println("OTA update started");
    oled.clear();
    oled.home();
    oled.print("OTA update...");
    oled.update();
  });
  ArduinoOTA.onEnd([](){ Serial.println("OTA update finished"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t){
    Serial.printf("OTA Progress: %u%%\r\n", (p / (t / 100)));
  });
  ArduinoOTA.onError([](ota_error_t err){
    Serial.printf("OTA Error[%u]: ", err);
    if(err==OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if(err==OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if(err==OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if(err==OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if(err==OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  delay(1000);

  // Инициализация кнопки с назначением событий:
  Button *btnRight = new Button(GPIO_NUM_33, false);
  btnRight->attachSingleClickEventCb(&onButtonSingleClickCb, NULL);
  btnRight->attachDuringHoldEventCb(&onButtonHoldCb, NULL);
  btnRight->attachReleaseEventCb(&onButtonReleaseCb, NULL);

  // Первичное считывание с коррекцией
  {
    float tempVal = sht20.readTemperature().toFloat();
    float offset = temperatureCorrection();
    tempHome = String(tempVal - offset, 2);
  }
  humidityHome = sht20.readHumidity();

  xTaskCreatePinnedToCore(fetchForecastTask, "ForecastTask", 8192, NULL, 1, NULL, 1);
}

void loop() {
  ArduinoOTA.handle();

  // Раз в цикл проверяем, не наступило ли 00:00 
  timeClient.update();
  int currentDay = timeClient.getDay();
  if (timeClient.getHours() == 0 && timeClient.getMinutes() == 0 && lastUpdatedDay != currentDay) {
    Serial.println("Midnight, updating time from NTP...");
    timeClient.update();
    lastUpdatedDay = currentDay;
  }

  // Обновляем данные с датчиков согласно актуальному sensorPeriod
  if (millis() - sensorTimer >= sensorPeriod) {
    sensorTimer = millis();
    float tempVal = sht20.readTemperature().toFloat();
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

  // Отображение на дисплее
  oled.clear();
  oled.home();
  switch (screen) {
    case 0:
      oled.print("Kitchen");
      oled.setCursor(0,2); oled.print("Temp: "); oled.print(tempHome);
      oled.setCursor(0,4); oled.print("Humid: "); oled.print(humidityHome);
      oled.setCursor(0,6); oled.print(timeClient.getFormattedTime());
      break;
    case 1:
      if (!globalForecast) { oled.print("Forecast loading..."); }
      else {
        oled.print("Saint Petersburg");
        oled.setCursor(0,2); oled.print("Temp: "); oled.print(globalForecast->temp[0]);
        oled.setCursor(0,4); oled.print("Press: "); oled.print(globalForecast->pressure[0] * 0.75);
        oled.setCursor(0,6); oled.print("Humid: "); oled.print(globalForecast->humidity[0]);
      }
      break;
    case 2:
      if (!globalForecast) { oled.print("Forecast loading..."); }
      else {
        oled.print("Saint Petersburg");
        oled.setCursor(0,2); oled.print("Wind: "); oled.print(globalForecast->wind_speed[0]);
        oled.setCursor(0,4); oled.print("Clouds: "); oled.print(globalForecast->clouds_all[0]);
      }
      break;
    case 3:
      if (!globalForecast) { oled.print("Forecast loading..."); }
      else {
        oled.print("Next 3h Forecast");
        oled.setCursor(0,2); oled.print("Temp: "); oled.print(globalForecast->temp[0]);
        oled.setCursor(0,4); oled.print("Humid: "); oled.print(globalForecast->humidity[0]);
        oled.setCursor(0,6); 
        oled.print("Rain: ");
        float pop = (globalForecast->pop[0] != 0) ? globalForecast->pop[0] : float(globalForecast->clouds_all[0]) / 100.0;
        oled.print(pop * 100, 0);
        oled.print("%");
      }
      break;
    default:
      oled.print("Screen err");
  }
  oled.update();

  delay(10);
}
