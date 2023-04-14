/* esp01s-device-manager.ino
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
 * Version....: 1.0 2023/03/29
 * Description: ESP-01s device manager firmware
 * URL........: https://github.com/gom9000/xp-home-sweet-home
 * License....: MIT License
 */


#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>


#define APSSID_PREFIX "homesweethome"
#define APPSK  "87654321"
#define SCAN_PERIOD 45000
#define SEND_PERIOD 30000
#define CONN_ATTEMPTS 80
#define RESET_BUTTON_INTERVAL 5000
#define TOGGLE_LED_INTERVAL 500


const int resetButtonPin = 0;
const unsigned int eeAddress = 11;

const IPAddress apIP(192, 168, 1, 1);
const IPAddress staIP(192, 168, 1, 100);
const IPAddress gateway(192, 168, 1, 254);
const IPAddress subnet(255, 255, 255, 0);
const char *appsk = APPSK;
char apssid[32];

struct Configuration
{
  int filledFlag;
  char hostname[253];
  char ssid[32];
  char psk[63];
  char serverHost[253];
  unsigned long serverPort;
};
Configuration config;
Configuration emptyConfig = {0, '\0', '\0', '\0', '\0', '\0'};
char id[10];
int resetButtonState;
int lastResetButtonState = HIGH;
unsigned long lastResetButtonDebounceTime = 0;

int activityLedState = HIGH;
unsigned long lastToggleMillis = 0;
unsigned long lastScanMillis = 0;
unsigned long lastSendMillis = 0;

// operation flags
bool writeConfigFlag = false;
bool writeIDFlag = false;
bool resetFlag = false;
bool rebootFlag = false;
bool connectFlag = false;
bool reconnectFlag = false;
bool runningTaskFlag = false;

const byte DNS_PORT = 53;
DNSServer dnsServer;
ESP8266WebServer server(80);

void setup()
{
    delay(1000);

    // init the device
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(resetButtonPin, INPUT);

    Serial.begin(115200);

    digitalWrite(LED_BUILTIN, LOW);
    delay(2000);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.println();

    EEPROM.begin(640);
    EEPROM.get(0, id);
    EEPROM.get(eeAddress, config);
    if (config.filledFlag != 0 and config.filledFlag != 1)
    {
      Serial.println("First boot");
      resetConfiguration(eeAddress, emptyConfig);
      EEPROM.put(0, '\0');

      EEPROM.get(eeAddress, config);
      EEPROM.get(0, id);
    }

    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);

    if (config.filledFlag == 0) {   // not yet configured, WiFi mode = AP
        Serial.println("Scan networks and generate unique SSID...");
        digitalWrite(LED_BUILTIN, LOW);
        scanNetworksAndGenerateAPSSID(apssid, APSSID_PREFIX);
        digitalWrite(LED_BUILTIN, HIGH);
        setupWiFiAP(apssid, appsk, apIP, gateway, subnet);
    } else {                  // try WiFi mode = STAtion
        if (!connectWiFiStation(config.ssid, config.psk, CONN_ATTEMPTS))
        {
            // bad configuration or network unavailable -> WiFi mode = AP
            Serial.println("Scan networks and generate unique SSID...");
            digitalWrite(LED_BUILTIN, LOW);
            scanNetworksAndGenerateAPSSID(apssid, APSSID_PREFIX);
            digitalWrite(LED_BUILTIN, HIGH);
            setupWiFiAP(apssid, appsk, apIP, gateway, subnet);
        } else {
            connectFlag = true;
            WiFi.setHostname(config.hostname);
        }
    }

    // setup dns server
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", apIP);

    // setup http server
    server.on("/", handleConfiguration);
    server.on("/conf", handleConfiguration);
    server.on("/conf/apply", handleConfigurationApply);
    server.on("/id", handleIdentification);
    server.on("/id/apply", handleIdentificationApply);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started");
}


void loop()
{
    long currentMillis = millis();

    // handle http client
    dnsServer.processNextRequest();
    server.handleClient();

    // test connection status
    if (WiFi.status() != WL_CONNECTED)
    {
        connectFlag = false;
        runningTaskFlag = true;
    }

    if (!connectFlag && reconnectFlag)
    {
        WiFi.disconnect();
        if (connectWiFiStation(config.ssid, config.psk, CONN_ATTEMPTS))
        {
            connectFlag = true;
            Serial.print("Connected to working network, IP address: ");
            Serial.println(WiFi.localIP());
            runningTaskFlag = false;
        } else {
            disconnectWiFi();
            connectFlag = false;
            Serial.println("Connection timeout occurred");
            // TODO... verifica se occorre modificare il flag runningTaskFlag
        }
    }

    // send sendor data
    if (connectFlag)
    {
        if (currentMillis - lastSendMillis > SEND_PERIOD)
        {
            lastSendMillis = currentMillis;
            Serial.print("Send sensor data identified by: ");
            Serial.println(id);
        }
    }

    // scan WiFi networks
    if (connectFlag)
    {
        if (currentMillis - lastScanMillis > SCAN_PERIOD)
        {
            lastScanMillis = currentMillis;
            Serial.println("Scan networks for out-of-sync devices...");
            WiFi.scanNetworks(true);
            runningTaskFlag = true;
        }
    }

    // check for out-of-sync devices
    int n = WiFi.scanComplete();
    if (n >= 0)
    {
        for (int ii = 0; ii < n; ii++)
        {
            if (WiFi.SSID(ii).indexOf(APSSID_PREFIX) != -1)
            {
                Serial.print("Found device to sync: ");
                Serial.println(WiFi.SSID(ii));

                // connect to device network and sync configuration
                String deviceSSID = WiFi.SSID(ii);
                WiFi.scanDelete();
                disconnectWiFi();
                if (connectWiFiStation(deviceSSID.c_str(), appsk, CONN_ATTEMPTS, false, staIP, gateway, subnet))
                {
                    Serial.println("Connected to device network");
                    postConfigToDevice(apIP, 80);
                }
                disconnectWiFi();
                reconnectFlag = true;
                break;
            }
        }
        WiFi.scanDelete();
        runningTaskFlag = false;
    }

    // toggle connection led activity
    if (!connectFlag)
    {
        if (currentMillis - lastToggleMillis >= TOGGLE_LED_INTERVAL)
        {
            lastToggleMillis = currentMillis;
            activityLedState = !activityLedState;
            digitalWrite(LED_BUILTIN, activityLedState);
        }
    }

    // write device configuration
    if (writeConfigFlag)
    {
        writeConfigFlag = false;
        Serial.print("Write device configuration... ");
        config.filledFlag = 1;
        EEPROM.put(eeAddress, config);
        if (EEPROM.commit()) {
            Serial.println("EEPROM successfully committed");
        } else {
            Serial.println("ERROR! EEPROM commit failed");
        }
        rebootFlag = true;
        runningTaskFlag = true;
    }

    // write device IDentification
    if (writeIDFlag)
    {
        writeIDFlag = false;
        Serial.print("Write device identification... ");
        EEPROM.put(0, id);
        if (EEPROM.commit()) {
            Serial.println("EEPROM successfully committed");
        } else {
            Serial.println("ERROR! EEPROM commit failed");
        }
    }

    // debounce reset button
    int resetButtonReading = digitalRead(resetButtonPin);
    if (resetButtonReading != lastResetButtonState) lastResetButtonDebounceTime = currentMillis;
    if ((currentMillis - lastResetButtonDebounceTime) > RESET_BUTTON_INTERVAL)
    {
        if (resetButtonReading != resetButtonState)
        {
            resetButtonState = resetButtonReading;
            if (resetButtonState == LOW) resetFlag = true;
        }
    }
    lastResetButtonState = resetButtonReading;

    // reset device configuration
    if (resetFlag)
    {
        resetFlag = false;
        resetConfiguration(eeAddress, emptyConfig);
        rebootFlag = true;
        runningTaskFlag = true;
    }

    // device reboot
    if (rebootFlag)
    {
        rebootFlag = false;
        runningTaskFlag = false;
        disconnectWiFi();
        server.close();
        Serial.println("Restart device...");
        ESP.restart();
    }

    // gestire una sleep???
    if (!runningTaskFlag)
        delay(1000);
}


void handleConfiguration()
{
    String page = "<!DOCTYPE html>\
    <head>\
      <title>ESP-01S Sensor Device Configuration Page</title>\
      <style>body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }</style>\
    </head><body>\
    <form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"/conf/apply\">\
      <label for=\"hostname\">Hostname</label><br>\
      <input type=\"text\" name=\"hostname\" id=\"hostname\" value=\"hostname_placeholder\"><br><br>\
      <label for=\"ssid\">SSID</label><br>\
      <input type=\"text\" name=\"ssid\" id=\"ssid\" value=\"ssid_placeholder\"><br><br>\
      <label for=\"psk\">PSK</label><br>\
      <input type=\"password\" name=\"psk\" id=\"psk\" value=\"psk_placeholder\"><br><br>\
      <label for=\"serverhost\">Server Host</label><br>\
      <input type=\"text\" name=\"serverhost\" id=\"serverhost\" value=\"serverhost_placeholder\"><br><br>\
      <label for=\"serverport\">Server Port</label><br>\
      <input type=\"text\" name=\"serverport\" id=\"serverport\" value=\"serverport_placeholder\"><br><br>\
      <input type=\"submit\" value=\"Apply\">\
    </form></body></html>";

    page.replace("hostname_placeholder", config.hostname);
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
void handleIdentification()
{
    String page = "<!DOCTYPE html>\
    <head>\
      <title>ESP-01S Sensor Device Identification Page</title>\
      <style>body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }</style>\
    </head><body>\
    <form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"/id/apply\">\
      <label for=\"hostname\">ID</label><br>\
      <input type=\"text\" name=\"id\" id=\"id\" value=\"id_placeholder\"><br><br>\
      <input type=\"submit\" value=\"Apply\">\
    </form></body></html>";

    page.replace("id_placeholder", id);

    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "text/html", page);
    server.client().stop();
}
void handleNotFound()
{
    handleConfiguration();
}
void handleConfigurationApply()
{
    String message = "POST form was:\n";
    for (uint8_t i = 0; i < server.args(); i++) { message += " " + server.argName(i) + ": " + server.arg(i) + "\n"; }
    server.arg("hostname").toCharArray(config.hostname, server.arg("hostname").length()+1);
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
void handleIdentificationApply()
{
    String message = "POST form was:\n";
    for (uint8_t i = 0; i < server.args(); i++) { message += " " + server.argName(i) + ": " + server.arg(i) + "\n"; }
    server.arg("id").toCharArray(id, server.arg("id").length()+1);

    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "text/plain", message);
    writeIDFlag = true;
}

void scanNetworksAndGenerateAPSSID(char *_ssid, const char* prefix)
{
    int maxSeqNum = 0;
    int currSeqNum = 0;
    int n = WiFi.scanNetworks();

    for (int ii = 0; ii < n; ii++)
    {
        if (WiFi.SSID(ii).indexOf(prefix) != -1)
        {
            Serial.println(WiFi.SSID(ii));
            currSeqNum = atoi(WiFi.SSID(ii).substring(strlen(prefix)).c_str());
            if (currSeqNum > maxSeqNum) maxSeqNum = currSeqNum;
        }
    }
    sprintf(_ssid, "%s%03d", prefix, ++maxSeqNum);
    WiFi.scanDelete();
}

void setupWiFiAP(const char* apssid, const char* appsk, const IPAddress ip, const IPAddress gateway, const IPAddress subnet)
{
    Serial.print("Setting access point SSID: ");
    Serial.print(apssid);
    Serial.print("... ");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(ip, gateway, subnet);
    WiFi.softAP(apssid, appsk, 1, 0, 1);

    Serial.print("Ready, IP address: ");
    Serial.println(WiFi.softAPIP());
}

bool connectWiFiStation(const char* ssid, const char* psk, const int attempts, const bool defvalues, const IPAddress ip, const IPAddress gateway, const IPAddress subnet)
{
    bool result = false;
    int attempt = 1;

    Serial.print("Connecting station to SSID: ");
    Serial.print(ssid);

    WiFi.mode(WIFI_STA);
    if (!defvalues) WiFi.config(ip, gateway, subnet);
    else WiFi.config(0, 0, 0);
    WiFi.begin(ssid, psk);
    while (WiFi.status() != WL_CONNECTED)
    {
        if (attempt++ > attempts) break;
        delay(250);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        result = true;
        Serial.print(" Connected, IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("Connection timeout occurred");
    }

    return result;
}

bool connectWiFiStation(const char* ssid, const char* psk, const int attempts)
{
    return connectWiFiStation(ssid, psk, attempts, true, 0, 0, 0);
}

void disconnectWiFi()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool postConfigToDevice(const IPAddress server, const unsigned long port)
{
    bool result = false;
    WiFiClient client;
    String postConfig = String("hostname=") + config.hostname
                + "&" + String("ssid=") + config.ssid
                + "&" + String("psk=") + config.psk
                + "&" + String("serverhost=") + config.serverHost
                + "&" + String("serverport=") + config.serverPort;

    if (client.connect(server, port))
    {
        client.println("POST /conf HTTP/1.1");
        client.println("Content-Type: application/x-www-form-urlencoded");
        client.print("Content-Length: ");
        client.println(postConfig.length());
        client.println();
        client.print(postConfig);
        client.stop();
        result = true;
    }

    return result;
}

void resetConfiguration(const int address, const Configuration empty)
{
    Serial.print("Reset device configuration... ");
    EEPROM.put(address, empty);
    if (EEPROM.commit()) {
        Serial.println("EEPROM successfully erased");
    } else {
        Serial.println("ERROR! EEPROM erase failed");
    }
}


// 0) sleep mode (???)
// 0) test della reconnect in loop() quando si perde la connessione...
// https://github.com/esp8266/Arduino/tree/master/libraries/DNSServer/examples/CaptivePortalAdvanced
// https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/station-examples.html
