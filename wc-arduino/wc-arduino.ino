#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <WiFiNINA.h>
#include <RTCZero.h>
#include <Base64.h>
#include <ArduinoJson.h>
#include <avr/dtostrf.h>
#include <Adafruit_SleepyDog.h>
#include <Adafruit_seesaw.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_AM2320.h>
#include "arduino_secrets.h"

// Real-time clock
RTCZero rtc;

// Wifi connection info
const char WIFI_SSID[] = SECRET_SSID;
const char WIFI_PASS[] = SECRET_PASS;

// Water control server address
IPAddress SERVER_IP(10,0,0,13);

// GPIO pins
const int WATER_CONTROL = 1;
const int BATTERY_VOLTAGE = A5;
const int CHARGING_VOLTAGE = A6;
const int CHIP_SELECT = 4;

// Default water control settings
byte CHECK_HOURS[3] = {7, 10, 13};
uint16_t MIN_SOIL_READING = 400;
byte RUN_DURATION = 15; // water run time (minutes)
byte MIN_BATTERY = 15;

// Flags & control variables
bool waterEnabled = false;
bool dryForcast = false;
volatile bool runMainFunction = false;
long lastSignal = 0;
byte errorCode = 0;
bool sdAvailable = true;
bool clockInitialized = false;

// For logging restart events (loss of power, watchdog timer, etc.)
bool startup = true;

// Voltage divider constants
const int R1 = 820000;
const int R2 = 680000;
const int ANALOG_RESOLUTION = 12;
const int MAX_READING = 4095;
const float OPERATING_VOLTAGE = 3.3;

// Initialize soil sensors
Adafruit_seesaw soilSensorA;
bool soilSensorAActive = true;
Adafruit_seesaw soilSensorB;
bool soilSensorBActive = true;

// Initialize case thermometer
Adafruit_AM2320 am2320 = Adafruit_AM2320();

// File name to log debug data to SD card
const char SD_LOG_FILE[] = "debuglog.txt";

// Track previous newline to for prepending timestamp
bool newLine = true;

// Sample data structure
struct SampledData {
  uint16_t soilMoistureA;
  uint16_t soilMoistureB;
  float soilTemperatureA;
  float soilTemperatureB;
  float caseTemp;
  float caseHumidity;
  float batteryVoltage;
  float loadVoltage;
  byte batteryPercent;

  // Flags
  byte starting;
  byte ending;
  byte skipping;
};

// Init, called once on startup
void setup() {
  watchdogEnable();
  
  // Log startup event on next connection with server
  startup = true;

  // Enable onboard LED and turn it off
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Set maximum analog read resolution for higher voltage reading accuracy
  analogReadResolution(ANALOG_RESOLUTION);

  // Enable log output
  Serial.begin(115200);
  watchdogReset();

  Serial.print("Initializing SD card... ");
  // See if the card is present and can be initialized:
  if(!SD.begin(CHIP_SELECT)) {
    Serial.println("Card failed or not present");
    // Disable SD card functionality
    sdAvailable = false;
  }
  watchdogReset();
  sdlogln("card initialized. (startup)");


  // Check for the WiFi module
  if(WiFi.status() == WL_NO_MODULE) {
    sdlogln("Communication with WiFi module failed!");
    // Blink onboard LED fast until watchdog reset
    while (true) {
      delay(50);
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
  }
  watchdogReset();

  // Initialize soil sensor A
  if(!soilSensorA.begin(0x36)) {
    sdlogln("ERROR! Soil sensor 0x36 not found");
    // Disable sensor readings and set status code for logging
    soilSensorAActive = false;
    errorCode |= 1; // add 00000001
  } else {
    sdlogln("Soil sensor A started.");
  }
  watchdogReset();

  if(!soilSensorB.begin(0x37)) {
    sdlogln("ERROR! Soil sensor 0x37 not found");
    soilSensorBActive = false;
    errorCode |= 2; // add 00000010
  } else {
    sdlogln("Soil sensor B started.");
  }
  watchdogReset();

  // Initialize case thermometer
  am2320.begin();

  // Initialize water control pin
  pinMode(WATER_CONTROL, OUTPUT);

  // Disable watchdog timer
  watchdogDisable();

  // Run initial data samples
  doMain();

  // Put Arduino into low power sleep mode
  rtc.standbyMode();
}

// Arduino main loop
void loop() {
  if(runMainFunction) {
    doMain();
    runMainFunction = false;
    // Reduce power consumption by turning off Wifi
    disconnectWifi();
  }
  if(!waterRunning() && !runMainFunction) {
    // Sleep until next alarm match
    rtc.standbyMode();
  }
  watchdogReset();
}

// Bound event called on alarm time match
void awaken() {
  runMainFunction = true;
}

void connectToWIFI() {
  int status = WL_IDLE_STATUS;
  // Attempt to connect to Wifi network if not already connected
  while (WiFi.status() != WL_CONNECTED) {
    sdlog("Attempting to connect to SSID: ");
    sdlogln(WIFI_SSID);
    status = WiFi.begin(WIFI_SSID, WIFI_PASS);

    // If status code 4 (often seen when in connection failure loop) restart 
    // Arduino using watchdog
    if(status == 4) {
      sdlogln("Restarting using watchdog.");
      watchdogEnable();
      delay(10000);
    }

    // Wait 10 seconds for connection while flashing LED
    for(int x = 0; x < 100; x++) {
      delay(100);
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
    digitalWrite(LED_BUILTIN, LOW);
  }
  printWifiStatus();
  lastSignal = WiFi.RSSI();
}

// Turn off Wifi chip to conserve battery
void disconnectWifi() {
  WiFi.disconnect();
  WiFi.end();
}

// Pull settings JSON object from wc-server
void updateSettings() {
  // Static JSON doc size caluclated using 
  // ArduinoJson Assistant: https://arduinojson.org/v6/assistant/
  StaticJsonDocument<400> doc;
  char json[300];

  httpRequest("/weather/get-settings/", json);

  watchdogEnable();
  // Deserialize the JSON document
  DeserializationError error = deserializeJson(doc, json);

  // Clear error before parsing
  errorCode &= (~4); // clear 00000100 bit

  // Test if parsing succeeds.
  if(error) {
    sdlog("deserializeJson() failed: ");
    sdlogln(error.c_str());
    errorCode |= 4; // add 00000100
  }

  // Initialize internal real-time clock
  rtc.begin();

  // Set RTC date/time from server
  rtc.setTime(doc["hour"], doc["minute"], doc["second"]);
  rtc.setDate(doc["day"], doc["month"], doc["year"]);
  clockInitialized = true;

  // Check if forcast will be dry
  if(doc.containsKey("dry_forcast")) {
    dryForcast = doc["dry_forcast"];
    sdlog(" dryForcast=");
    sdlog((dryForcast) ? "true" : "false");
  }

  // Remote configuration for solenoid valve control
  if(doc.containsKey("water_enabled")) {
    waterEnabled = doc["water_enabled"];
    sdlog(" waterEnabled=");
    sdlog((waterEnabled) ? "true" : "false");
  }

  // Minimum soil moisture reading
  if(doc.containsKey("min_soil")) {
    MIN_SOIL_READING = doc["min_soil"];
    sdlog(" MIN_SOIL_READING=");
    sdlog(MIN_SOIL_READING);
  }

  // Number of minutes to run the water
  if(doc.containsKey("run_duration")) {
    RUN_DURATION = doc["run_duration"];
    sdlog(" RUN_DURATION=");
    sdlog(RUN_DURATION);
  }

  // Hours to check if the water should be run
  if(doc.containsKey("run_times")) {
    for(byte x = 0; x < (sizeof(CHECK_HOURS) / sizeof(*CHECK_HOURS)); x++) {
      CHECK_HOURS[x] = doc["run_times"][x];
      sdlog(" CHECK_HOURS[");
      sdlog(x);
      sdlog("]=");
      sdlog(CHECK_HOURS[x]);
    }
  }
  sdlogln();

  // Run checks on the half hour of every hour
  rtc.setAlarmTime(00, 30, 00);
  rtc.enableAlarm(rtc.MATCH_MMSS);

  // Attach alarm event to awaken function
  rtc.attachInterrupt(awaken);

  watchdogDisable();
}

// Read soil moisture and temperature levels if the sensors are present
void readSoilSensors(float &soilTemperatureA, float &soilTemperatureB, 
                      uint16_t &soilMoistureA, uint16_t &soilMoistureB) {
  watchdogEnable();

  soilTemperatureA = soilSensorAActive ? soilSensorA.getTemp() : 0.0f;
  soilMoistureA = soilSensorAActive ? soilSensorA.touchRead(0) : 0.0f;
  soilTemperatureB = soilSensorBActive ? soilSensorB.getTemp() : 0.0f;
  soilMoistureB = soilSensorBActive ? soilSensorB.touchRead(0) : 0.0f;

  watchdogDisable();
}

// Read sensors, log data and run the water if the conditions are right
void doMain() {
  bool runWater = false;
  static byte lastRunDay = 0;

  // Record sampled data and flags to for loging to server
  SampledData sData;
  sData.soilMoistureA = 0;
  sData.soilMoistureB = 0;
  sData.soilTemperatureA = 0.0f;
  sData.soilTemperatureB = 0.0f;
  sData.caseTemp = 0.0f;
  sData.caseHumidity = 0.0f;
  sData.batteryVoltage = 0.0f;
  sData.loadVoltage = 0.0f;
  sData.batteryPercent = 0;
  sData.starting = 0;
  sData.ending = 0;
  sData.skipping = 0;

  // Stop water if it is running
  if(waterRunning()) {
    digitalWrite(WATER_CONTROL, LOW);
    sData.ending = 1;
  }

  updateSettings();

  // Get current day from real-time clock to compare with last run date
  byte currentDay = rtc.getDay();

  // Read all sensors
  readCaseSensors(sData.caseTemp, sData.caseHumidity);
  readSoilSensors(sData.soilTemperatureA, sData.soilTemperatureB, sData.soilMoistureA, sData.soilMoistureB);
  readPowerLevels(sData.batteryVoltage, sData.loadVoltage, sData.batteryPercent);

  watchdogEnable();
  // If water has not run today and soil is dry
  if(lastRunDay != rtc.getDay() && 
    (sData.soilMoistureA <= MIN_SOIL_READING || 
      sData.soilMoistureB <= MIN_SOIL_READING)) {

    bool timeToWater = isScheduledTime();

    // Run the water if it is a scheduled time and the forcast is dry
    runWater = dryForcast && timeToWater;

    // Log skipping due to weather
    if(!dryForcast && timeToWater) {
      sData.skipping = 1;
    }
  }
  watchdogReset();
  
  // Run water if conditions are right and there is enough battery
  if(runWater && waterEnabled && sData.batteryPercent >= 15) {
    // Set next event time RUN_DURATION from current time
    rtc.setAlarmMinutes((rtc.getMinutes() + RUN_DURATION) % 60);

    // Log that watering has initiated for RUN_DURATION minutes
    sData.starting = RUN_DURATION;
    sData.ending = 0;

    // Open solenoid valve to run water
    digitalWrite(WATER_CONTROL, HIGH);

    // Set current day as last run time to avoid watering more than once per day
    lastRunDay = rtc.getDay();
  } else {
    // Reset event time to the half hour
    rtc.setAlarmMinutes(30);
  }
  watchdogDisable();

  logData(sData);
}

// Check schedule to see if it is time to run water. Only water on the half hour 
// (with some leeway since the Wifi can take time to connect).
bool isScheduledTime() {
  bool result = false;
  byte currentHour = rtc.getHours();
  byte currentMinute = rtc.getMinutes();

  if(currentMinute >= 30 && currentMinute <= 35) {
    for(byte x = 0; x < (sizeof(CHECK_HOURS) / sizeof(*CHECK_HOURS)); x++) {
      if(currentHour == CHECK_HOURS[x]) {
        result = true;
        break;
      }
    }
  }

  return result;
}

// Water is running if the WATER_CONTROL pin is HIGH
bool waterRunning() {
  return digitalRead(WATER_CONTROL) == HIGH;
}

// Read an analog pin and calculate the voltage using the voltage divider formula
float getVoltage(int analogPin) {
  int analogReading = 0;
  float Vout = 0;
  float Vin = 0;

  // Read the value from the sensor
  analogReading = analogRead(analogPin);
  
  Vout = ((float)analogReading / (float)MAX_READING) * OPERATING_VOLTAGE;
  Vin = (Vout * ((float)R1 + (float)R2)) / (float)R2;

  return Vin;
}

// Calculate the average of NUM_READINGS for a more accurate voltage reading
float getAverageVoltage(int analogPin) {
  float totalReading = 0;
  const int NUM_READINGS = 10;

  for(int i = 0; i < NUM_READINGS; i++) {
    totalReading += getVoltage(analogPin);
    delay(1);
  }

  return totalReading / (float)NUM_READINGS;
}

// Calculate battery percentage based on tested discharge rate. Voltage is 
// linear then drops rapidly under 5%.
byte getBatteryPercent(float voltage) {
  byte percent = -1;

  if(voltage > 3.41) {
    percent = round(118.75 * voltage - 399.938);
    if(percent > 100) {
      percent = 100;
    }
  } else if(voltage <= 3.41 && voltage > 3.38) {
    percent = 5;
  } else if(voltage <= 3.38 && voltage > 3.33) {
    percent = 4;
  } else if(voltage <= 3.33 && voltage > 3.24) {
    percent = 3;
  } else if(voltage <= 3.24 && voltage > 3.09) {
    percent = 2;
  } else if(voltage <= 3.09 && voltage > 2.51) {
    percent = 1;
  } else if(voltage <= 2.51) {
    percent = 0;
  }

  return percent;
}

// Read the temperature and humidity inside project box
void readCaseSensors(float &caseTemp, float &caseHumidity) {
  watchdogEnable();

  // Clear errors reading sensor
  errorCode &= (~8);  // clear 00001000
  errorCode &= (~16); // clear 00010000

  caseTemp = am2320.readTemperature();
  caseHumidity = am2320.readHumidity();

  // Set values to 0.0f when nan and/or sensor not attached
  if(isnan(caseTemp)) {
    caseTemp = 0.0f;
    errorCode |= 8;  // add 00001000
  }
  if(isnan(caseHumidity)) {
    caseHumidity = 0.0f;
    errorCode |= 16; // add 00010000
  }

  watchdogDisable();
}

void readPowerLevels(float &batteryVoltage, float &loadVoltage, byte &batteryPercent) {
  watchdogEnable();

  batteryVoltage = getAverageVoltage(BATTERY_VOLTAGE);
  loadVoltage = getAverageVoltage(CHARGING_VOLTAGE);
  batteryPercent = getBatteryPercent(batteryVoltage);
  
  watchdogDisable();
}

void logData(SampledData sData) {
  watchdogEnable();

  // Build request to log data
  char url[20] = "/weather/log-data/";
  char response[10];

  // Use data to build JSON string to send to server
  const size_t capacity = JSON_OBJECT_SIZE(15);
  DynamicJsonDocument doc(capacity);
  char jsonData[300] = "";
  char postData[450] = "data=";
  char encodedData[401] = "";
  size_t jsonDataLen = sizeof(jsonData);
  size_t encodedDataLen = sizeof(encodedData);
  
  doc["soil_moisture_a"] = sData.soilMoistureA;
  doc["soil_moisture_b"] = sData.soilMoistureB;
  doc["soil_temperature_a"] = sData.soilTemperatureA;
  doc["soil_temperature_b"] = sData.soilTemperatureB;
  doc["starting"] = sData.starting;
  doc["ending"] = sData.ending;
  doc["skipping"] = sData.skipping;
  doc["case_temperature"] = sData.caseTemp;
  doc["case_humidity"] = sData.caseHumidity;
  doc["battery_voltage"] = sData.batteryVoltage;
  doc["battery_percent"] = sData.batteryPercent;
  doc["load_voltage"] = sData.loadVoltage;
  doc["signal"] = lastSignal;
  doc["startup"] = startup ? 1 : 0;
  doc["error"] = errorCode;
  
  serializeJson(doc, jsonData, jsonDataLen);
  Base64.encode(encodedData, jsonData, jsonDataLen);
  strncat(postData, encodedData, encodedDataLen);
  watchdogDisable();

  httpRequest(url, response, postData);
}

// Overload for get requests
bool httpRequest(const char *url, char *response) {
  return httpRequest(url, response, "");
}

// Make an HTTP GET or POST request to the URL
bool httpRequest(const char *url, char *response, const char *postData) {
  // Check Wifi connection and reconnect if needed
  connectToWIFI();
  watchdogEnable();

  // Initialize the Wifi client library
  WiFiClient client;

  bool connectionSuccess = true;
  sdlogln(url);
  response[0] = 0;

  // Close any open connection before sending a new request
  client.stop();
  client.flush();

  // If there's a successful connection
  if(client.connect(SERVER_IP, 80)) {
    // Send the HTTP GET request if the POST data is blank
    if(strlen(postData) == 0) {
      client.print("GET ");
    } else {
      client.print("POST ");
    }

    client.print(url);
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(SERVER_IP);
    client.println("User-Agent: ArduinoWiFi/1.1");
    client.println("Connection: close");
    
    if(strlen(postData) == 0) {
      client.println();
    } else {
      client.println("Content-Type: application/x-www-form-urlencoded;");
      client.print("Content-Length: ");
      client.println((unsigned)strlen(postData));
      client.println();
      client.println(postData);
    }
    watchdogReset();

    int timeout = 5000; // 5 seconds
    while (!client.available() && timeout > 0) {
      timeout--;
      delay(1);
    }
    watchdogReset();
  } else {
    sdlogln("HTTP connection failed.");
    connectionSuccess = false;
  }

  // Process response and remove headers when the first pair of \n 
  // characters is reached last char was \n. Strings would make this
  // much less complicated but could quickly fragment the small amount
  // of memory available on the Arduino.
  bool lfLast = false;
  
  // Headers have been skipped
  bool headersSkipped = false;

  // Used for appending char using strcat
  char buffer[2] = "";
  buffer[1] = '\0';

  watchdogReset();
  
  // While there are still characters in response stream
  while (client.available()) {
    // Read next character in the stream
    char c = client.read();

    // Ignore carriage returns per RFC7230, § 3.5
    if(c != '\r' && !headersSkipped) {
      if(c == '\n' && !lfLast) {
        // One line feed found
        lfLast = true;

      } else if(c == '\n' && lfLast) {
        // Two line feed characters in a row
        headersSkipped = true;

      } else {
        // Reset line feed check, non-line end character found
        lfLast = false;
      }
    }
    // Once the headers have been skipped, build response string
    if(headersSkipped) {
      buffer[0] = c;
      strcat(response, buffer);
    }
  }
  watchdogDisable();

  sdlog("response: ");
  sdlogln(response);

  return connectionSuccess;
}

// Print data of any type to both the SD card and Serial monitor
template<typename T>
void sdlogger(T data, bool ln) {
  if(sdAvailable) {
    // Open the log file in write mode
    File dataFile = SD.open(SD_LOG_FILE, FILE_WRITE);

    // If the file is available, write to it
    if(dataFile) {
      if(newLine) {
        if(clockInitialized) {
          // Print timestamp if RTC is set
          dataFile.print(rtc.getHours());
          dataFile.print(":");
          dataFile.print(rtc.getMinutes());
          dataFile.print(":");
          dataFile.print(rtc.getSeconds());
        } else if(!clockInitialized) {
          dataFile.print("RTC not set");
        }
        dataFile.print(" - ");
      }

      if(ln) {
        dataFile.println(data);
        Serial.println(data);
      } else {
        dataFile.print(data);
        Serial.print(data);
      }

      // Close the file to commit the changes to the SD card
      dataFile.close();
    } else {
      // If the file failed to open, display warning on serial monitor
      Serial.print("Error opening file ");
      Serial.println(SD_LOG_FILE);
    }
  }
  newLine = ln;
}

// SD logger overloads to make it function similar to Serial.print() and
// Serial.println()
template<typename T>
void sdlog(T data) {
  sdlogger(data, false);
}

template<typename T>
void sdlogln(T data) {
  sdlogger(data, true);
}

void sdlogln() {
  sdlogger("", true);
}

// Enable watchdog timer with maximum countdown and reset timer since
// countdown is preserved
void watchdogEnable() {
  Watchdog.enable(8000);
  watchdogReset();
}

// Reset watchdog timer countdown to maximum value set with enable
void watchdogReset() {
  Watchdog.reset();
}

// Reset before disable since countdown is preserved
void watchdogDisable() {
  watchdogReset();
  Watchdog.disable();
}

void printWifiStatus() {
  // Print the SSID of the connected network
  sdlog("SSID: ");
  sdlogln(WiFi.SSID());

  // Print Arduino IP address
  IPAddress ip = WiFi.localIP();
  sdlog("IP Address: ");
  sdlogln(ip);

  // Print the received signal strength
  long rssi = WiFi.RSSI();
  sdlog("Signal strength (RSSI):");
  sdlog(rssi);
  sdlogln(" dBm");
}
