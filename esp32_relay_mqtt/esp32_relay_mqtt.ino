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

// One entry per relay channel. Every topic string lives in this table and
// nowhere else, so the MQTT contract has a single source of truth.
struct Relay {
  uint8_t pin;           // GPIO driving the relay (active LOW)
  const char* cmdTopic;  // topic this channel listens on
  int eepromAddr;        // byte offset of the persisted state
  bool on;               // current state
};

Relay relays[] = {
  { 4,  "relay/device1", 0, false },
  { 5,  "relay/device2", 1, false },
  { 18, "relay/device3", 2, false },
  { 19, "relay/device4", 3, false },
  { 21, "relay/device5", 4, false },
  { 22, "relay/device6", 5, false },
};
const size_t RELAY_COUNT = sizeof(relays) / sizeof(relays[0]);

// Variable to enable or disable state saving
const bool saveState = true;

// EEPROM address to store the state
const int eepromSize = 6;

// Longest command payload we accept ("off" + NUL, rounded up). Payloads are
// copied into a fixed stack buffer so no heap allocation happens per message.
const size_t MAX_PAYLOAD = 8;

// Drive the relay hardware to match r.on. Relay modules are active LOW.
void applyRelay(const Relay& r) {
  digitalWrite(r.pin, r.on ? LOW : HIGH);
}

// Callback function for MQTT subscription
void callback(char* topic, byte* message, unsigned int length) {
  char payload[MAX_PAYLOAD];
  size_t n = length < (MAX_PAYLOAD - 1) ? length : (MAX_PAYLOAD - 1);
  memcpy(payload, message, n);
  payload[n] = '\0';

  // Print the message for debugging
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  Serial.println(payload);

  for (size_t i = 0; i < RELAY_COUNT; i++) {
    Relay& r = relays[i];
    if (strcmp(topic, r.cmdTopic) != 0) {
      continue;
    }

    if (strcmp(payload, "on") == 0) {
      r.on = true;
    } else if (strcmp(payload, "off") == 0) {
      r.on = false;
    } else {
      break;
    }

    Serial.printf("%s -> %s\n", r.cmdTopic, r.on ? "on" : "off");
    applyRelay(r);
    if (saveState) {
      EEPROM.write(r.eepromAddr, r.on ? 1 : 0);
    }
    break;
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

  // Park every relay OFF *before* the pad becomes an output. pinMode(OUTPUT)
  // drives the reset-default LOW, and these modules are active LOW, so doing
  // this in the other order pulses all six loads on at every boot, brownout
  // and watchdog reset. Writing the output register first means the pin drives
  // HIGH the instant it is enabled.
  for (size_t i = 0; i < RELAY_COUNT; i++) {
    digitalWrite(relays[i].pin, HIGH);
    pinMode(relays[i].pin, OUTPUT);
    digitalWrite(relays[i].pin, HIGH);
  }

  // Load saved states from EEPROM
  if (saveState) {
    for (size_t i = 0; i < RELAY_COUNT; i++) {
      relays[i].on = (EEPROM.read(relays[i].eepromAddr) == 1);
    }
  }

  // Set initial relay states
  for (size_t i = 0; i < RELAY_COUNT; i++) {
    applyRelay(relays[i]);
  }

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
  for (size_t i = 0; i < RELAY_COUNT; i++) {
    client.subscribe(relays[i].cmdTopic);
  }
}

void loop() {
  client.loop();
}
