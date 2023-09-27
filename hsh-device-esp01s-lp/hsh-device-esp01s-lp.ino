/* hsh-device-esp01s-lp.ino
 *           _  _               ___                _   _  _               
 * __ ___ __| || |___ _ __  ___/ __|_ __ _____ ___| |_| || |___ _ __  ___ 
 * \ \ / '_ \ __ / _ \ '  \/ -_)__ \ V  V / -_) -_)  _| __ / _ \ '  \/ -_)
 * /_\_\ .__/_||_\___/_|_|_\___|___/\_/\_/\___\___|\__|_||_\___/_|_|_\___|
 *     |_|                                                                
 *
 * This file is part of the HomeSweetHome eXPerience:
 *     https://github.com/gom9000/xp-home-sweet-home
 *
 * Author.....: Alessandro Fraschetti (mail: gos95@gommagomma.net)
 * Target.....: ESP-01S board
 * Version....: 1.0 2023/04/15
 * Description: HomeSweetHome low-power device manager firmware for ESP-01s board
 * URL........: https://github.com/gom9000/xp-home-sweet-home
 * License....: MIT License
 */


#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include "DHT.h"


#define APSSID "HomeSweetHome"
#define APPSK  "87654321"
#define STA_CONNECTION_ATTEMPTS 20
#define STA_CONNECTION_RETRIES 2
#define SWITCH_PIN 0
#define CONFIG_SWITCH_INTERVAL 1000
#define REBOOT_SWITCH_INTERVAL 4000
#define RESET_SWITCH_INTERVAL 8000
#define CONFIG_MODE_INTERVAL 90000
#define TOGGLE_ACTIVITY_LED_ON_INTERVAL 50
#define TOGGLE_ACTIVITY_LED_FAST_INTERVAL 1000
#define TOGGLE_ACTIVITY_LED_SLOW_INTERVAL 4000
#define SLEEP_INTERVAL_MINUTES 1

#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321
#define DHTPIN 2


const IPAddress apIP(192, 168, 1, 1);
const IPAddress gateway(192, 168, 1, 254);
const IPAddress subnet(255, 255, 255, 0);
const char *appsk = APPSK;
const char* apssid = APSSID;

struct SensorData
{
    float temperature = 0.0;
    float humidity = 0.0;
};
SensorData lastSensorData, currentSensorData;

struct Configuration
{
  int filledFlag;
  char ssid[32];
  char psk[63];
  char serverHost[253];
  unsigned long serverPort;
};
Configuration config;
Configuration emptyConfig = {0, '\0', '\0', '\0', '\0'};
int connectionAttempt = 0;

// eeprom address offsets
const unsigned int eeRetryAddr = 0;
const unsigned int eeConfigAddr = sizeof(int);
const unsigned int eeSensorDataAddr = eeConfigAddr + sizeof(Configuration);

// switch status vars
int lastSwitchState = HIGH;
unsigned long lastSwitchDebounceTime = 0;

// activity led status vars
int activityLedState = HIGH;
unsigned long lastActivityLedToggleMillis = 0;
unsigned long ledToggleInterval;

// config mode status vars
unsigned long lastConfigModeMillis = 0;

// operation mode flags
bool configLoopFlag = false;
bool workingLoopFlag = false;
bool configModeFlag = false;
bool resetModeFlag = false;
bool rebootModeFlag = false;
bool ledToggleFlag = false;
bool connectedFlag = false;
bool startSTAFlag = false;
bool stopSTAFlag = false;
bool startAPFlag = false;
bool startHTTPServerFlag = false;
bool writeConfigFlag = false;
bool timeToSleepFlag = false;
bool readyToSendDataFlag = false;
bool sentDataFlag = false;
bool timeoutFlag = false;

//bool runningTaskFlag = false;

// http server vars
const byte DNS_PORT = 53;
DNSServer dnsServer;
ESP8266WebServer server(80);

// init DHT sensor.
DHT dht(DHTPIN, DHTTYPE);


void setup()
{
    delay(1000);

    // init the device
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(SWITCH_PIN, INPUT);
    pinMode(DHTPIN, INPUT);

    Serial.begin(115200);

    digitalWrite(LED_BUILTIN, LOW);
    delay(2000);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.println();

    // DHT start sensor 
    dht.begin();

    // load configuration from eeprom
    EEPROM.begin(512);
    EEPROM.get(eeRetryAddr, connectionAttempt);
    EEPROM.get(eeConfigAddr, config);
    EEPROM.get(eeSensorDataAddr, lastSensorData);

    if (config.filledFlag != 0 and config.filledFlag != 1)
    {
        Serial.println("first device boot");
        writeConnAttempt(eeRetryAddr, 0);
        writeConfiguration(eeConfigAddr, emptyConfig);
        EEPROM.get(eeRetryAddr, connectionAttempt);
        EEPROM.get(eeConfigAddr, config);
    } else {
        Serial.println("device boot");
        Serial.print("connection attempt: ");
        Serial.println(connectionAttempt);
    }

    // test configuration
    if (config.filledFlag == 0 || connectionAttempt >= STA_CONNECTION_RETRIES)
    {
        // not yet configured, config loop
        configLoopFlag = true;
        Serial.println("start config loop");
    } else {
        // try working loop
        workingLoopFlag = true;
        Serial.println("start working loop");
    }
}


void loop()
{
    long currentMillis = millis();

    //
    // debounce function switch 
    //
    int switchReading = digitalRead(SWITCH_PIN);
    if ((currentMillis - lastSwitchDebounceTime) > RESET_SWITCH_INTERVAL)
    {
        if (switchReading != lastSwitchState)
        {
            lastSwitchDebounceTime = currentMillis;
            if (switchReading == HIGH) {
                Serial.println("switch event: RESET request");
                resetModeFlag = true;
            }
        }
    }
    else if ((currentMillis - lastSwitchDebounceTime) > REBOOT_SWITCH_INTERVAL)
    {
        if (switchReading != lastSwitchState)
        {
            lastSwitchDebounceTime = currentMillis;
            if (switchReading == HIGH) {
                Serial.println("switch event: REBOOT request");
                rebootModeFlag = true;
            }
        }
    }
    else if ((currentMillis - lastSwitchDebounceTime) > CONFIG_SWITCH_INTERVAL)
    {
        if (switchReading != lastSwitchState)
        {
            lastSwitchDebounceTime = currentMillis;
            if (switchReading == HIGH) {
                Serial.println("switch event: CONFIG request");
                configModeFlag = true;
            }
        }
    }
    lastSwitchState = switchReading;


    //
    // toggle activity led
    //
    if (ledToggleFlag)
    {
        long hiLoInterval = ledToggleInterval;
        if (activityLedState == LOW) { hiLoInterval = (long)TOGGLE_ACTIVITY_LED_ON_INTERVAL; }
        if (currentMillis - lastActivityLedToggleMillis >= hiLoInterval)
        {
            lastActivityLedToggleMillis = currentMillis;
            activityLedState = !activityLedState;
            digitalWrite(LED_BUILTIN, activityLedState);
        }
    }


    //
    // configuration loop
    //
    if (configLoopFlag)
    {
        ledToggleFlag = true;

        if (configModeFlag)
        {
            if (startHTTPServerFlag)
            {
                // handle http client
                dnsServer.processNextRequest();
                server.handleClient();
            }

            if (!startAPFlag)
            {
                Serial.println("start config mode...");

                // led toggle fast            
                ledToggleInterval = TOGGLE_ACTIVITY_LED_FAST_INTERVAL;

                // start Wifi AP mode
                startWiFiAP(apssid, appsk, apIP, gateway, subnet);
                startAPFlag = true;

                // start HTTPServer
                startHTTPServer();
                startHTTPServerFlag = true;

                lastConfigModeMillis = currentMillis;
            }

            if (currentMillis - lastConfigModeMillis >= CONFIG_MODE_INTERVAL)
            {
                // config mode timeout occurred
                Serial.println("stop config mode...");
                stopHTTPServer();
                stopWiFi();
                WiFi.config(0, 0, 0);
                startHTTPServerFlag = false;
                startAPFlag = false;
                configModeFlag = false;
            }
        }
        else
        {
            // led toggle slow
            ledToggleInterval = TOGGLE_ACTIVITY_LED_SLOW_INTERVAL;
        }
    }


    //
    // working loop
    //
    if (workingLoopFlag)
    {
        if (!readyToSendDataFlag && !stopSTAFlag && !startSTAFlag)
        {
            // read sensor data
            Serial.println("read sensor data");
            Serial.print("last values: T=");
            Serial.print(lastSensorData.temperature);
            Serial.print(", H=");
            Serial.println(lastSensorData.humidity);
            readSensorTempAndHum(&currentSensorData.temperature, &currentSensorData.humidity);
            Serial.print("new values: T=");
            Serial.print(currentSensorData.temperature);
            Serial.print(", H=");
            Serial.println(currentSensorData.humidity);
            if (currentSensorData.temperature != lastSensorData.temperature || currentSensorData.humidity != lastSensorData.humidity)
            {
                lastSensorData.temperature = currentSensorData.temperature;
                lastSensorData.humidity = currentSensorData.humidity;
                writeSensorData(eeSensorDataAddr, lastSensorData);
                readyToSendDataFlag = true;
            } else {
                // nothing else to do, prepare to sleep
                Serial.print("nothing else to do, ");
                timeToSleepFlag = true;
            }
        }

        if (stopSTAFlag)
        {
            // disconnect and stop WiFi
            stopWiFi();
            connectedFlag = false;
            startSTAFlag = false;
            stopSTAFlag = false;
        }

        if (connectedFlag)
        {
            // send data
            Serial.println("send data");

            // TODO... send sensor data
            // TODO... send log data
            // TODO... write send-error event to log
            readyToSendDataFlag = false;
            sentDataFlag = true;
            stopSTAFlag = true;
        }

        if (!startSTAFlag && readyToSendDataFlag)
        {
            // start & connect WiFi
            startSTAFlag = true;
            if (startWiFiStation(config.ssid, config.psk, STA_CONNECTION_ATTEMPTS))
            {   
                connectedFlag = true;
                connectionAttempt = 0;
            } else {
                // TODO... write event to log
                timeoutFlag = true;
                connectionAttempt++;
            }
            writeConnAttempt(eeRetryAddr, connectionAttempt);
        }

        if (sentDataFlag && !stopSTAFlag || timeoutFlag)
        {
            // prepare to sleep
            timeToSleepFlag = true;
            timeoutFlag = false;
        }
    }


    //
    // reset device configuration
    //
    if (resetModeFlag)
    {
        resetModeFlag = false;
        connectionAttempt = 0;
        writeConnAttempt(eeRetryAddr, connectionAttempt);        
        writeConfiguration(eeConfigAddr, emptyConfig);
        rebootModeFlag = true;
    }

    //
    // device reboot
    //
    if (rebootModeFlag)
    {
        if (startAPFlag) { stopHTTPServer(); stopWiFi(); }        
        Serial.println("restart device...");
        ESP.restart();
    }

    //
    // write device configuration
    //
    if (writeConfigFlag)
    {
        writeConfigFlag = false;
        config.filledFlag = 1;
        writeConfiguration(eeConfigAddr, config);
        connectionAttempt = 0;
        writeConnAttempt(eeRetryAddr, connectionAttempt);
        rebootModeFlag = true;
    }

    //
    // time to sleep
    //
    if (timeToSleepFlag)
    {
        // TODO... wakeup by switch and/or timer
        timeToSleepFlag = false;
        Serial.println("ready to sleep...");
        //ESP.deepSleep(SLEEP_INTERVAL_MINUTES * 60 * 1e6);
        delay(SLEEP_INTERVAL_MINUTES * 60 * 1e3);
        ESP.restart();
    } else {
        delay(50);
    }
}


void startWiFiAP(const char* apssid, const char* appsk, const IPAddress ip, const IPAddress gateway, const IPAddress subnet)
{
    Serial.print("setting access point SSID: ");
    Serial.print(apssid);
    Serial.print("... ");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(ip, gateway, subnet);
    WiFi.softAP(apssid, appsk, 1, 0, 1);

    Serial.print("ready, IP address: ");
    Serial.println(WiFi.softAPIP());
}
bool startWiFiStation(const char* ssid, const char* psk, const int attempts)
{
    bool result = false;
    int attempt = 1;

    Serial.print("connecting station to SSID: ");
    Serial.print(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, psk);
    while (WiFi.status() != WL_CONNECTED)
    {
        if (attempt++ > attempts) break;
        delay(1000);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        result = true;
        Serial.print(" connected, IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("connection timeout occurred");
    }

    return result;
}
void stopWiFi()
{
    Serial.println("disconnect and stop WiFi");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void startHTTPServer()
{
    // setup dns server
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", apIP);

    // setup http server
    server.on("/", handleConfiguration);
    server.on("/apply", handleConfigurationApply);
    server.onNotFound(handleConfiguration);
    server.begin();
    Serial.println("HTTP server started");
}
void stopHTTPServer()
{
    server.stop();
    dnsServer.stop();
}

void handleConfiguration()
{
    String page = "<!DOCTYPE html>\
    <head>\
      <title>HomeSweetHome Sensor Device Configuration Page</title>\
      <style>body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }</style>\
    </head><body>\
    <form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"/apply\">\
      <label for=\"ssid\">SSID</label><br>\
      <input type=\"text\" name=\"ssid\" id=\"ssid\" value=\"ssid_placeholder\"><br><br>\
      <label for=\"psk\">PSK</label><br>\
      <input type=\"password\" name=\"psk\" id=\"psk\" value=\"psk_placeholder\"><br><br>\
      <label for=\"serverHost\">Server Host</label><br>\
      <input type=\"text\" name=\"serverHost\" id=\"serverHost\" value=\"serverhost_placeholder\"><br><br>\
      <label for=\"serverPort\">Server Port</label><br>\
      <input type=\"text\" name=\"serverPort\" id=\"serverPort\" value=\"serverport_placeholder\"><br><br>\
      <input type=\"submit\" value=\"Apply\">\
    </form></body></html>";

    page.replace("ssid_placeholder", config.ssid);
    page.replace("psk_placeholder", config.psk);
    page.replace("serverhost_placeholder", config.serverHost);
    page.replace("serverport_placeholder", String(config.serverPort));

    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "text/html", page);
    server.client().stop();
}
void handleConfigurationApply()
{
    String message = "POST form was:\n";
    for (uint8_t i = 0; i < server.args(); i++) { message += " " + server.argName(i) + ": " + server.arg(i) + "\n"; }
    server.arg("ssid").toCharArray(config.ssid, server.arg("ssid").length()+1);
    server.arg("psk").toCharArray(config.psk, server.arg("psk").length()+1);
    server.arg("serverHost").toCharArray(config.serverHost, server.arg("serverHost").length()+1);
    config.serverPort = (unsigned long)atoi(server.arg("serverPort").c_str());

    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "text/plain", message);
    writeConfigFlag = true;
}

void writeConfiguration(const int address, const Configuration conf)
{
    Serial.print("write device configuration... ");
    EEPROM.put(address, conf);
    if (EEPROM.commit()) {
        Serial.println("EEPROM successfully committed");
    } else {
        Serial.println("ERROR! EEPROM commit failed");
    }
}
void writeConnAttempt(const int address, const int count)
{
    Serial.print("write connection attempt... ");
    EEPROM.put(address, count);
    if (EEPROM.commit()) {
        Serial.println("EEPROM successfully committed");
    } else {
        Serial.println("ERROR! EEPROM commit failed");
    }
}
void writeSensorData(const int address, const SensorData data)
{
    Serial.print("write sensor data... ");
    EEPROM.put(address, data);
    if (EEPROM.commit()) {
        Serial.println("EEPROM successfully committed");
    } else {
        Serial.println("ERROR! EEPROM commit failed");
    }
}

void readSensorTempAndHum(float *currentTemperature, float *currentHumidity)
{
    // TODO... read sensor data
    *currentTemperature = dht.readTemperature();
    *currentHumidity = dht.readHumidity();

    if (isnan(*currentTemperature) || isnan(*currentHumidity))
    {
        Serial.println(F("Failed to read from DHT sensor!"));
    }
}

// https://github.com/esp8266/Arduino/tree/master/libraries/DNSServer/examples/CaptivePortalAdvanced
// https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/station-examples.html
// https://www.fattelodasolo.it/2021/03/22/esp8266-programmare-il-deep-sleep/
