/* This code is for the sonoff basic r2. Adds presence sensing capabilities to the smart switch.
 * The code has a number of dependencies; WiFiManager, Pinger, ESP866HTTPClient (all availible in manage libraries) and the ESP8266 board definition
 * The sketch should be compiled with the board definition: Generic ESP8285 Module, 80 MHz, Flash, Enabled, ck, 26 MHz, 1M (no SPIFFS), 2, V2 Lower Memory, Disabled, None, Only Sketch, 115200 on COM%number% 
 * 
 */



#include <ESP8266WiFi.h>          // ESP8266 Core WiFi Library (you most likely already have this in your sketch)
#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <ESP8266HTTPClient.h>    // used for posting data to smart things
#include <WiFiManager.h>          // WiFi Configuration
#include <Pinger.h>               // easy pinging
#include <EEPROM.h>               // used to pickle the config details

extern "C"
{
  #include <lwip/icmp.h> // needed for icmp packet definitions
}

// the smarthings IP, hard coding will be replaced with ssdp advertisment method of procurement
String smartThings = "http://192.168.1.119:39500";

// the pin defintions
int button = 0;
int gpio = 2;
int relay = 12;
int led = 13;

// global variables
bool recording = false;  // false indicates that the esp is not timing a button press 
int relayState = 0;  // 0 indicates that the relay is open
char ssid[19]= "sonoff";

unsigned int frequency = 180;  // the number of seconds between presence checks
unsigned short hysterisis = 2;  // the minimum number of cyles someone unaccounted for before a sensor is given the absent value
int reportMode = 0;  // 0 - report on every check, 1 - report only when a presence state changes

const unsigned short maxNumberOfSensors = 32;  // the maximum number of sensors
unsigned short used = 0;  // record the number of allocated sensors
char names[maxNumberOfSensors][11] = {};  // the names assosiated with each sensor max len 10 chars
char hostnm[maxNumberOfSensors][25] = {};  // the the host names associated with each sensor in the form of a char array 
bool states[maxNumberOfSensors] = {};  // the state of each sensor. true - present, false - absent
bool dynamicStates[maxNumberOfSensors] = {};  // the dynamic states of the presence accurate at the current time, only transfered to states after the hysterisis period is complete
unsigned short timeUnaccounted[maxNumberOfSensors] = {};  // record the time that a presence indicatior has been unaccounted for


WiFiManager wifiManager;
ESP8266WebServer server(80);
Pinger pinger;


//////////////////////////////////////////////////////////////
// data utility fuctions

String getItem(String csv, int feild, char delim){
  int start = 0;
  int fin = csv.indexOf(delim, 0);

  if (feild == 0){
    return csv.substring(0, fin);
  }
  if (fin == -1){
    return csv;
  }
  for (int i = 0; i < feild; i++){
    start = csv.indexOf(delim, fin);
    fin = csv.indexOf(delim, start + 1);
  }
  
  return (fin == -1) ? csv.substring(start + 1) : csv.substring(start + 1, fin);   
}


String buildJSON(){
  String JSON = "{";
  for (int i = 0; i < used; i++){
    JSON += "\"sensor";
    JSON.concat(i);
    JSON += "\": {\"name\": \"";
    JSON.concat(names[i]);
    JSON += "\", \"hostName\": \"";
    JSON.concat(hostnm[i]);
    JSON += "\", \"state\": \"";
    (states[i]) ? (JSON += "in") : (JSON += "out");
    JSON += "\"}, ";
  }

 JSON += "\"config\": {\"relay\": \"";
 JSON.concat(relayState);
 JSON += "\", \"frequency\": ";
 JSON.concat(frequency);
 JSON += "\", \"hysterisis\": ";
 JSON.concat(hysterisis);
 JSON += "\", \"mode\": ";
 JSON.concat(reportMode);
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
}


void erase(){
  // set the bit preceeding the saved variables to 0 indicating that they shouldn't be read
  unsigned short isdata = 0;
  EEPROM.put(0, isdata);
}

void load(){
  // read variables from the EEPROM
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

  for (int i = 0; i < used; i++){
    states[i] = false;
    dynamicStates[i] = false;
  }
}

//////////////////////////////////////////////////////////////
// http handlers

void newSens(){
  // add a new state sensor
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
    save();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(200, "text/plain", "ERROR");
  }
  
}

void forcestate(){
  // force the state of a particular sensor
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
}
  
  
void report(){
  // report the presence sensor states and config variables to smart things via a http response
  server.send(200, "application/json", buildJSON());
}


void factoryreset(){
  // put the sonoff into a "new flash" state
  wifiManager.resetSettings();
  erase();
  server.send(200, "text/plain", "OK");
  ESP.restart();
  delay(5000);
}


void setmode(){
  // allow a choice between reports every n minutes and reports only on changes
  // allow a minimum of m seconds disconnection before deeming a device disconnected
  
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
    }
  }
  save();

  server.send(200, "text/plain", "OK");
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
  bool changed = false;  // have we changed states on this check
  
  for (int i = 0; i < used; i++){
    bool in = pinger.Ping(hostnm[i]);
    if (in == true and states[i] == false){  // if they've registed in and if they were out 
      changed = true;
      states[i] = true;
      dynamicStates[i] = true;
      timeUnaccounted[i] = 0;
      
    } else if (in == false and states[i] == true){  // if they registered as out and were in
      dynamicStates[i] = false;
      timeUnaccounted[i] ++;
      
      if (timeUnaccounted[i] >= hysterisis){
        states[i] = false;
        changed = true; 
      }
    }
  }

  // report when nessisary
  if (changed and reportMode == 1){
    restReport();
  } else if (reportMode == 0){
    report();
  }
}

void restReport(){
  // report all the data with out request 
  HTTPClient http;
  http.begin(smartThings);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(buildJSON());
  http.end();
}


void setup() {
  // set pins
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);
  pinMode(relay, OUTPUT);
  digitalWrite(relay, LOW);

  pinMode(button, INPUT);
  
  // check the first byte of eeprom, if it is a one load vars else, dont.
  unsigned short flag;
  EEPROM.get(0, flag);
  if (flag == 1){
    load();
  }
  

  // indicate succesfull power on
  digitalWrite(led, HIGH);
  delay(500);
  digitalWrite(led, LOW);
  delay(500);
  digitalWrite(led, HIGH);
  delay(500);
  digitalWrite(led, LOW);

  // create a unique ssid for the sonoff switch
  String mac = WiFi.macAddress();
  char buf[mac.length()+1];
  mac.toCharArray(buf, sizeof(buf));
  strcat(ssid, buf);

  // set the config AP turn off after 10 mins
  wifiManager.setTimeout(600);
  
  //wifiManager.setSaveConfigCallback(saveConfigCallback);

  // auto connect, on fail lauch config portol, if time out hit returns false
  if (!wifiManager.autoConnect(ssid, "configme")){
    digitalWrite(led, HIGH);
    delay(500);
    digitalWrite(led, LOW);
    ESP.reset();
    delay(5000);
  }

  // setup the server for the presence sensor and relay control API 
  server.on("/new", newSens);  // add a new presence sensor
  server.on("/force", forcestate);  // force a state for a given sensor
  server.on("/report", report);  // report the current presence state
  server.on("/reset", factoryreset);  // factoy reset the module, require a password?
  server.on("/mode", setmode);  // allow a choice of periodic reports, or of reports only upon a change, allow the config of a minimum time disconected before a device is considered disconnected, allow configuartion of a delay between checks 
  server.on("/", report);
  server.onNotFound([](){
    server.send(404, "text/plain", "PAGE NOT FOUND");
  });


  // setup the pinger call backs
  pinger.OnReceive([](const PingerResponse& response){
    if (!response.ReceivedResponse){
      Serial.printf("Request timed out.\n");
      return false;
    }
    return true;
  });
  pinger.OnEnd([](const PingerResponse& response)
  {
    // Evaluate lost packet percentage
    float loss = 100;
    if(response.TotalReceivedResponses > 0)
    {
      loss = (response.TotalSentRequests - response.TotalReceivedResponses) * 100 / response.TotalSentRequests;
    }

    if(loss == 100){
      return false;
    }
    
    return true;
  });
  
  
}


void loop() {
  // deal with any button presses
  static unsigned long dwn = 0;
  int state = digitalRead(button);
  
  if (state == HIGH and recording == false){  // if the buttoin has been pressed
    dwn = millis();  // the time that the button was last pressed
    recording = true;
  }else if (state == LOW and recording == true){  // if the button has been released
    unsigned long up = millis();
    dwn = 0;
    recording = false;

    if ((up - dwn) < 3000){  // if the press was less than 3 seconds, and therefore deemed short
      // toggle the relay
      digitalWrite(relay, !relayState);
    }
    else {
      // launch the longPress funct, normaly opens the config AP
      longPress();
    }
  }

  //logic for checking presence periodicaly
  int t = round(millis()/1000);
  if ((t % frequency) == 0){
    sense();
  }

  // deal with web requests
  server.handleClient();
}
