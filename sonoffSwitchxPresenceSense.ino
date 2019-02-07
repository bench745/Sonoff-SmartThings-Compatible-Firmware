/* This code is for the sonoff basic r2. Adds presence sensing capabilities to the smart switch.
 * The code has a number of dependencies; WiFiManager, ESP8266Ping (https://github.com/dancol90/ESP8266Ping), ESP866HTTPClient (all availible in manage libraries) and the ESP8266 board definition
 * The sketch should be compiled with the board definition: Generic ESP8285 Module, 80 MHz, Flash, Enabled, ck, 26 MHz, 1M (no SPIFFS), 2, V2 Lower Memory, Disabled, None, Only Sketch, 115200 on COM%number% 
 * 
 */



#include <ESP8266WiFi.h>          // ESP8266 Core WiFi Library (you most likely already have this in your sketch)
#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <ESP8266SSDP.h>          // used for service advertisements
#include <ESP8266HTTPClient.h>    // used for posting data to smart things
#include <WiFiManager.h>          // WiFi Configuration
#include <ESP8266Ping.h>          // used for the ping 
#include <EEPROM.h>               // used to pickle the config details

extern "C"
{
  #include <lwip/icmp.h> // needed for icmp packet definitions
}

// the smarthings IP, hard coding will be replaced with ssdp advertisment method of procurement
String smartThings = "http://none:none";

// the pin defintions
const int button = 0;
const int gpio = 2;
const int relay = 12;
const int led = 13;

// global variables
bool recording = false;  // false indicates that the esp is not timing a button press 
int relayState = 0;  // 0 indicates that the relay is open
char ssid[19]= "sonoff";

unsigned int frequency = 180;  // the number of seconds between presence checks
unsigned short hysterisis = 2;  // the minimum number of cyles someone unaccounted for before a sensor is given the absent value
int reportMode = 0;  // 0 - report on every check, 1 - report only when a presence state changes
int btnToRelay = 1;  // 0 - button doesnt directly control relay, 1 - button does control relay

const unsigned short maxNumberOfSensors = 32;  // the maximum number of sensors
unsigned short used = 0;  // record the number of allocated sensors
char names[maxNumberOfSensors][11] = {};  // the names assosiated with each sensor max len 10 chars
char hostnm[maxNumberOfSensors][25] = {};  // the the host names associated with each sensor in the form of a char array 
bool states[maxNumberOfSensors] = {};  // the state of each sensor. true - present, false - absent
bool dynamicStates[maxNumberOfSensors] = {};  // the dynamic states of the presence accurate at the current time, only transfered to states after the hysterisis period is complete
unsigned short timeUnaccounted[maxNumberOfSensors] = {};  // record the time that a presence indicatior has been unaccounted for


WiFiManager wifiManager;
ESP8266WebServer server(80);

//////////////////////////////////////////////////////////////
// data utility fuctions

String buildJSON(){
  String JSON = "";
  (used > 0) ? (JSON += "{\"sensors\":{") : ("{");
  for (int i = 0; i < used; i++){
    JSON += "\"sensor";
    JSON.concat(i);
    JSON += "\": {\"name\": \"";
    JSON.concat(names[i]);
    JSON += "\", \"hostName\": \"";
    JSON.concat(hostnm[i]);
    JSON += "\", \"state\": \"";
    (states[i]) ? (JSON += "in") : (JSON += "out");
    JSON += "\"}";
    (i < (used -1)) ? (JSON += ", ") : (JSON += "}, "); 
  }

 JSON += "\"config\": {\"relay\": \"";
 JSON.concat(relayState);
 JSON += "\", \"freq\": \"";
 JSON.concat(frequency);
 JSON += "\", \"hyst\": \"";
 JSON.concat(hysterisis);
 JSON += "\", \"mode\": \"";
 JSON.concat(reportMode);
 JSON += "\", \"btn\": \"";
 JSON.concat(btnToRelay);
 JSON += "\"}}";

 return JSON; 
}


void save(){
  // save to EEPROM:
  // - frequency
  // - hysterisis
  // - used
  // - names
  // - hostnm
  // - reportMode
  // and set a bit before all of these to 1 inicating saved variables
  Serial.println("saving variables");
  
  unsigned short flag = 1;
  int addr = 0;
  EEPROM.put(addr, flag);
  addr += sizeof(flag);  // start from an offset of the size of the data to be read byte
  EEPROM.put(addr, frequency);  // put the frequency at addr 
  addr += sizeof(frequency);  // ajust the address we want to update
  EEPROM.put(addr, hysterisis);  
  addr += sizeof(hysterisis);
  EEPROM.put(addr, used);
  addr += sizeof(used);
  EEPROM.put(addr, names);
  addr += sizeof(names);
  EEPROM.put(addr, hostnm);
  addr += sizeof(hostnm);
  EEPROM.put(addr, reportMode);
  addr += sizeof(reportMode);
  EEPROM.put(addr, btnToRelay);
}


void erase(){
  // set the bit preceeding the saved variables to 0 indicating that they shouldn't be read
  Serial.println("erasing variables");
  
  unsigned short isdata = 0;
  EEPROM.put(0, isdata);
}

void load(){
  // read variables from the EEPROM
  Serial.println("loading variables");
  
  int addr = sizeof(unsigned short);  // start from an offset of the size of the data to be read byte
  EEPROM.get(addr, frequency);  // get the frequency 
  addr += sizeof(frequency);  // ajust the address we want to read
  EEPROM.get(addr, hysterisis);  
  addr += sizeof(hysterisis);
  EEPROM.get(addr, used);
  addr += sizeof(used);
  EEPROM.get(addr, names);
  addr += sizeof(names);
  EEPROM.get(addr, hostnm);
  addr += sizeof(hostnm);
  EEPROM.get(addr, reportMode);
  addr += sizeof(reportMode);
  EEPROM.get(addr, btnToRelay);

  for (int i = 0; i < used; i++){
    states[i] = false;
    dynamicStates[i] = false;
  }
}

//////////////////////////////////////////////////////////////
// http handlers

void clearSensors(){
  used = 0;  // record the number of allocated sensors
  names[maxNumberOfSensors][11] = {};  // the names assosiated with each sensor max len 10 chars
  hostnm[maxNumberOfSensors][25] = {};  // the the host names associated with each sensor in the form of a char array 
  states[maxNumberOfSensors] = {};  // the state of each sensor. true - present, false - absent
  dynamicStates[maxNumberOfSensors] = {};  // the dynamic states of the presence accurate at the current time, only transfered to states after the hysterisis period is complete
  timeUnaccounted[maxNumberOfSensors] = {};  // record the time that a presence indicatior has been unaccounted for

  erase();
  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"clear\"}");
}


void ledCtrl(){
  for (int i = 0; i < server.args(); i++){
    if (server.argName(i) == "state" and server.arg(i) == "1"){
      digitalWrite(led, LOW);
      Serial.print("turning on LED (");
      Serial.print(led);
      Serial.println(")");
      server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"led on\"}");
    }else if(server.argName(i) == "state" and server.arg(i) == "0"){
      digitalWrite(led, HIGH);
      Serial.print("turning off LED (");
      Serial.print(led);
      Serial.println(")");
      server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"led off\"}");
    }
  }
}


void gpioCtrl(){
  for (int i = 0; i < server.args(); i++){
    if (server.argName(i) == "state" and server.arg(i) == "1"){
      digitalWrite(led, HIGH);
      Serial.println("turning on GPIO");
      server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"gipo on\"}");
    }else if(server.argName(i) == "state" and server.arg(i) == "0"){
      digitalWrite(led, LOW);
      Serial.println("turning off GPIO");
      server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"gipo off\"}");
    }
  }
}


void relayOpen(){
  Serial.println("opening relay");
  
  digitalWrite(relay, LOW);
  relayState = 0;
  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"off\"}");
}

void relayClose(){
  Serial.println("closing relay");
  
  digitalWrite(relay, HIGH);
  relayState = 1;
  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"on\"}");
}


void newSens(){
  // add a new state sensor
  Serial.println("adding sensor");
  
  char nm[11];
  char hnm[25];
  
  for (int i = 0; i < server.args(); i++){
    if (server.argName(i) == "name" and server.arg(i).length() < 11){
      char buf[server.arg(i).length() + 1];
      server.arg(i).toCharArray(buf, sizeof(buf));
      strcpy(nm, buf);
    } else if (server.argName(i) == "hostname" and server.arg(i).length() < 25){
      char buf[server.arg(i).length() + 1];
      server.arg(i).toCharArray(buf, sizeof(buf));
      strcpy(hnm, buf);
    }
  }

  if (strlen(nm) > 0 and strlen(hnm) > 0 and used < maxNumberOfSensors){
    strcpy(names[used], nm);
    strcpy(hostnm[used], hnm);
    states[used] = false;
    dynamicStates[used] = false;
    timeUnaccounted[used] = 0;
    used++;
    save();
    server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"new sensor\"}");
  } else {
    server.send(200, "application/json", "{\"OK\":\"0\",\"cmd\":\"force state\"}");
  }
  
}

void forcestate(){
  // force the state of a particular sensor
  Serial.println("forcing state");
  
  bool newState;  // stores state to be forced
  char snm[11];  // the name of the account to force
  
  for (int i = 0; i < server.args(); i++){
    if (server.argName(i) == "state" and (server.arg(i) == "true" or server.arg(i) == "false" or server.arg(i) == "1" or server.arg(i) == "0")){
      if (server.arg(i) == "true" or server.arg(i) == "1"){
        newState = true;
      } else if (server.arg(i) == "false" or server.arg(i) == "0"){
        newState = false;
      }
    } else if (server.argName(i) == "name" and server.arg(i).length() < 25){
      char buf[server.arg(i).length() + 1];
      server.arg(i).toCharArray(buf, sizeof(buf));
      strcpy(snm, buf);
    }
  }

  for (int i = 0; i < used; i++){
    if (names[i] == snm){
      states[i] = newState;
      dynamicStates[i] = newState;
    }
  }

  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"force state\"}");
}
  
  
void report(){
  // report the presence sensor states and config variables to smart things via a http response
  Serial.println("reporting (solicited)");
  
  server.send(200, "application/json", buildJSON());
}


void factoryreset(){
  // put the sonoff into a "new flash" state
  Serial.println("factory reset");
  
  wifiManager.resetSettings();
  erase();
  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"factory reset\"}");
  ESP.restart();
  delay(5000);
}


void setmode(){
  // allow a choice between reports every n minutes and reports only on changes
  // allow a minimum of m seconds disconnection before deeming a device disconnected
  Serial.println("setting mode");
  
  for (int i = 0; i < server.args(); i++){
    if (server.argName(i) == "mode"){
      char buf[server.arg(i).length() + 1];
      server.arg(i).toCharArray(buf, sizeof(buf));
      reportMode = atoi(buf);
    } else if (server.argName(i) == "freq"){
      char buf[server.arg(i).length() + 1];
      server.arg(i).toCharArray(buf, sizeof(buf));
      frequency = atoi(buf);
    } else if (server.argName(i) == "hyst"){
      char buf[server.arg(i).length() + 1];
      server.arg(i).toCharArray(buf, sizeof(buf));
      hysterisis = atoi(buf);
    } else if (server.argName(i) == "btn"){
      if (server.arg(i) == "1"){
        btnToRelay = 1;
      }else if (server.arg(i) == "0"){
        btnToRelay = 0;
      }
    }
    
  }
  save();

  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"set mode\"}");
}


void STaddr(){
  Serial.println("setting report addr");
  smartThings = "http://" + server.arg("ip");
  smartThings += ":";
  smartThings += server.arg("port");
  server.send(200, "application/json", "{\"OK\":\"1\",\"cmd\":\"set addr\"}");
}


//////////////////////////////////////////////////////////////
// IO functions

void longPress(){
  wifiManager.resetSettings();
  wifiManager.setTimeout(300);
  if (!wifiManager.autoConnect(ssid, "configme")){
    digitalWrite(led, HIGH);
    delay(500);
    digitalWrite(led, LOW);
    ESP.restart();
    delay(5000);
  }
}


void sense(){
  // sense states 
  Serial.println("checking presence ...");

  bool changed = false;  // have we changed states on this check
  
  for (int i = 0; i < used; i++){
    server.handleClient();
    
    bool in = Ping.ping(hostnm[i]);
    
    if (in == true and states[i] == false){  // if they've registed in and if they were out 
      changed = true;
      states[i] = true;
      dynamicStates[i] = true;
      timeUnaccounted[i] = 0;
      Serial.print("  ");
      Serial.print(names[i]);
      Serial.println(" is responding");
      
    } else if (in == false){  // if they registered as out and were in
      dynamicStates[i] = false;
      timeUnaccounted[i] ++;

      Serial.print("  ");
      Serial.print(names[i]);
      Serial.println(" is not responding");
      
      if (timeUnaccounted[i] >= (hysterisis + 1) and states[i] == true){
        states[i] = false;
        changed = true; 
        
        Serial.print("  ");
        Serial.print(names[i]);
        Serial.println(" is now considered out");
      }
    }
  }

  // report when nessisary
  if (changed and reportMode == 1){
    restReport(buildJSON());
  } else if (reportMode == 0){
    restReport(buildJSON());
  }
}

void restReport(String data){
  // report all the data with out request 
  
  if (smartThings != "http://none:none"){
    Serial.println("reporting (unsolicited)");
    HTTPClient http;
    http.begin(smartThings);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(data);
    http.end();
  }
}


//////////////////////////////////////////////////////////////
// main functions

void setup() {
  // set pins
  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH);
  pinMode(relay, OUTPUT);
  digitalWrite(relay, LOW);

  pinMode(button, INPUT);

  Serial.begin(115200);
  
  // check the first byte of eeprom, if it is a one load vars else, dont.
  unsigned short flag;
  EEPROM.get(0, flag);
  if (flag == 1){
    load();
  }
  

  // indicate succesfull power on
  digitalWrite(led, LOW);
  delay(500);
  digitalWrite(led, HIGH);
  delay(500);
  digitalWrite(led, LOW);
  delay(500);
  digitalWrite(led, HIGH);

  Serial.println("booted");
  
  // create a unique ssid for the sonoff switch
  String mac = WiFi.macAddress();
  char buf[mac.length()+1];
  mac.toCharArray(buf, sizeof(buf));
  strcat(ssid, buf);

  // set the config AP turn off after 10 mins
  wifiManager.setTimeout(600);
  
  //wifiManager.setSaveConfigCallback(saveConfigCallback);

  Serial.print("trying to connect to wifi ... ");

  // auto connect, on fail lauch config portol, if time out hit returns false
  if (!wifiManager.autoConnect(ssid, "configme")){
    Serial.println("failed");
    digitalWrite(led, LOW);
    delay(500);
    digitalWrite(led, HIGH);
    ESP.reset();
    delay(5000);
  }

  Serial.println("Connected");

  Serial.print("configuring server ... ");

  // setup the server for the presence sensor and relay control API 
  server.on("/on", relayClose);  // relay control
  server.on("/off", relayOpen);
  server.on("/new", newSens);  // add a new presence sensor
  server.on("/force", forcestate);  // force a state for a given sensor
  server.on("/report", report);  // report the current presence state
  server.on("/reset", factoryreset);  // factoy reset the module, require a password?
  server.on("/mode", setmode);  // allow a choice of periodic reports, or of reports only upon a change, allow the config of a minimum time disconected before a device is considered disconnected, allow configuartion of a delay between checks 
  server.on("/addr", STaddr);  // allow the congfig of an address to report to
  server.on("/clear", clearSensors);  // clear all of the sensors
  server.on("/led", ledCtrl);  // change the state of the led
  server.on("/gpio", gpioCtrl);  // change the stste of the GPIO pin
  server.on("/", report);
  
  // setup the server for SSDP
  server.on("/description.xml", HTTP_GET, []() {SSDP.schema(server.client());});

  // setup the server for 404 page not found
  server.onNotFound([](){
    server.send(404, "text/plain", "PAGE NOT FOUND");
  });

  Serial.println("Done");

  Serial.print("setting up SSDP ...");

  //setup the SSDP details
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("Sonoff");
  SSDP.setSerialNumber(ESP.getChipId());
  SSDP.setURL("index.html");
  SSDP.setModelName("Sonoff Compound");
  SSDP.setModelNumber("Sonoff_SL");
  SSDP.setModelURL("http://smartlife.tech");
  SSDP.setManufacturer("Smart Life Automated");
  SSDP.setManufacturerURL("http://smartlife.tech");

  Serial.println("Done");

  Serial.print("begining HTTP server and SSDP ... ");
  
  server.begin();
  SSDP.begin();

  Serial.println("Done");
}


void loop() {
  // deal with any button presses
  static unsigned long dwn = 0;
  static bool checked = false;
  
  int state = digitalRead(button);
  
  if (state == LOW and recording == false){  // if the buttoin has been pressed
    dwn = millis();  // the time that the button was last pressed
    recording = true;
    restReport("{\"button\":\"dwn\"}");
    
    Serial.println("btn dwn");
    
  }else if (state == HIGH and recording == true){  // if the button has been released
    unsigned long up = millis();
    recording = false;
    restReport("{\"button\":\"up\"}");

    Serial.print("btn up: ");
    Serial.print(up);
    Serial.print(", ");
    Serial.print(dwn);
    Serial.print(": ");
    Serial.println(up - dwn);

    if ((up - dwn) < 4500){  // if the press was less than 4.5 seconds, and therefore deemed short
      // toggle the relay
      Serial.println("short press");
      //restReport(buildJSON());

      if (btnToRelay == 1){
        digitalWrite(relay, !relayState);
        relayState = !relayState;
      }
    }
    else {
      // launch the longPress funct, normaly opens the config AP
      Serial.println("long press");
      longPress();
    }
    dwn = 0;
  }

  //logic for checking presence periodicaly
  int t = round(millis()/1000);
  if ((t % frequency) == 0 and checked == false){
    sense();
    checked = true;
  } else if ((t % frequency) != 0 and checked == true){
    checked = false;
  }

  // deal with web requests
  server.handleClient();
}
