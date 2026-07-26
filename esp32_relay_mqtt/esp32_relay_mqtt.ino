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

// MQTT client ID. A broker permits one connection per client ID and evicts the
// existing session when a second client presents the same one, so a shared
// literal like "ESP32Client" lets a second board -- or anyone who can reach the
// broker -- kick this device off at will. Derived from the factory MAC, which
// is unique per device and stable across reboots and reflashes.
char clientId[24];

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

// Longest command payload we accept. Payloads are copied into a fixed stack
// buffer so no heap allocation happens per message; anything longer than this
// is rejected outright rather than truncated (truncating "onwards" to "on"
// would actuate a relay the sender never asked for).
const size_t MAX_PAYLOAD = 16;

// Reconnect backoff: start at 1s and cap at 20s so a reconnect never feels
// stuck. An auth rejection backs off much further -- retrying bad credentials
// every second is pointless and keeps fail2ban-style broker lockouts alive.
const unsigned long RECONNECT_MIN_MS = 1000;
const unsigned long RECONNECT_MAX_MS = 20000;
const unsigned long RECONNECT_AUTH_MS = 60000;

// Detect a dead link reasonably fast and keep NAT mappings alive.
const uint16_t MQTT_KEEPALIVE_S = 45;

unsigned long lastConnectAttempt = 0;
unsigned long reconnectDelayMs = RECONNECT_MIN_MS;

// NOTE: every function definition must stay below the type definitions above.
// The Arduino preprocessor auto-generates prototypes and injects them ahead of
// the first function in the file, so a function defined before `struct Relay`
// makes the generated `applyRelay(const Relay&)` prototype fail to compile.
void buildClientId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(clientId, sizeof(clientId), "esp32-relay-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

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

// Strip leading and trailing whitespace in place.
char* trimWhitespace(char* s) {
  while (*s != '\0' && isspace((unsigned char)*s)) {
    s++;
  }
  char* end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  return s;
}

// Parse a command payload into a boolean. Accepts on/off, 1/0 and true/false
// in any case. Returns false when the payload is not a recognised command so
// the caller can reject it -- an unparsed payload must never fall through to a
// default that actuates a relay.
bool parseOnOff(const char* s, bool& out) {
  if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0) {
    out = true;
    return true;
  }
  if (strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

// Callback function for MQTT subscription
void callback(char* topic, byte* message, unsigned int length) {
  // An empty payload is how brokers and tools clear a retained topic (MQTT
  // Explorer's "delete topic", history clearing). It is never a command.
  if (length == 0) {
    Serial.printf("Ignoring empty payload on %s\n", topic);
    return;
  }

  if (length >= MAX_PAYLOAD) {
    Serial.printf("Ignoring oversized payload (%u bytes) on %s\n", length, topic);
    return;
  }

  char raw[MAX_PAYLOAD];
  memcpy(raw, message, length);
  raw[length] = '\0';
  char* payload = trimWhitespace(raw);

  bool desired;
  if (!parseOnOff(payload, desired)) {
    Serial.printf("Ignoring unrecognised command '%s' on %s\n", payload, topic);
    return;
  }

  for (size_t i = 0; i < RELAY_COUNT; i++) {
    if (strcmp(topic, relays[i].cmdTopic) == 0) {
      setRelay(relays[i], desired);
      return;
    }
  }

  Serial.printf("No relay bound to topic %s\n", topic);
}

// Turn PubSubClient's state() code into something greppable in a serial log.
// "it randomly disconnects" is a 30-second diagnosis with this line present.
const char* mqttStateName(int state) {
  switch (state) {
    case MQTT_CONNECTION_TIMEOUT:      return "connection timeout";
    case MQTT_CONNECTION_LOST:         return "connection lost";
    case MQTT_CONNECT_FAILED:          return "TCP connect failed";
    case MQTT_DISCONNECTED:            return "disconnected";
    case MQTT_CONNECTED:               return "connected";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "broker rejected protocol version";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "broker rejected client ID";
    case MQTT_CONNECT_UNAVAILABLE:     return "broker unavailable";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "bad username or password";
    case MQTT_CONNECT_UNAUTHORIZED:    return "not authorized";
    default:                           return "unknown";
  }
}

// Subscriptions do not survive a reconnect, so this runs on every connect.
void subscribeAll() {
  for (size_t i = 0; i < RELAY_COUNT; i++) {
    client.subscribe(relays[i].cmdTopic);
  }
}

// One connection attempt. Returns true on success. Never blocks longer than
// the underlying socket timeout, so it is safe to call from loop().
bool mqttConnect() {
  Serial.printf("Connecting to MQTT as %s ...\n", clientId);

  if (!client.connect(clientId, mqttUser, mqttPassword)) {
    int st = client.state();
    Serial.printf("MQTT connect failed: state %d (%s)\n", st, mqttStateName(st));

    if (st == MQTT_CONNECT_BAD_CREDENTIALS || st == MQTT_CONNECT_UNAUTHORIZED) {
      Serial.println("Not authorized -- check credentials. Retrying slowly.");
      reconnectDelayMs = RECONNECT_AUTH_MS;
    } else {
      unsigned long next = reconnectDelayMs * 2;
      reconnectDelayMs = next > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : next;
    }
    return false;
  }

  Serial.println("MQTT connected");
  reconnectDelayMs = RECONNECT_MIN_MS;
  subscribeAll();
  return true;
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
  WiFi.mode(WIFI_STA);
  buildClientId();
  Serial.printf("MQTT client ID: %s\n", clientId);

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
  client.setKeepAlive(MQTT_KEEPALIVE_S);

  while (!client.connected()) {
    Serial.printf("Connecting to MQTT as %s ...\n", clientId);
    if (client.connect(clientId, mqttUser, mqttPassword)) {
      Serial.println("connected");
    } else {
      Serial.print("failed with state ");
      Serial.print(client.state());
      delay(2000);
    }
  }

  // Subscribe to topics
  subscribeAll();
}

void loop() {
  // Re-establish the broker session after a drop. Without this the board went
  // deaf permanently on the first blip -- relays frozen in their last state
  // until someone power-cycled it.
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastConnectAttempt >= reconnectDelayMs) {
      lastConnectAttempt = now;
      mqttConnect();
    }
    return;
  }

  client.loop();
}
