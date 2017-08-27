#include <Arduino.h>

#include <ESP8266WiFi.h>           //https://github.com/esp8266/Arduino

//needed for library
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
ESP8266WebServer server(80);
const char* configurationAPName = "AutoConnectAP";

//for DS3231/I2C
#include <time.h>
#include <Wire.h>
#include <RtcDS3231.h>            //https://github.com/Makuna/Rtc
RtcDS3231<TwoWire> Rtc(Wire);

//SSD1306 I2C
#include <SSD1306.h>              //https://github.com/squix78/esp8266-oled-ssd1306
#include <OLEDDisplay.h>
#include <OLEDDisplayFonts.h>
#include <OLEDDisplayUi.h>
// Include custom images
#include "images.h"
// Display Settings
SSD1306 display(0x3c, D2, D1);
OLEDDisplayUi ui (&display);

#include <math.h>
#include <FS.h>                            //stockage de settings
#include <ArduinoJson.h>
StaticJsonBuffer<1000> jsonBuffer;

//relay control
#define _modeEco 3
#define _modeOff 2
#define _modeHGe 1
#define _modeCom 0
#define RelayPin1 D5
#define RelayPin2 D6

//variable de settings
float timezone = 1;
bool daylight = true;                      // activation de l'heure d'été pour calcul de l'heure locale
bool ntpActive = true;
char ntpServerName[32] = "time.nist.gov";
unsigned long intervalNTP = 3600000;       // refresh display interval (1 Hour)
int relayMode = _modeCom;

#define ConsoPin  A0
int mVperAmp = 66;                         // use 100 for 20A Module and 66 for 30A Module
int RawValue= 0;
int ACSoffset = 2500;

const int led = 13;
unsigned long interval = 1000;             // refresh display interval
unsigned long prevMillis = 0;
unsigned long prevMillisNTP = 0;
unsigned int localPort = 2390;             // local port to listen for UDP packets

/* Don't hardwire the IP address or we won't get the benefits of the pool.
 *  Lookup the IP address for the host name instead */
//IPAddress timeServer(129, 6, 15, 28); // time.nist.gov NTP server
IPAddress timeServerIP; // time.nist.gov NTP server address
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
// A UDP instance to let us send and receive packets over UDP
WiFiUDP udp;

//*****************
//* initial setup *
//*****************
void setup() {
    // put your setup code here, to run once:
    pinMode(led, OUTPUT);
    digitalWrite(led, 0);
    Serial.begin(115200);
    //saut de ligne pour l'affichage dans la console.
    Serial.println();
    Serial.println();

    SPIFFS.begin();
    loadSettings();

    pinMode(RelayPin1, OUTPUT);
    pinMode(RelayPin2, OUTPUT);
    controlRelay(relayMode);

    // initialize dispaly
    display.init();
    display.clear();
    display.display();

    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setContrast(255);
    display.drawString(64, 10, "Initialisation...");
    display.display();

    // Setup RTC
    Rtc.Begin();
    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    printDateTime(compiled);
    if (!Rtc.IsDateTimeValid()) {
        Serial.println(F("WARNING: RTC invalid time, setting RTC with compile time!"));
        Rtc.SetDateTime(compiled);
    }
    // Check if the RTC clock is running (Yes, it can be stopped, if you wish!)
    if (!Rtc.GetIsRunning()) {
        Serial.println(F("WARNING: RTC was not actively running, starting it now."));
        Rtc.SetIsRunning(true);
    }
    // We finally get the time from the RTC into a time_t value (let's call it "now"!)
    RtcDateTime now = Rtc.GetDateTime();
    // As stated above, we reset it to the "Compile time" in case it is "older"
    if (now < compiled) {
        Serial.println(F("WARNING: RTC is older than compile time, setting RTC with compile time!"));
        Rtc.SetDateTime(compiled);
    }
    // Reset the DS3231 RTC status in case it was wrongly configured
    Rtc.Enable32kHzPin(false);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone);

    //WiFiManager
    //Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;
    //reset saved settings
    //wifiManager.resetSettings();

    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setConfigPortalTimeout(180);

    //set custom ip for portal
    //wifiManager.setAPConfig(IPAddress(10,0,1,1), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

    //fetches ssid and pass from eeprom and tries to connect
    //if it does not connect it starts an access point with the specified name
    //here  "AutoConnectAP"
    //and goes into a blocking loop awaiting configuration
    wifiManager.autoConnect(configurationAPName);
    //or use this for auto generated name ESP + ChipID
    //wifiManager.autoConnect();

    if(WiFi.status() != WL_CONNECTED) {
        //relance l'application après le timeout du portail
        //en cas d'arret secteur où la box redemarre lentement.
        ESP.restart();
    }

    //if you get here you have connected to the WiFi
    Serial.println("connected...");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.println("Starting UDP");
    udp.begin(localPort);
    Serial.print("Local port: ");
    Serial.println(udp.localPort());

    if (MDNS.begin("esp8266")) {
        Serial.println("MDNS responder started");
    }

    server.on("/", handleRoot);
    server.on("/temperature", HTTP_GET, handleTemperature);
    server.on("/datetime", HTTP_GET, handleGetDateTime);
    server.on("/date", HTTP_GET, handleGetDate);
    server.on("/time", HTTP_GET, handleGetTime);
    server.on("/params", HTTP_GET, handleParams);
    server.on("/params", HTTP_POST, handleUpdateParams);
    server.on("/action", HTTP_GET, handleAction);
    server.on("/action", HTTP_POST, handleUpdateAction);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP server started");

    setNTPTime();
    displayManagement();
}

//*************
//* main loop *
//*************

void loop() {
    // put your main code here, to run repeatedly:
    server.handleClient();

    unsigned long currMillis = millis();
    // gestion de l'interval entre les actions d'affichage
    if(currMillis > (prevMillis + interval)) {
        prevMillis = currMillis;
        displayManagement();
    }
    // gestion de l'interval NTP
    if(currMillis > (prevMillisNTP + intervalNTP)) {
        prevMillisNTP = currMillis;
        setNTPTime();
    }
}

//**********************
//* display management *
//**********************

void displayManagement() {
    RtcDateTime now = Rtc.GetDateTime();

    display.clear();

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, printDate(now));
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 0, printTime(now));

    //wifi signal indicator
    if(WiFi.status() == WL_CONNECTED) {
        const long signalWiFi = WiFi.RSSI();
        if(signalWiFi > -100) {
            int strength = 0;
            display.setFont(ArialMT_Plain_16);
            display.setTextAlignment(TEXT_ALIGN_CENTER);
            display.drawString(96, 11, String(signalWiFi) + "dBm");
            if(signalWiFi > (-60)) {
                strength = 5;
            } else if(signalWiFi > (-70)) {
                strength = 4;
            } else if(signalWiFi > (-75)) {
                strength = 3;
            } else if(signalWiFi > (-80)) {
                strength = 2;
            } else if(signalWiFi > (-85)) {
                strength = 1;
            }
            display.drawXbm(60, 2, 8, 8, wifiSymbol);
            for(int i=0; i < strength; i++) {
                display.drawLine(68 + (i*2), 10, 68 + (i*2), 10 - (i*2));
            }
        }
    }

    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(32, 11, printTemperature());

    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(32, 28, printAmps());

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(32, 54, WiFi.localIP().toString());

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    String strMode = "inconnu";
    switch(relayMode) {
        case _modeEco:
            strMode = "Economique";
            break;
        case _modeOff:
            strMode = "Eteint";
            break;
        case _modeHGe:
            strMode = "Hors gel";
            break;
        case _modeCom:
            strMode = "Confort";
            break;
        default:
            strMode = "Inconnu";
            break;
    }
    display.drawString(96, 54, strMode);

    display.display();
}

//**********************
//* configuration mode *
//**********************

void configModeCallback (WiFiManager *myWiFiManager) {
    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 10, "Configuaration Mode");
    display.drawString(64, 28, "please connect to " + String(configurationAPName));
    display.display();

    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());

    Serial.println(myWiFiManager->getConfigPortalSSID());
}

//***********
//* routing *
//***********

void handleRoot() {
    digitalWrite(led, 1);
    jsonBuffer.clear();
    JsonObject& root = jsonBuffer.createObject();
    //horodatage
    RtcDateTime now = Rtc.GetDateTime();
    root["date"] = printDate(now);
    root["time"] = printTime(now);
    //valeurs
    root["temperature"] = printTemperature();
    root["mode"] = printMode(relayMode);
    root["consomation"] = printAmps();
    root.prettyPrintTo(Serial);
    Serial.println();
    //envoi
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleParams() {
    digitalWrite(led, 1);
    jsonBuffer.clear();
    JsonObject& root = jsonBuffer.createObject();
    //paramètres
    root["interval"] = interval;
    root["timezone"] = timezone;
    root["daylight"] = daylight;
    root["ntpactive"] = ntpActive;
    root["ntpservername"] = ntpServerName;
    root["ntprefreshinterval"] = intervalNTP;
    root.prettyPrintTo(Serial);
    Serial.println();
    //envoi
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleAction() {
    digitalWrite(led, 1);
    jsonBuffer.clear();
    JsonObject& root = jsonBuffer.createObject();
    //paramètres
    root["mode"] = printMode(relayMode);
    root.prettyPrintTo(Serial);
    Serial.println();
    //envoi
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleTemperature() {
    digitalWrite(led, 1);
    jsonBuffer.clear();
    JsonObject& root = jsonBuffer.createObject();
    root["temperature"] = printTemperature();
    root.prettyPrintTo(Serial);
    Serial.println();
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleGetDateTime() {
    digitalWrite(led, 1);
    jsonBuffer.clear();
    JsonObject& root = jsonBuffer.createObject();
    RtcDateTime now = Rtc.GetDateTime();
    root["date"] = printDate(now);
    root["time"] = printTime(now);
    root.prettyPrintTo(Serial);
    Serial.println();
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleGetDate() {
    digitalWrite(led, 1);
    jsonBuffer.clear();
    JsonObject& root = jsonBuffer.createObject();
    RtcDateTime now = Rtc.GetDateTime();
    root["date"] = printDate(now);
    root.prettyPrintTo(Serial);
    Serial.println();
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleGetTime() {
    digitalWrite(led, 1);
    JsonObject& root = jsonBuffer.createObject();
    jsonBuffer.clear();
    RtcDateTime now = Rtc.GetDateTime();
    root["time"] = printTime(now);
    root.prettyPrintTo(Serial);
    Serial.println();
    server.send(200, "application/json", formatJSON(root));
    digitalWrite(led, 0);
}

void handleUpdateParams() {
    digitalWrite(led, 1);
    bool shouldSaveConfig = false;
    if(server.args() > 0) {
        for (int i = 0; i < server.args(); i++) {
            String key = server.argName(i);
            String value = server.arg(i);
            Serial.println("arguments -> " + key + ":" + value);
            if(key == "date") {
                Serial.println("key date");
                //paramétrage de la date
                String newDate = transDate(value);
                //Serial.println("Date -> " + newDate);

                //paramétrage de l'heure
                RtcDateTime now = Rtc.GetDateTime();
                String newTime = printTime(now);
                //Serial.println("Time -> " + newTime);

                //mise à jour de l'horloge
                RtcDateTime nextDT = RtcDateTime(newDate.c_str(), newTime.c_str());
                Serial.println("DateTime -> " + printDateTime(nextDT));
                Rtc.SetDateTime(nextDT);
                continue;
            } else if(key == "time"){
                Serial.println("key time");

                //paramétrage de la date
                RtcDateTime now = Rtc.GetDateTime();
                String newDate = transDate(printDate(now));
                //Serial.println("Date -> " + (String)newDate);

                //paramétrage de l'heure
                String newTime = value;
                //Serial.println("Time -> " + newTime);

                //mise à jour de l'horloge
                RtcDateTime nextDT = RtcDateTime(newDate.c_str(), newTime.c_str());
                Serial.println("DateTime -> " + printDateTime(nextDT));
                Rtc.SetDateTime(nextDT);
                continue;
            } else if(key == "interval"){
                Serial.println("key interval");
                interval = value.toInt();
                shouldSaveConfig = true;
                continue;
            } else if(key == "timezone"){
                Serial.println("key timezone");
                timezone = value.toInt();
                shouldSaveConfig = true;
                continue;
            } else if(key == "daylight"){
                Serial.println("key daylight");
                daylight = false;
                if(value="true") daylight = true;
                shouldSaveConfig = true;
                continue;
            } else if(key == "ntpactive"){
                Serial.println("key ntpactive");
                ntpActive = false;
                if(value="true") ntpActive = true;
                shouldSaveConfig = true;
                continue;
            } else if(key == "ntpservername"){
                Serial.println("key ntpservername");
                value.toCharArray(ntpServerName, 32);
                shouldSaveConfig = true;
                continue;
            } else if(key == "ntprefreshinterval"){
                Serial.println("key ntprefreshinterval");
                intervalNTP = value.toInt();
                shouldSaveConfig = true;
                continue;
            } else {
                Serial.println("uknown key");
            }
            if(shouldSaveConfig) {
                saveSettings();
            }
        }
        Serial.println();
        server.send(200, "text/plain", "ok");
        digitalWrite(led, 0);
        return;
    }
    Serial.println("no data found in body");
    Serial.println();
    server.send(400, "text/plain", "bad request");
    digitalWrite(led, 0);
}

void handleUpdateAction() {
    digitalWrite(led, 1);
    bool shouldSaveConfig = false;
    if(server.args() > 0) {
        for (int i = 0; i < server.args(); i++) {
            String key = server.argName(i);
            String value = server.arg(i);
            Serial.println("arguments -> " + key + ":" + value);
            if(key == "mode") {
                Serial.println("key mode");
                relayMode = value.toInt();
                controlRelay(relayMode);
                shouldSaveConfig = true;
                continue;
            } else {
                Serial.println("uknown key");
            }
        }
        if(shouldSaveConfig) {
            saveSettings();
        }
        Serial.println();
        server.send(200, "text/plain", "ok");
        digitalWrite(led, 0);
        return;
    }
    Serial.println("no data found in body");
    Serial.println();
    server.send(400, "text/plain", "bad request");
    digitalWrite(led, 0);
}

void handleNotFound(){
    digitalWrite(led, 1);
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET)?"GET":"POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i=0; i<server.args(); i++){
        message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
    }
    server.send(404, "text/plain", message);
    digitalWrite(led, 0);
}

//*************
//* utilities *
//*************
#define countof(a) (sizeof(a) / sizeof(a[0]))

String formatJSON(const JsonObject& obj) {
    char buffer[1000];
    obj.printTo(buffer, sizeof(buffer));
    String strOut = buffer;
    return strOut;
}

String printTemperature() {
    RtcTemperature temp = Rtc.GetTemperature();
    char tmp[10];
    dtostrf(temp.AsFloat(),1,2,tmp);
    String strOut = tmp;
    return strOut + " C";
}

String printDateTime(const RtcDateTime& dt) {
    return printDate(dt) + " " + printTime(dt);
}

String printDate(const RtcDateTime& dt) {
    char datestring[11];

    snprintf_P(datestring,
            countof(datestring),
            PSTR("%02u/%02u/%04u"),
            dt.Day(),
            dt.Month(),
            dt.Year()
            );
    //Serial.println("printDate:" + (String)datestring);
    return datestring;
}

String printTime(const RtcDateTime& dt) {
    char timestring[9];

    snprintf_P(timestring,
            countof(timestring),
            PSTR("%02u:%02u:%02u"),
            dt.Hour(),
            dt.Minute(),
            dt.Second()
            );
    //Serial.println("printTime:" + (String)timestring);
    return timestring;
}

String transDate(String localDate) {
    //Serial.println("date initiale -> " + localDate);
    String _day = localDate.substring(0,2);
    //Serial.println("date initiale -> dd  =" + _day);
    String _month = localDate.substring(3,5);
    //Serial.println("date initiale -> mm  =" + _month);
    String _year = localDate.substring(6,10);
    //Serial.println("date initiale -> yyyy=" + _year);

    switch(_month.toInt()) {
        case 1:
            _month = "Jan";
            break;
        case 2:
            _month = "Feb";
            break;
        case 3:
            _month = "Mar";
            break;
        case 4:
            _month = "Apr";
            break;
        case 5:
            _month = "May";
            break;
        case 6:
            _month = "Jun";
            break;
        case 7:
            _month = "Jul";
            break;
        case 8:
            _month = "Aug";
            break;
        case 9:
            _month = "Sep";
            break;
        case 10:
            _month = "Oct";
            break;
        case 11:
            _month = "Nov";
            break;
        case 12:
            _month = "Dec";
            break;
    }

    String result = _month + " " + _day + " " + _year;
    //Serial.println("date finale -> " + result);
    return result;
}

void setNTPTime() {
    // ressort si le ntp n'est pas activé
    if(!ntpActive) return;

    //get a random server from the pool
    WiFi.hostByName(ntpServerName, timeServerIP);

    sendNTPpacket(timeServerIP); // send an NTP packet to a time server
    // wait to see if a reply is available
    delay(1000);

    int cb = udp.parsePacket();
    if (!cb) {
        Serial.println("no packet yet");
    } else {
        // We've received a packet, read the data from it
        udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

        // the timestamp starts at byte 40 of the received packet and is four bytes,
        // or two words, long. First, esxtract the two words:
        unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
        unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);

        // combine the four bytes (two words) into a long integer
        // this is NTP time (seconds since Jan 1 1900):
        unsigned long secsSince1900 = highWord << 16 | lowWord;
        //Serial.println(secsSince1900);

        // now convert NTP time into everyday time:
        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        const unsigned long seventyYears = 2208988800UL;

        // calcul du timezone
        unsigned long localTimezone = timezone * 3600;

        // subtract seventy years:
        unsigned long epoch = secsSince1900 - seventyYears + localTimezone;

        // calcul de l'heure d'été
        RtcDateTime timeToTest;
        timeToTest.InitWithEpoch32Time(epoch);
        unsigned long epochMar = epochBeginDaylight(timeToTest.Year());
        unsigned long epochOct = epochEndDaylight(timeToTest.Year());
        if(epoch > epochMar) {
            if(epoch < epochOct) {
                epoch += 3600;
            }
        }

        // update DS3231 Time
        //Serial.println(epoch);
        RtcDateTime timeToSet;
        timeToSet.InitWithEpoch32Time(epoch);
        Serial.println("NTP DateTime:" + printDateTime(timeToSet));
        Rtc.SetDateTime(timeToSet);
    }
}

// send an NTP request to the time server at the given address
unsigned long sendNTPpacket(IPAddress& address)
{
    //Serial.println("sending NTP packet...");
    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49;
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;

    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    udp.beginPacket(address, 123); //NTP requests are to port 123
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
}

unsigned long epochBeginDaylight(int year) {
    String marDate = "Mar 31 " + String(year);
    String marTime = "01:00:00";
    RtcDateTime timeToGet = RtcDateTime(marDate.c_str(), marTime.c_str());
    timeToGet = timeToGet - timeToGet.DayOfWeek() * 86400;
    //Serial.println("endMar:" + printDateTime(timeToGet));
    return timeToGet.Epoch32Time();
}

unsigned long epochEndDaylight(int year) {
    String octDate = "Oct 31 " + String(year);
    String octTime = "01:00:00";
    RtcDateTime timeToGet = RtcDateTime(octDate.c_str(), octTime.c_str());
    timeToGet = timeToGet - timeToGet.DayOfWeek() * 86400;
    //Serial.println("endOct:" + printDateTime(timeToGet));
    return timeToGet.Epoch32Time();
}

void controlRelay(int mode) {
    switch(mode) {
        case _modeEco:
            Serial.println("mode economique");
            digitalWrite(RelayPin1, LOW);
            digitalWrite(RelayPin2, LOW);
            break;
        case _modeOff:
            Serial.println("mode eteint");
            digitalWrite(RelayPin1, HIGH);
            digitalWrite(RelayPin2, LOW);
            break;
        case _modeHGe:
            Serial.println("mode hors gel");
            digitalWrite(RelayPin1, LOW);
            digitalWrite(RelayPin2, HIGH);
            break;
        case _modeCom:
            Serial.println("mode confort");
            digitalWrite(RelayPin1, HIGH);
            digitalWrite(RelayPin2, HIGH);
            break;
        default:
            Serial.println("mode inconnu");
            break;
    }
}

String printMode(int mode) {
    String result = "inconnu";
    switch(mode) {
        case _modeEco:
            Serial.println("mode economique");
            result = "Economique";
            break;
        case _modeOff:
            Serial.println("mode eteint");
            result = "Eteint";
            break;
        case _modeHGe:
            Serial.println("mode hors gel");
            result = "Horsgel";
            break;
        case _modeCom:
            Serial.println("mode confort");
            result = "Confort";
            break;
        default:
            Serial.println("mode inconnu");
            result = "Inconnu";
            break;
    }
    return result;
}

String printAmps() {
    RawValue = analogRead(ConsoPin);
    double voltage = (RawValue / 1024.0) * 5000; // Gets you mV
    double mAmps = ((voltage - ACSoffset) / mVperAmp);
    String value = "";
    String unit = "";
    if(mAmps > 10000) {
      value = String(round(mAmps/1000));
      unit = "A";
    } else if(mAmps > 1000) {
      value = String(round(mAmps/100)/10);
      unit = "A";
    } else {
      value = String(mAmps);
      unit = "mA";
    }
    return value + " " + unit;
}

void saveSettings() {
    Serial.println("saving config");
    jsonBuffer.clear();
    JsonObject& json = jsonBuffer.createObject();
    //Serial.println("encodage JSON");
    json["timezone"] = timezone;
    json["daylight"] = daylight;
    json["ntpactive"] = ntpActive;
    json["ntpservername"] = ntpServerName;
    json["ntprefreshinterval"] = intervalNTP;
    json["relaymode"] = relayMode;
    //json.prettyPrintTo(Serial);
    //Serial.println();

    File configFile = SPIFFS.open("/settings.json", "w");
    if (!configFile) {
        Serial.println("file open failed");
    } else {
        //Serial.println("ecriture fichier");
        json.printTo(configFile);
    }
    //Serial.println("fermeture dufichier");
    configFile.close();
    Serial.println("saving done");
}

void loadSettings() {
    Serial.println("loading config");
    //creation du fichier avec les valeurs par default si il n'existe pas
    if(!SPIFFS.exists("/settings.json")) {
        //Serial.println("fichier absent -> creation");
        saveSettings();
    }

    //lecture des données du fichier
    File configFile = SPIFFS.open("/settings.json", "r");
    if(!configFile) {
        Serial.println("file open failed");
    } else {
        //Serial.println("lecture fichier");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);

        //Serial.println("décodage JSON");
        jsonBuffer.clear();
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        //json.prettyPrintTo(Serial);
        //Serial.println();
        if (json.success()) {
            //Serial.println("parsing dans les variables");
            timezone = json["timezone"];
            daylight = json["daylight"];
            ntpActive = json["ntpactive"];
            strcpy(ntpServerName, json["ntpservername"]);
            intervalNTP = json["ntprefreshinterval"];
            relayMode = json["relaymode"];
        }
    }
    //Serial.println("fermeture dufichier");
    configFile.close();
    Serial.println("loading done");
}
