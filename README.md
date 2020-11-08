# arduino_garage_opener
A simple garage opener (momentary relay contact) based on ESP2866

## HW Requirements
Compiled for WEMOS D1 mini (you might have to change pin settings)
- 5V Relay on `relayPin`
- 2 IR sensors (cheap ones like https://www.aliexpress.com/i/4000026440924.html work ok, you have to fiddle with sensitivity)
- DS18B20 temperature sensor
  
## SW Requirements
- MQTT used to log temperature and to wait for open/close actions. I ran a Mosquitto server on a separate raspberry pi in the same network
- IFTTT used to notify phone or to use a trigger action to open/close garage (e.g. I arrive home -> open garage)
- WiFi for a simple web page with a push button and status page and general connectivity for MQTT and IFTTT

## Non common libaries used
- PubSubClient for MQTT
- OneWire and DallasTemperature for the DS18B20 temperature sensor 
- Ticker to update things on a schedule (temperature readings, status of garage door)
- simpleDSTadjust to change DST based on my location

## HomeBridge Temperature
To make the temperature readings available in HomeBridge, you can use this:

```
{
  "accessory": "mqttthing",
  "caption": "WorksShop temeperature",
  "name": "Temp Workshop",
  "topics": {
    "getCurrentTemperature": {
    "apply": "return (message.toString().split(';')[3].replace('}','')-32)/1.8;",
      "topic": "sensors/mydevicename/temp"
    }
  },
  "type": "temperatureSensor",
  "url": "http://mymqttserver:1883"
},
```
