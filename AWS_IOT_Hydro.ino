// DHT Code: 
//      - https://randomnerdtutorials.com/esp32-dht11-dht22-temperature-humidity-sensor-arduino-ide/
//      - https://www.best-microcontroller-projects.com/dht22.html
//      - http://playground.arduino.cc/main/DHT11Lib
// TDS Code:
//      - https://wiki.keyestudio.com/KS0429_keyestudio_TDS_Meter_V1.0
// Wifi: arduino example code, HelloServer
// AWS IOT: https://aws.amazon.com/blogs/compute/building-an-aws-iot-core-device-using-aws-serverless-and-an-esp32/

// watchdog 
#include <esp_task_wdt.h>

// EEProm for saving data
#include <Preferences.h>

// TDS Sensor
#include <esp_adc_cal.h>

// 18b20 Water Temp
#include <DallasTemperature.h>
#include <OneWire.h>

// arduino-timer
#include <arduino-timer.h>

// DHT22
#include <DHT.h>
#include <DHT_U.h>

#include "secrets.h"
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"

// NTP DateTime
#include <NTPClient.h>
#include <WiFiUdp.h>

// Webserver
#include <WebServer.h>

// watchdog
//3 seconds WDT
#define WDT_TIMEOUT 60

// Web Server
WebServer server(80);

// The MQTT topics that this device should publish/subscribe
#define AWS_IOT_PUBLISH_TOPIC   "esp32/pub"
#define AWS_IOT_SUBSCRIBE_TOPIC "esp32/sub"

WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(256);

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// nutirent pump relay
const int relay = 27;

// sprinkler 
const int sprinkler = 14;

// 18b20 Water Temp
const int oneWireBus = 4;
OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);
// TDS Sensor
const int TdsSensorPin = 35;
esp_adc_cal_characteristics_t *adc_chars = new esp_adc_cal_characteristics_t;
#define REF_VOLTAGE 1142

// DHT22
#define DHTPIN 0
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// EEprom bytes
Preferences preferences;

// arduino-timer
auto timer = timer_create_default();

struct record
{
   float water_ppm;
   float water_ppm_voltage;
   float water_ppm_comp_voltage;
   float water_temp_c;
   float water_temp_f;
   float air_humidity;
   float air_temp_c;
   float air_temp_f;
   float air_dew_point;
   float air_heat_index_c;
   float air_heat_index_f;
   int status_code;
   String status_message;
};
struct record aRec;

void led_on() {
  digitalWrite(LED_BUILTIN, 1);
}

bool led_off (void *) {
  digitalWrite(LED_BUILTIN, 0);
  return false;
}

bool led_flash(void *) {
  led_on();
  timer.every(500, led_off);
  return true;
}

void ledBurst(int x, int waitTime) {
  for (int i = 0; i < x; i++) {
    digitalWrite(LED_BUILTIN, 1);
    delay(waitTime);
    digitalWrite(LED_BUILTIN, 0);
    delay(waitTime);
  }
}

void errorSOS() {
  ledBurst(3, 100);
  ledBurst(3, 50);
  ledBurst(3, 100);
}

// TDS Sensor
// ========= analogRead_cal =========
float analogRead_cal() {
//  uint8_t channel = 34;
  adc1_channel_t channelNum = ADC1_CHANNEL_6;

  // Set number of cycles per sample. Default is 8 and seems to do well, Range is 1 - 255
  // analogSetCycles(255);

  /*
     Set number of samples in the range.
     Default is 1
     Range is 1 - 255
     This setting splits the range into
     "samples" pieces, which could look
     like the sensitivity has been multiplied
     that many times
   * */
//   analogSetSamples(255);

  
  adc1_config_channel_atten(channelNum, ADC_ATTEN_DB_11);
  float raw = analogRead(TdsSensorPin);
  float cal_mv = esp_adc_cal_raw_to_voltage(raw, adc_chars);
//  Serial.printf("raw: %f calibrated v: %.3f mV\n", raw, cal_mv);  
  return cal_mv;
}


void update_sensors() {

  // water temp  
  sensors.requestTemperatures();
  aRec.water_temp_c = sensors.getTempCByIndex(0);
  aRec.water_temp_f = sensors.getTempFByIndex(0);

  // dh22
  if(!read_dht22()) {
    errorSOS();
  } else {
    aRec.status_code = 0; // 1=error
    aRec.status_message = "All good!";
  }

  upateTDS_PPM();
  printMessage();
//  return true; // keep timer active? true
}

bool read_dht22() {
  digitalWrite(LED_BUILTIN, 1);
  // Reading temperature or humidity takes about 250 milliseconds!
  // Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor)
  float h = dht.readHumidity();
  // Read temperature as Celsius (the default)
  float t = dht.readTemperature();
  // Read temperature as Fahrenheit (isFahrenheit = true)
  float f = dht.readTemperature(true);
  // Check if any reads failed and exit early (to try again).
  if (isnan(h) || isnan(t) || isnan(f)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    aRec.air_humidity = 0;
    aRec.air_temp_c = 0;
    aRec.air_temp_f = 0;
    aRec.air_heat_index_f = 0;
    aRec.air_heat_index_c = 0;
    aRec.air_dew_point = 0;
    aRec.status_code = 1; // 1=error
    aRec.status_message = "Failed to read from DHT sensor!";
    return false;
  }
  // Compute heat index in Fahrenheit (the default)
  float hif = dht.computeHeatIndex(f, h);
  // Compute heat index in Celsius (isFahreheit = false)
  float hic = dht.computeHeatIndex(t, h, false);
  float dp = dewPoint(t, h);
  
  aRec.air_humidity = h;
  aRec.air_temp_c = t;
  aRec.air_temp_f = f;
  aRec.air_heat_index_f = hif;
  aRec.air_heat_index_c = hic;
  aRec.air_dew_point = dp;
  digitalWrite(LED_BUILTIN, 0);
  return true;
}

String getMessage() {
  String message = "\n";
  message += "uptime: " + String(millis() / 1000)  + "\n";
  message += "water_ppm: " + String(aRec.water_ppm) + "\n";
  message += "water_ppm_voltage: " + String(aRec.water_ppm_voltage) + "\n";
  message += "water_ppm_comp_voltage: " + String(aRec.water_ppm_comp_voltage) + "\n";
  message += "water_temp_c: " + String(aRec.water_temp_c) + "\n";
  message += "water_temp_f: " + String(aRec.water_temp_f) + "\n";
  message += "air_humidity: " + String(aRec.air_humidity) + "\n";
  message += "air_temp_c: " + String(aRec.air_temp_c) + "\n";
  message += "air_temp_f: " + String(aRec.air_temp_f) + "\n";
  message += "air_dew_point: " + String(aRec.air_dew_point) + "\n";
  message += "air_heat_index_c: " + String(aRec.air_heat_index_c) + "\n";
  message += "air_heat_index_f: " + String(aRec.air_heat_index_f) + "\n";
  message += "status_code: " + String(aRec.status_code) + "\n";
  message += "status_message: " + aRec.status_message + "\n";
  message += "wifi_status: " + String(WiFi.status()) + "\n";
  message += "aws_connected: " + String(client.connected())  + "\n";
  message += "time: " + String(timeClient.getFormattedTime()) + "\n";
  unsigned int sprinkler_run = preferences.getUInt("sprinkler_run", 600);
  message += "sprinkler_runtime : " + String(sprinkler_run) + "\n";
  unsigned int sprinkler_hour = preferences.getUInt("sprinkler_hour", 6);
  unsigned int sprinkler_min = preferences.getUInt("sprinkler_min", 30);
  message += "sprinkler_run_at : " + String(sprinkler_hour) + ":" + String(sprinkler_min) + "\n";
  message += "\n";
  return message;
}

void printMessage() {
  String message = getMessage();
  Serial.print("Sensor Data: " + message + "\n");
}

double dewPoint(double celsius, double humidity)
{
  // (1) Saturation Vapor Pressure = ESGG(T)
  double RATIO = 373.15 / (273.15 + celsius);
  double RHS = -7.90298 * (RATIO - 1);
  RHS += 5.02808 * log10(RATIO);
  RHS += -1.3816e-7 * (pow(10, (11.344 * (1 - 1 / RATIO ))) - 1) ;
  RHS += 8.1328e-3 * (pow(10, (-3.49149 * (RATIO - 1))) - 1) ;
  RHS += log10(1013.246);

  // factor -3 is to adjust units - Vapor Pressure SVP * humidity
  double VP = pow(10, RHS - 3) * humidity;

  // (2) DEWPOINT = F(Vapor Pressure)
  double T = log(VP / 0.61078); // temp var
  return (241.88 * T) / (17.558 - T);
}

void upateTDS_PPM() {
  float averageVoltage = analogRead_cal() / 1000;
  float tempDiff = aRec.water_temp_c - 25.0;
  float compensationCoefficient = 1.0 + 0.02 * tempDiff; //temperature compensation formula: fFinalResult(25^C) = fFinalResult(current)/(1.0+0.02*(fTP-25.0));
  float compensationVoltage = averageVoltage / compensationCoefficient; //temperature compensation
  float tds1 = 133.42 * compensationVoltage * compensationVoltage * compensationVoltage;
  float tds2 = 255.86 * compensationVoltage * compensationVoltage;
  float tds3 = 857.39 * compensationVoltage;
  float tdsV = (tds1 - tds2 + tds3) / 2;
//  Serial.println("tdsV: " + String(tdsV) + "PPM");
  aRec.water_ppm = tdsV;
  aRec.water_ppm_voltage = averageVoltage;
  aRec.water_ppm_comp_voltage = compensationVoltage;
}

bool connectAWS(void *)
{
  
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("Wifi not connected, trying to connect...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    unsigned int restart_count = 0;
    while (WiFi.status() != WL_CONNECTED){
      delay(500);
      restart_count++;
      Serial.print(".");
      if(restart_count > 30) {    
        Serial.println("Unable to connect to wifi, restarting...");
        ledBurst(10, 200);
        ESP.restart();
      }
    }
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(WIFI_SSID);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }

  if(!client.connected()){
    net.setCACert(AWS_CERT_CA);
    net.setCertificate(AWS_CERT_CRT);
    net.setPrivateKey(AWS_CERT_PRIVATE);
  
    // Connect to the MQTT broker on the AWS endpoint we defined earlier
    client.begin(AWS_IOT_ENDPOINT, 8883, net);
  
    // Create a message handler
    client.onMessage(messageHandler);
  
    Serial.print("Connecting to AWS IOT");
  
    unsigned int aws_restart_count = 0;
    while (!client.connect(THINGNAME)) {
      aws_restart_count++;
      Serial.print(".");
      delay(100);
      if(aws_restart_count > 30) { 
        Serial.println("Unable to connect to AWS IOT, restarting...");
        ESP.restart();
      }
    }
    
    if(!client.connected()){
      Serial.println("AWS IoT Timeout!");
      ledBurst(25, 100);
      Serial.println("Unable to connect to AWS IOT, restarting...");
      ledBurst(20, 200);
      ESP.restart();
      return true; // timer retry
    }
    
    // Subscribe to a topic
    client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);
    Serial.println("AWS IoT Connected!");
  }  
  return true;
}

void publishMessage()
{
  StaticJsonDocument<200> doc;
//  doc["time"] = millis();
  doc["water_ppm"] = aRec.water_ppm;
  doc["water_temp_c"] = aRec.water_temp_c;
//  doc["water_temp_f"] = aRec.water_temp_f;
  doc["air_humidity"] = aRec.air_humidity;
  doc["air_temp_c"] = aRec.air_temp_c;
//  doc["air_temp_f"] = aRec.air_temp_f;
  doc["air_dew_point"] = aRec.air_dew_point;
  doc["air_heat_index_c"] = aRec.air_heat_index_c;
//  doc["nutrient_pump"] = 0;
//  doc["air_heat_index_f"] = aRec.air_heat_index_f;
//  doc["sensor_a0"] = analogRead(0);

  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer); // print to client
//  Serial.println("Published:" + String(jsonBuffer));

  client.publish(AWS_IOT_PUBLISH_TOPIC, jsonBuffer);
}

void messageHandler(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);
//  Serial.println("incoming: " + topic + " - " + payload);

//  StaticJsonDocument<200> doc;
//  deserializeJson(doc, payload);
////  Serial.println("doc:" + String(doc));
//  const char* message = doc["message"];
//  
//  Serial.println("message:" + String(message));
}

bool CheckFixPPM(void *) {
  Serial.println("CheckFixPPM: aRec.water_ppm:" + String(aRec.water_ppm));
  unsigned int ppm_target = preferences.getUInt("ppm_target", 1000);
  Serial.println("PPM Target: " + String(ppm_target));
  if(aRec.water_ppm < ppm_target) {
    Serial.println("Pump on: " + String(millis()));
    digitalWrite(relay, HIGH);
    delay(10000);
    Serial.println("Pump off: " + String(millis()));
    digitalWrite(relay, LOW);
      
    StaticJsonDocument<200> doc;
    doc["nutrient_pump"] = 1;
    char jsonBuffer[512];
    serializeJson(doc, jsonBuffer); // print to client
    client.publish(AWS_IOT_PUBLISH_TOPIC, jsonBuffer);
    client.loop();
  } else  {
    Serial.println("Levels ok, target: " + String(ppm_target) + "PPM, skip pump");
  }
  
  ledBurst(2, 100);
  return true;
}


bool do_work(void *) {
  update_sensors();
  publishMessage();
  client.loop();
  ledBurst(2, 100);
  return true;
}
bool clientloop(void *) {
  client.loop();
  return true;
}

bool sprinkler_off(void *) {
  Serial.println("sprinkler off: " + String(millis()));
  digitalWrite(sprinkler, LOW);
  return false;
}

bool sprinkler_check(void *) {
  unsigned int sprinkler_run = preferences.getUInt("sprinkler_run", 600);
  Serial.println("sprinkler_run : " + String(sprinkler_run));
  unsigned int sprinkler_hour = preferences.getUInt("sprinkler_hour", 6);
  unsigned int sprinkler_min = preferences.getUInt("sprinkler_min", 30);
  Serial.println("sprinkler_hour : " + String(sprinkler_hour));
  Serial.println("sprinkler_min : " + String(sprinkler_min));

  int hour = timeClient.getHours();
  int mint = timeClient.getMinutes();
  int day  = timeClient.getDay();
  Serial.println("Day : " + String(day));
  Serial.println("Hour: " + String(hour));
  Serial.println("Mint: " + String(mint));
  if(hour == sprinkler_hour && mint == sprinkler_min) {
    Serial.println("its time to run the sprinkler");
      Serial.println("sprinkler on: " + String(millis()));
      
      Serial.println("will run for: " + String(sprinkler_run) + " seconds");
      digitalWrite(sprinkler, HIGH);
//      delay(sprinkler_run);
      timer.every(sprinkler_run * 1000, sprinkler_off);
  } else {
    Serial.println("its NOT time to run the sprinkler");
  }
  Serial.println("end of sprinker_check");
  return true;
}


void handleRoot() {
  String message = getMessage();
  Serial.print("Sensor Data: " + message + "\n");
  server.send(200, "text/plain", message);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}


void setup() {
  Serial.begin(115200);
  
  // Preferences
  preferences.begin("my-app", false);
//  unsigned int counter = preferences.getUInt("counter", 0);
  
  // watchdog
  esp_task_wdt_init(WDT_TIMEOUT, true); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL); //add current thread to WDT watch


//  Serial.println("sprinkler_run: " + String(sprinkler_run));
  Serial.println("-- Startup -- ¯\\_(ツ)_/¯");
  Serial.println("LED_BUILTIN: " + String(LED_BUILTIN));
  pinMode(LED_BUILTIN, OUTPUT);
  connectAWS(false);
  timeClient.begin();
  // Set offset time in seconds to adjust for your timezone, for example:
  // GMT +1 = 3600
  // GMT +8 = 28800
  // GMT -1 = -3600
  // GMT -7 = 3600 * 7 = 25200
  timeClient.setTimeOffset(-25200);
  
  //water temp
  sensors.begin();
  
  // relay pump
  pinMode(relay, OUTPUT);
  digitalWrite(relay, LOW); // turn off pump

  pinMode(sprinkler, OUTPUT);
  digitalWrite(sprinkler, LOW); // turn off sprinkler

  // DHT22 
  dht.begin();

  analogReadResolution(12);
  esp_adc_cal_value_t val_type =
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, REF_VOLTAGE, adc_chars);

  

  server.on("/", handleRoot);
//  server.on("/status", handleStatus);

//  server.on("/inline", []() {
//    server.send(200, "text/plain", "this works as well");
//  });

  server.onNotFound(handleNotFound);

  server.begin();
  
  timer.every(30000, do_work);
  timer.every(180000, CheckFixPPM);
  timer.every(1000, clientloop);
  timer.every(60000, sprinkler_check);
  timer.every(300000, connectAWS);
  timer.every(2000, led_flash);
}

void loop() {
//  client.loop();
  
  server.handleClient();
  while(!timeClient.update()) {
    timeClient.forceUpdate();
  }
  timer.tick(); 
  esp_task_wdt_reset();

//  delay(1000);
}
