#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

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
  const char* nvsKey;    // Preferences key holding the persisted state
  bool on;               // current state
};

// REWIRE REQUIRED: Device2 moved from GPIO5 to GPIO23. GPIO5 is an ESP32
// strapping pin -- it is sampled at reset and emits a PWM burst at boot, so a
// relay module loading or pulling it down can stop the board booting. GPIO23
// has no strapping or boot-glitch behaviour.
Relay relays[] = {
  { 4,  "relay/device1", "d1", false },
  { 23, "relay/device2", "d2", false },
  { 18, "relay/device3", "d3", false },
  { 19, "relay/device4", "d4", false },
  { 21, "relay/device5", "d5", false },
  { 22, "relay/device6", "d6", false },
};
const size_t RELAY_COUNT = sizeof(relays) / sizeof(relays[0]);

// Variable to enable or disable state saving
const bool saveState = true;

// Persisted relay state. NVS wear-levels internally and skips the write when
// the stored value already matches, unlike the raw EEPROM emulation this
// replaces (whose commit() erased a full 4 KB sector unconditionally).
Preferences prefs;

// Longest command payload we accept ("off" + NUL, rounded up). Payloads are
// copied into a fixed stack buffer so no heap allocation happens per message.
const size_t MAX_PAYLOAD = 8;

// Drive the relay hardware to match r.on. Relay modules are active LOW.
void applyRelay(const Relay& r) {
  digitalWrite(r.pin, r.on ? LOW : HIGH);
}

// Apply a new state to a relay, persisting it only when it actually changed.
// The previous code wrote and committed on every inbound message -- one full
// flash sector erase each -- so a chatty publisher (or a hostile one) could
// exhaust the flash endurance in a few hours.
void setRelay(Relay& r, bool on) {
  if (r.on == on) {
    return;
  }
  r.on = on;
  applyRelay(r);
  Serial.printf("%s -> %s\n", r.cmdTopic, on ? "on" : "off");
  if (saveState) {
    prefs.putBool(r.nvsKey, on);
  }
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
      setRelay(r, true);
    } else if (strcmp(payload, "off") == 0) {
      setRelay(r, false);
    }
    break;
  }
}

void setup() {
  Serial.begin(115200);

  // Open the NVS namespace holding the persisted relay states
  if (saveState) {
    prefs.begin("relay", false);
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

  // Load saved states from NVS (defaults to off on a fresh device)
  if (saveState) {
    for (size_t i = 0; i < RELAY_COUNT; i++) {
      relays[i].on = prefs.getBool(relays[i].nvsKey, false);
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
