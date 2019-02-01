# Sonoff-SmartThings-Compatible-Firmware #
Firmware for the sonoff basic r2 that adds presence sensing capability and the possibility of SmartThings connectivity

UNDER DEV, variable persistance untested

NOTE:   This firmware is written for the Sonoff Basic R2. The R2 uses an ESP8285 and has different pin definitions compared to the R1.
	This code needs to have its pin defintions modified and be compilied for the R1's esp chip, to be compatible with the 
	Sonoff R1 basic.

### Setup ###

To flash the sonoff basic r2, power it on while holding the button and the flash the firmware. Whether you are using the arduino IDE, 
esptool.py or some other flashing tool ensure that you have configured your tool for flashing an ESP8285.

Upon boot a successfull flash is designated by a quick double flash of the onboard LED.

At this point the device will attempt to connnect to any wifi networks configured in memory. If it cannot it will create a wireless
AP with an SSID of Sonoff%MAC_ADDR%. Log onto the AP and using the captive portol configure a connection to you home AP.
If the portol times out the sonoff will flash once.

The device will now begin SSDP advertisements.

Now you can use the onboard button. A short press (\< 3 secs) will toggle the relay and a long press (\> 3 secs) will reset the devices 
WiFi settings.  

### Rest API Commands ###

To use the devices full funtionality you must interact with a HTTP based API.
To access the devices API you must use a URL in the form:
http://deviceip/ \<command and arguments\>

The commands are as follows:

  ```/``` \
  gets a report in JSON form

  ```/on``` \
  raises the relay pin high (closes the relay)

  ```/off``` \
  lowers the relay pin to low (opens the relay)

  ```/new?name=<name of person to assosiate with device>&hostname=<the hostname or ip address of there main device>``` \
  adds a new sensor

  ```/force?name=<name of person assosiated with device>&state=<the new state>``` \
  forces a sensor to a particular state, the state can be 1, 0, true or false

  ```/report``` \
  gets a report in JSON form

  ```/reset``` \
  factory resets the device, erases ALL configuration

  ```/mode?mode=<0 or 1>&freq=<time between checks in secs>&hyst=<no of checks before a person is reported to be absent>&btn=<1 or 0,1 gives direct control of the relay to the button, 0 doesnt>``` \
  sets the report mode; 0 is every check, 1 is only on changes, along with other configuration options for instance check frequency,   	  hysterisis and button configuration.

  ```/addr?ip=<IP address>&port=<port number>``` \
  configures the address that reports will be sent to. &ip=none&port=none will disable reports, disabled by default
  
  ```/clear``` \
  deletes all the current presence sensors
  
  ```/gpio?state=<1 for high, 0 for low>``` \
  switch the broken out GPIO pin
  
  ```/led?state=<1 for on, 0 for off>``` \
  turn on and off the onborad led

To setup the device properly I suggest the following steps:
1. add each presence sensor with /new, run the command for each new sensor
2. set the unsolicited report address with /addr
3. set the report mode with /mode

### Message Format ###

Naturaly the device will send messages. All of these messages are sent using HTTP, the method with which they are sent depends on weather they are solicited or not.

Solicited responses are responses to user commands, they are sent as answers to the HTTP requests that make up the commands. Where as unsolicited responses are sent when an event occurs, they are sent via a HTTP post request to IP address and port number configured in the /addr command.

**Command Success Message**
```JSON
{"OK":"<0 indicating false, or 1 indicating true>", "cmd":"<command sent - for example 'new' or 'on'>"}
```
This is sent to idicate the success or failure of a command. 

**Report Message**
```JSON
{
"sensors":{
	"sensor0":{
		"name":"<the name assosciated with the sensor>",
		"hostName":"<the hostname or IP address of the device the sensor looks for>",
		"state":"<in or out, depending on the prsenceof the device>"},
	"sensor1":{
		"name":"<the name assosciated with the sensor>",
		"hostName":"<the hostname or IP address of the device the sensor looks for>",
		"state":"<in or out, depending on the prsenceof the device>"}
},
"config":{
 	"relay":"<1 or 0, 1 indicates relay is closed and 0 indicates the relay is open>",
	"freq":"<the nuber of seconds between each report>",
	"hyst":"<the number of checks that each person must fail to report before absence in confirmed>",
	"mode":"<1 or 0, 1 indicates report only one state change and 0 indicates a report per check>",
	"btn":"<1 or 0, 1 indicates that the onboard button directly controls the relay>"}
}
```
This is sent upon the request for a report, when a report is triggered by a state change or simply a check. Each active sensor will be listed.

**Button Event Message**
```JSON
{"button":"<up or dwn>"}
```
This is sent when a button event occurs on the board, it is only ever unsolicited.
