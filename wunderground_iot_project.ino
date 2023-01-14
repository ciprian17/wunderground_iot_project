#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <Wire.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// Used in communication with the BME280 sensor module
#define I2C_SDA 21
#define I2C_SCL 22

/* Conversion factor for micro seconds to seconds */
#define uS_TO_S_FACTOR 1000000
/* Time ESP32 will go to sleep (in seconds) */
#define TIME_TO_SLEEP  7

// Change to your network credentials
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_passwd"

// Insert your Firebase project API Key
#define API_KEY "your_firebase_project_api_key"

// Insert your Firebase RTDB URL */
#define DATABASE_URL "your_firebase_rtdb_URL"

// Change to your Firebase user email and passwd
#define USER_EMAIL "your_user_email"
#define USER_PASSWORD "your_user_passwd"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Function that gets current epoch time
unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return(0);
  }
  time(&now);
  return now;
}

// Initialize WiFi
void initWiFi() {
  uint32_t not_ok_counter = 0;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.println("Wifi connecting...");
    not_ok_counter++;
    // Reset platform if not connected after 20s
    if(not_ok_counter > 200) {
        Serial.println("Resetting board due to Wifi not connecting in 20s");
        ESP.restart();
    }
  }

  Serial.println(WiFi.localIP());
  Serial.println();
}

void FirebaseInit(String *databasePath) {
  const char* ntpServer = "pool.ntp.org";
  String uid;

  configTime(0, 0, ntpServer);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);

  // Assign the callback function for the long running token generation task */
  config.token_status_callback = tokenStatusCallback;

  // Assign the maximum retry of token generation
  config.max_token_generation_retry = 5;

  // Initialize with the Firebase authen and config
  Firebase.begin(&config, &auth);

  // Getting the user UID might take a few seconds
  Serial.println("Getting User UID");
  while ((auth.token.uid) == "") {
    Serial.print('.');
    delay(1000);
  }

  uid = auth.token.uid.c_str();
  Serial.print("User UID: ");
  Serial.println(uid);

  // Update database path
  *databasePath = "/UsersData/" + uid + "/readings";
  Serial.printf("databasePath: ");
  Serial.println(*databasePath);
}

void EnterHibernation() {
  /*
  First we configure the wake up source
  We set our ESP32 to wake up every 7 seconds
  */
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);

  Serial.println("Setup ESP32 to sleep for every " + String(TIME_TO_SLEEP) +
  " Seconds");

  Serial.println("Going to sleep now");
  Serial.flush();

  // force specific powerdown modes for RTC peripherals and RTC memories
  esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);

  // Enter Deep Sleep -- Hibernation
  esp_deep_sleep_start();
}

String constructHttpGET(float temp, float humidity, float pressure) {
  String url = "https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php?ID=INSERT_YOUR_ID&PASSWORD=INSERT_YOUR_PASSWD&dateutc=now";
  float tempf;

  // Convert temperature to Fahrenheit from Celsius
  tempf = (temp * 1.8) + 32;
  url += "&tempf=" + String(tempf);
  url += "&humidity=" + String(humidity);

  pressure *= 0.0296;  // metric to US
  url += "&baromin=" + String(pressure);
  url += "&action=updateraw";

  return url;
}

void wundergroundSendData(float temp, float humidity, float pressure) {
  HTTPClient httpClient;
  String urlSend;

  // Send Data to WunderGround Server
  urlSend = constructHttpGET(temp, humidity, pressure);
  Serial.println("URL to send: " + urlSend);

  httpClient.begin(urlSend.c_str());
  Serial.println("Connected to  wunderground server");
  int httpResponseCode = httpClient.GET();

  if (httpResponseCode > 0) {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("Error code: ");
    Serial.println(httpResponseCode);
  }
  httpClient.end();
}

void FirebaseSend(float *temp, float *pressure, float *humidity,
                  String *databasePath) {
  uint32_t timestamp;
  // Parent Node -- will be updated in every iteration
  String parentPath;
  // Database child nodes
  String tempPath = "/temperature";
  String pressurePath = "/pressure";
  String humidityPath = "/humidity";
  String timePath = "/timestamp";
  FirebaseJson json;

  // Send sensor queries to database
  if (Firebase.ready()){
    //Get current timestamp
    timestamp = getTime();
    Serial.print ("time: ");
    Serial.println (timestamp);

    parentPath = *databasePath + "/" + String(timestamp);

    json.set(tempPath.c_str(), String(*temp));
    json.set(pressurePath.c_str(), String(*pressure));
    json.set(humidityPath.c_str(), String(*humidity));
    json.set(timePath, String(timestamp));
    Serial.printf("Set json... %s\n", Firebase.RTDB.setJSON(&fbdo, parentPath.c_str(), &json) ? "ok" : fbdo.errorReason().c_str());
  } else {
    Serial.printf("Firebase is not ready\n");
  }
}

void setup(){
  bool status, sendToFirebase = true;
  float temp, humidity, pressure;
  uint32_t tempf;
  String databasePath;
  Adafruit_BME280 bme;
  TwoWire I2CBME = TwoWire(0);

  // disable brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  I2CBME.begin(I2C_SDA, I2C_SCL, 100000);

  // default settings
  status = bme.begin(0x76, &I2CBME);
  if (!status) {
    Serial.println("Could not find a valid BME280 sensor, check wiring!");
    while (1);
  }
  Serial.println("-- Default BME Test Passed --");

  initWiFi();

  if (sendToFirebase)
    FirebaseInit(&databasePath);

  temp = bme.readTemperature();
  humidity = bme.readHumidity();
  pressure = bme.readPressure() / 100.0F;

  if (sendToFirebase)
    FirebaseSend(&temp, &pressure, &humidity,
                 &databasePath);

  wundergroundSendData(temp, humidity, pressure);

  EnterHibernation();
}

void loop(){
  /* This is not going to be called
   * since we enter into Hibernation after setup method
   */
}
