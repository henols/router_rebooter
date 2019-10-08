#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESPHelper.h>        //ESPHelper & links to its dependencies found here: https://github.com/ItKindaWorks/ESPHelper
#include <ESPHelperFS.h>
#include <ESPHelperWebConfig.h>
#include <IPAddress.h>
#include <Metro.h>            //Metro library found here: http://playground.arduino.cc/Code/Metro
#include <sharedData.h>
#include <ESP8266Ping.h>
#include <stdint.h>

 #define PING_PERIOD 120 * 1000
//#define PING_PERIOD 5 * 1000
#define CONNECTION_TIMEOUT PING_PERIOD * 5
#define RELAY_OFF_PERIOD 10 * 1000
#define COOLDOWN_PERIOD 120 * 1000 + RELAY_OFF_PERIOD

void startWifi();
void loadConfig();
void checkForAPMode();
void handleStatus();

netInfo config;
ESPHelper myESP;

//setup a server on port 80 (http). We use an external server here because we want more than just a config page
//but also a status page or anything else that we want to display
ESP8266WebServer server(80);
ESPHelperWebConfig configPage(&server, "/config");

//defualt net info for unconfigured devices
netInfo homeNet = { //
    .mqttHost = "YOUR MQTT-IP", //can be blank if not using MQTT
        .mqttUser = "YOUR MQTT USERNAME",   //can be blank
        .mqttPass = "",   //can be blank
        .mqttPort = 1883, //default port for MQTT is 1883 - only chance if needed.
        .ssid = "YOUR SSID", //
        .pass = "", //
        .otaPassword = "", //
        .hostname = "NEW-ESP8266" //
    };

//AP moade setup info
const char * broadcastSSID = "ESP-Hotspot";
const char * broadcastPASS = "";
IPAddress broadcastIP = { 192, 168, 1, 1 };
//const int configBtnPin = D2;
const int configBtnPin = 0;

//timeout before triggering the relay to turn off and on
Metro connectTimeout = Metro(CONNECTION_TIMEOUT);
Metro pingTimer = Metro();
bool pingStatus = false;
const char* pingHost = "google.com";

//variables for controlling the router relay
const int relayPin = 2;
Metro relayTimer = Metro(RELAY_OFF_PERIOD);

//cooldown timer to prevent the system from restarting the router over and over before it
//can fully start up
Metro cooldownTimer = Metro(COOLDOWN_PERIOD);

enum states {
  CHECKING, RUNNING, COOLDOWN, AP_MODE
};
int8_t currentState = CHECKING;

void setup(void) {
  Serial.begin(115200);

  //print some debug
  Serial.println("Starting Up - Please Wait...");
  delay(100);

  pinMode(configBtnPin, INPUT);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);

  //startup the wifi and web server (more in the lines below)
  startWifi();

  //setup the http server and config page (fillConfig will take the netInfo file and use that for
  //default values)
  configPage.fillConfig(&config);
  configPage.begin(config.hostname);

  // Actually start the server (again this would be done automatically
  //if we were just using the config page and didnt use an external server...)
  server.begin();
  server.on("/", HTTP_GET, handleStatus);
}

void loop(void) {

  ////// config handling  //////

  //check to see if the AP mode button has been pressed and go into AP mode if needed
  checkForAPMode();

  //handle saving a new network config
  if (configPage.handle()) {
    Serial.println("Saving new network config and restarting...");
    myESP.saveConfigFile(configPage.getConfig(), "/netConfig.json");
    delay(500);
    ESP.restart();
  }

  //////  main state machine  //////

  //get the current status of ESPHelper
  int espHelperStatus = myESP.loop();

  //if the ping timer has elapsed and we are connected to WiFi then ping the test host
  if (currentState == CHECKING && pingTimer.check()) {
    pingTimer.interval(PING_PERIOD);
    //update pingstatus with results of ping or false if no wifi
    if (espHelperStatus >= WIFI_ONLY) {
      pingStatus = Ping.ping(pingHost);
    } else {
      pingStatus = false;
    }

    //print out results of ping attempt
    if (pingStatus) {
      Serial.println(String("Good ping to " + String(pingHost)));
    } else {
      Serial.println(String("Could not ping " + String(pingHost)));
    }

    pingTimer.reset();
  }
  //reset the timer if the pingStatus is true or if we are in AP mode
  if (currentState == AP_MODE || pingStatus) {
    connectTimeout.reset();
  }

  //if the ESP cannot connect (and based on the above if statement is not in AP broadcast mode)
  //then trigger the relay pin and start (reset) the relay timer
  if (currentState == CHECKING && connectTimeout.check()) {
    Serial.println("Cannot connect to wifi or cannot reach WAN, restarting router...");
    digitalWrite(relayPin, LOW);
    relayTimer.reset();
    cooldownTimer.reset();
    currentState = RUNNING;
  }

  //if the relay is on and the timer for turning the relay off has elapsed
  //then turn the relay off
  else if (currentState == RUNNING && relayTimer.check()) {
    Serial.println("Turning router back on...");
    digitalWrite(relayPin, HIGH);
    currentState = COOLDOWN;
  }

  //cooldown state timer checker (cooldown to prevent the router from being rebooted while still starting up)
  if (currentState == COOLDOWN && cooldownTimer.check()) {
    Serial.println("Router restart cooldown complete returning to normal state.");
    connectTimeout.reset();
    currentState = CHECKING;
  }

  //after each loop give the ESP some time to do other (networking related) functions
  delay(50);
}

//ESPHelper & config setup and runtime handler functions

//function that checks for the config button to be pressed and drops the ESP into AP mode for configuring
void checkForAPMode() {
  if (!digitalRead(configBtnPin) && currentState != AP_MODE) {
    Serial.println("AP mode button pressed - starting broadcast (AP) mode...");
    currentState = AP_MODE;
    myESP.broadcastMode(broadcastSSID, broadcastPASS, broadcastIP);
    myESP.OTA_setPassword(config.otaPassword);
    myESP.OTA_setHostnameWithVersion(config.hostname);
    myESP.OTA_enable();
    Serial.println("Done.");
  }
}

void startWifi() {
  loadConfig();

  //setup other ESPHelper info and enable OTA updates
  myESP.setHopping(false);
  myESP.OTA_setPassword(config.otaPassword);
  myESP.OTA_setHostnameWithVersion(config.hostname);
  myESP.OTA_enable();

  Serial.println("Connecting to network");
  Serial.println(String("SSID: " + String(myESP.getSSID()) + "\nWiFiPass: " + String(myESP.getPASS()) + " "));
  delay(10);

  //connect to wifi before proceeding. (also check for the AP button being pressed and drop into AP mode if it is)
  //this will timeout after 20 seconds and just continue the loop regardless
  Metro startupTimeout = Metro(20000);
  while (myESP.loop() < WIFI_ONLY) {
    checkForAPMode();

    if (currentState == AP_MODE) {
      return;
    }
    if (startupTimeout.check()) {
      break;
    }
    delay(10);
  }

  if (myESP.loop() >= WIFI_ONLY) {
    Serial.println("Success!");
    Serial.print("IP Address: ");
    Serial.println(myESP.getIP());
  } else {
    Serial.println(
        "Could not connect to router. Make sure your device is configured \ncorrectly and/or put it into AP mode for configuration");
  }
}

//attempt to load a network configuration from the filesystem
void loadConfig() {
  //check for a good config file and start ESPHelper with the file stored on the ESP
  if (ESPHelperFS::begin()) {
    Serial.println("Filesystem loaded - Loading Config");
    if (ESPHelperFS::validateConfig("/netConfig.json") == GOOD_CONFIG) {
      Serial.println("Config loaded");
      delay(10);
      myESP.begin("/netConfig.json");
    }

    //if no good config can be loaded (no file/corruption/etc.) then
    //attempt to generate a new config and restart the module
    else {
      Serial.println("Could not load config - saving new config from default values and restarting");
      delay(10);
      
      ESPHelperFS::createConfig(&homeNet, "/netConfig.json");
      ESPHelperFS::end();
      ESP.restart();
   }
  }

  //if the filesystem cannot be started, just fail over to the
  //built in network config hardcoded in here
  else {
    Serial.println("Could not load filesystem, proceeding with default config values");
    delay(10);
    myESP.begin(&homeNet);
  }

  //load the netInfo from espHelper for use in the config page
  config = myESP.getNetInfo();
}

//main config page that allows user to enter in configuration info
void handleStatus() {
  server.send(200, "text/html", String("<html><header><title>Device Info</title></header>"  //
      "<body><p><strong>System Status</strong></br>"//
      "Device Name: " + String(myESP.getHostname()) + "</br>" //
          "Connected SSID: " + String(myESP.getSSID()) + "</br>"  //
          "Device IP: " + String(myESP.getIP()) + "</br>" //
          "Uptime (ms): " + String(millis()) + "</p>"
      "<p> </p>"
      "<p><strong>Device Variables</strong></p>"  //
      "<p>Ping Status: " + String(pingStatus) + "</p>"  //
          "<p>States: 0: CHECKING | 1: RUNNING | 2: COOLDOWN | 3: AP_MODE  </p>"//
          "<p>Device State: " + String(currentState) + "</p>" //
          "</body></html>"));
}
