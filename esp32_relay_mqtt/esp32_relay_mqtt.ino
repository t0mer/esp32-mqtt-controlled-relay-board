#include <WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>

// Replace with your network credentials
const char* ssid = "";  // Network SSID (name)
const char* password = "";  // Network password

// MQTT Broker details
const char* mqttServer = "";
const int mqttPort = 1883;
const char* mqttUser = "";
const char* mqttPassword = "";

// Initialize the WiFi and MQTT client objects
WiFiClient espClient;
PubSubClient client(espClient);

// Variables to store the current state of each device (ON/OFF)
String Device1State = "off";
String Device2State = "off";
String Device3State = "off";
String Device4State = "off";
String Device5State = "off";
String Device6State = "off";

// Assign each device to a GPIO pin
const int Device1 = 4;
const int Device2 = 5;
const int Device3 = 18;
const int Device4 = 19;
const int Device5 = 21;
const int Device6 = 22;

// Variable to enable or disable state saving
const bool saveState = true;

// EEPROM address to store the state
const int eepromSize = 6;

// Callback function for MQTT subscription
void callback(char* topic, byte* message, unsigned int length) {
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    messageTemp += (char)message[i];
  }

  // Print the message for debugging
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  Serial.println(messageTemp);

  // Check the received message and update the corresponding relay state
  if (String(topic) == "relay/device1") {
    if (messageTemp == "on") {
      Serial.println("Device 1 on");
      Device1State = "on";
      digitalWrite(Device1, LOW);
      if (saveState) EEPROM.write(0, 1);
    } else if (messageTemp == "off") {
      Serial.println("Device 1 off");
      Device1State = "off";
      digitalWrite(Device1, HIGH);
      if (saveState) EEPROM.write(0, 0);
    }
  } else if (String(topic) == "relay/device2") {
    if (messageTemp == "on") {
      Serial.println("Device 2 on");
      Device2State = "on";
      digitalWrite(Device2, LOW);
      if (saveState) EEPROM.write(1, 1);
    } else if (messageTemp == "off") {
      Serial.println("Device 2 off");
      Device2State = "off";
      digitalWrite(Device2, HIGH);
      if (saveState) EEPROM.write(1, 0);
    }
  } else if (String(topic) == "relay/device3") {
    if (messageTemp == "on") {
      Serial.println("Device 3 on");
      Device3State = "on";
      digitalWrite(Device3, LOW);
      if (saveState) EEPROM.write(2, 1);
    } else if (messageTemp == "off") {
      Serial.println("Device 3 off");
      Device3State = "off";
      digitalWrite(Device3, HIGH);
      if (saveState) EEPROM.write(2, 0);
    }
  } else if (String(topic) == "relay/device4") {
    if (messageTemp == "on") {
      Serial.println("Device 4 on");
      Device4State = "on";
      digitalWrite(Device4, LOW);
      if (saveState) EEPROM.write(3, 1);
    } else if (messageTemp == "off") {
      Serial.println("Device 4 off");
      Device4State = "off";
      digitalWrite(Device4, HIGH);
      if (saveState) EEPROM.write(3, 0);
    }
  } else if (String(topic) == "relay/device5") {
    if (messageTemp == "on") {
      Serial.println("Device 5 on");
      Device5State = "on";
      digitalWrite(Device5, LOW);
      if (saveState) EEPROM.write(4, 1);
    } else if (messageTemp == "off") {
      Serial.println("Device 5 off");
      Device5State = "off";
      digitalWrite(Device5, HIGH);
      if (saveState) EEPROM.write(4, 0);
    }
  } else if (String(topic) == "relay/device6") {
    if (messageTemp == "on") {
      Serial.println("Device 6 on");
      Device6State = "on";
      digitalWrite(Device6, LOW);
      if (saveState) EEPROM.write(5, 1);
    } else if (messageTemp == "off") {
      Serial.println("Device 6 off");
      Device6State = "off";
      digitalWrite(Device6, HIGH);
      if (saveState) EEPROM.write(5, 0);
    }
  }

  if (saveState) {
    EEPROM.commit();
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM
  if (saveState) {
    EEPROM.begin(eepromSize);
  }

  // Initialize the GPIO pins for the devices as outputs and set them to HIGH (NC state)
  pinMode(Device1, OUTPUT);
  pinMode(Device2, OUTPUT);
  pinMode(Device3, OUTPUT);
  pinMode(Device4, OUTPUT);
  pinMode(Device5, OUTPUT);
  pinMode(Device6, OUTPUT);
  
  // Load saved states from EEPROM
  if (saveState) {
    Device1State = EEPROM.read(0) == 1 ? "on" : "off";
    Device2State = EEPROM.read(1) == 1 ? "on" : "off";
    Device3State = EEPROM.read(2) == 1 ? "on" : "off";
    Device4State = EEPROM.read(3) == 1 ? "on" : "off";
    Device5State = EEPROM.read(4) == 1 ? "on" : "off";
    Device6State = EEPROM.read(5) == 1 ? "on" : "off";
  }

  // Set initial relay states
  digitalWrite(Device1, Device1State == "on" ? LOW : HIGH);
  digitalWrite(Device2, Device2State == "on" ? LOW : HIGH);
  digitalWrite(Device3, Device3State == "on" ? LOW : HIGH);
  digitalWrite(Device4, Device4State == "on" ? LOW : HIGH);
  digitalWrite(Device5, Device5State == "on" ? LOW : HIGH);
  digitalWrite(Device6, Device6State == "on" ? LOW : HIGH);

  // Connect to Wi-Fi
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // Connect to MQTT Broker
  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);

  while (!client.connected()) {
    Serial.println("Connecting to MQTT...");
    if (client.connect("ESP32Client", mqttUser, mqttPassword)) {
      Serial.println("connected");
    } else {
      Serial.print("failed with state ");
      Serial.print(client.state());
      delay(2000);
    }
  }

  // Subscribe to topics
  client.subscribe("relay/device1");
  client.subscribe("relay/device2");
  client.subscribe("relay/device3");
  client.subscribe("relay/device4");
  client.subscribe("relay/device5");
  client.subscribe("relay/device6");
}

void loop() {
  client.loop();
}
