// Transport security. Set to 1 once your broker has TLS configured. The port
// follows this toggle automatically (1883 <-> 8883), which avoids the classic
// "TLS enabled but still pointed at the cleartext port" failure. Plain MQTT
// sends the broker username and password, and every relay command, in the
// clear -- readable and injectable by anyone on the path.
#define MQTT_USE_TLS 0

#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#if MQTT_USE_TLS
#include <WiFiClientSecure.h>
#endif

// Replace with your network credentials
const char* ssid = "";  // Network SSID (name)
const char* password = "";  // Network password

// MQTT Broker details
const char* mqttServer = "";
const char* mqttUser = "";
const char* mqttPassword = "";

#if MQTT_USE_TLS
const int mqttPort = 8883;

// The broker is verified against this certificate -- there is no unverified
// fallback, so a MITM cannot impersonate your broker. Paste the CA that signed
// the broker's certificate (for a self-signed broker, its own certificate).
static const char MQTT_CA_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE YOUR BROKER CA CERTIFICATE HERE
-----END CERTIFICATE-----
)EOF";

// Delete this line once a real certificate is pasted above.
#define MQTT_CA_CERT_IS_PLACEHOLDER 1

#ifdef MQTT_CA_CERT_IS_PLACEHOLDER
#error "MQTT_USE_TLS is 1: paste your broker's CA certificate into MQTT_CA_CERT, then delete the MQTT_CA_CERT_IS_PLACEHOLDER line."
#endif
#else
const int mqttPort = 1883;
#endif

// Initialize the WiFi and MQTT client objects
#if MQTT_USE_TLS
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif
PubSubClient client(espClient);

// MQTT client ID. A broker permits one connection per client ID and evicts the
// existing session when a second client presents the same one, so a shared
// literal like "ESP32Client" lets a second board -- or anyone who can reach the
// broker -- kick this device off at will. Derived from the factory MAC, which
// is unique per device and stable across reboots and reflashes.
char clientId[24];

#define FW_VERSION "2026.7.0"

// Liveness topics. status is the Last Will payload as well: the broker
// publishes "0" retained if this board drops off without a clean disconnect,
// so subscribers can tell "off" from "not there".
const char* STATUS_TOPIC = "relay/status";
const char* VERSION_TOPIC = "relay/version";

// One entry per relay channel. Every topic string lives in this table and
// nowhere else, so the MQTT contract has a single source of truth.
struct Relay {
  uint8_t pin;             // GPIO driving the relay (active LOW)
  const char* cmdTopic;    // topic this channel listens on
  const char* stateTopic;  // topic this channel reports its state on
  const char* nvsKey;      // Preferences key holding the persisted state
  bool on;                 // current state
};

// REWIRE REQUIRED: Device2 moved from GPIO5 to GPIO23. GPIO5 is an ESP32
// strapping pin -- it is sampled at reset and emits a PWM burst at boot, so a
// relay module loading or pulling it down can stop the board booting. GPIO23
// has no strapping or boot-glitch behaviour.
Relay relays[] = {
  { 4,  "relay/device1", "relay/device1/state", "d1", false },
  { 23, "relay/device2", "relay/device2/state", "d2", false },
  { 18, "relay/device3", "relay/device3/state", "d3", false },
  { 19, "relay/device4", "relay/device4/state", "d4", false },
  { 21, "relay/device5", "relay/device5/state", "d5", false },
  { 22, "relay/device6", "relay/device6/state", "d6", false },
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

// How often to re-issue a Wi-Fi association attempt while disconnected.
const unsigned long WIFI_RETRY_MS = 10000;

// Commands are events, not state: a retained command republished by the broker
// on every (re)subscribe would re-actuate relays on each reconnect. PubSubClient
// does not expose the retain flag to the callback, but retained messages are
// delivered immediately after SUBSCRIBE, so inbound commands are ignored for a
// short settling window. The authoritative boot state comes from NVS instead.
// Trade-off: a command published within this window of a reconnect is dropped.
const unsigned long RETAINED_SETTLE_MS = 1500;

unsigned long lastConnectAttempt = 0;
unsigned long reconnectDelayMs = RECONNECT_MIN_MS;
unsigned long lastWifiAttempt = 0;
bool wifiWasConnected = false;
unsigned long subscribedAt = 0;
bool settling = false;

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

// Report a relay's state, retained so a subscriber learns it on connect
// instead of waiting for the next change. PubSubClient publishes at QoS 0
// only; the LWT below is the one publish that carries QoS 1.
void publishState(const Relay& r) {
  if (client.connected()) {
    client.publish(r.stateTopic, r.on ? "on" : "off", true);
  }
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
  publishState(r);
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
  // Drop the retained backlog the broker replays right after SUBSCRIBE.
  if (settling) {
    if (millis() - subscribedAt < RETAINED_SETTLE_MS) {
      Serial.printf("Ignoring replayed retained message on %s\n", topic);
      return;
    }
    settling = false;
  }

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

// Non-blocking Wi-Fi maintenance. setup() used to spin in a while loop with no
// timeout, so a board powered on while the AP was down never reached loop() at
// all and stayed inert until the network happened to come back.
bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print("WiFi connected. IP address: ");
      Serial.println(WiFi.localIP());
    }
    return true;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi connection lost.");
  }

  unsigned long now = millis();
  if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = now;
    Serial.println("WiFi not connected; retrying association...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
  return false;
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
bool subscribeAll() {
  bool ok = true;
  for (size_t i = 0; i < RELAY_COUNT; i++) {
    // QoS 1 for commands. A failed SUBSCRIBE used to pass silently, leaving
    // the board connected and apparently healthy while ignoring every command.
    if (!client.subscribe(relays[i].cmdTopic, 1)) {
      Serial.printf("SUBSCRIBE FAILED for %s\n", relays[i].cmdTopic);
      ok = false;
    }
  }
  // Open the window during which replayed retained commands are ignored.
  subscribedAt = millis();
  settling = true;
  return ok;
}

// One connection attempt. Returns true on success. Never blocks longer than
// the underlying socket timeout, so it is safe to call from loop().
bool mqttConnect() {
  Serial.printf("Connecting to MQTT as %s ...\n", clientId);

  // Register the Last Will so an ungraceful drop is visible to subscribers.
  if (!client.connect(clientId, mqttUser, mqttPassword,
                      STATUS_TOPIC, 1, true, "0")) {
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

  // Subscribe before announcing liveness: a session that cannot receive
  // commands must not advertise itself as up. A clean disconnect does not
  // fire the Last Will, so status would otherwise be stuck at "1".
  if (!subscribeAll()) {
    Serial.println("Dropping session because a subscription failed.");
    client.disconnect();
    unsigned long next = reconnectDelayMs * 2;
    reconnectDelayMs = next > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : next;
    return false;
  }

  reconnectDelayMs = RECONNECT_MIN_MS;

  // Re-announce liveness and the full relay snapshot on every connect, not
  // just the first, so subscribers resync after any drop.
  client.publish(STATUS_TOPIC, "1", true);
  client.publish(VERSION_TOPIC, FW_VERSION, true);

  for (size_t i = 0; i < RELAY_COUNT; i++) {
    publishState(relays[i]);
  }
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

  // The SSID is deliberately not logged -- anyone with a serial console (or a
  // USB cable and thirty seconds) should not get a free read of the network
  // this board is joined to.
  Serial.println("Starting Wi-Fi association...");
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  lastWifiAttempt = millis();

  // Configure the MQTT client. Connecting is left to loop() so that an
  // unreachable AP or broker at power-on can never stall startup.
#if MQTT_USE_TLS
  espClient.setCACert(MQTT_CA_CERT);
  Serial.println("MQTT transport: TLS, broker certificate verified");
#else
  Serial.println("MQTT transport: PLAINTEXT -- credentials and commands are");
  Serial.println("readable on the network. Set MQTT_USE_TLS to 1 when able.");
#endif
  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  client.setKeepAlive(MQTT_KEEPALIVE_S);
}

void loop() {
  // Relays hold their restored state while offline; nothing here actuates.
  if (!ensureWifi()) {
    delay(10);  // yield to the scheduler instead of spinning on the retry timer
    return;
  }

  // Re-establish the broker session after a drop. Without this the board went
  // deaf permanently on the first blip -- relays frozen in their last state
  // until someone power-cycled it.
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastConnectAttempt >= reconnectDelayMs) {
      lastConnectAttempt = now;
      mqttConnect();
    }
    delay(10);  // yield to the scheduler while waiting out the backoff
    return;
  }

  client.loop();
}
