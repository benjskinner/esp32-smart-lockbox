/*
 * Package Lockbox Firmware for ESP32
 *
 * First-boot behaviour:
 *   1. Generates a unique device ID and secret, saves to flash
 *   2. Starts BLE advertising so the mobile app can pair
 *   3. Mobile app writes WiFi credentials + server URL via BLE
 *   4. Device reboots into normal operation
 *
 * Normal operation:
 *   - Connects to saved WiFi + WebSocket server
 *   - Commands from server: "lock", "unlock" (admin, PIR grace),
 *                           "unlock_guest" (guest web page, no PIR grace)
 *   - Motion sensor: auto-locks after LOCK_DELAY_TIME ms of inactivity
 *   - PIR is ignored for PIR_IGNORE_TIME ms after an admin app unlock
 *   - If connection is lost for DISCONNECT_GRACE_MS, unlocks and waits;
 *     restores previous lock state on reconnect
 *   - Sends "box_locked" event to server whenever motion triggers a lock
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_random.h>

const int PIR_PIN = 27;
const int RELAY_PIN = 26;
const int LED_PIN = 2;

const unsigned long LOCK_DELAY_TIME = 15000;     // ms after motion before auto-lock
const unsigned long PIR_IGNORE_TIME = 45000;     // ms to ignore PIR after app unlock
const unsigned long DISCONNECT_GRACE_MS = 8000;  // ms offline before safe mode
const unsigned long HEARTBEAT_INTERVAL = 30000;  // ms between WebSocket heartbeats

// BLE service + characteristic UUIDs — must match the mobile app exactly
#define LOCKBOX_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DEVICE_ID_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DEVICE_SECRET_UUID "beb5483f-36e1-4688-b7f5-ea07361b26a8"
#define WIFI_SSID_UUID "beb54840-36e1-4688-b7f5-ea07361b26a8"
#define WIFI_PASS_UUID "beb54841-36e1-4688-b7f5-ea07361b26a8"
#define SERVER_URL_UUID "beb54842-36e1-4688-b7f5-ea07361b26a8"
#define STATUS_UUID "beb54843-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_UUID "beb54844-36e1-4688-b7f5-ea07361b26a8"

// ─── State ────────────────────────────────────────────────────────────────────
Preferences prefs;
WebSocketsClient wsClient;

String deviceId;
String deviceSecret;
String wifiSsid;
String wifiPassword;
String serverUrl;  // Configured during device provisioning.

// Connection
bool wsAuthenticated = false;
unsigned long lastHeartbeat = 0;

// Lock
bool locked = false;  // Device starts unlocked.

// Motion / PIR
bool motionDetected = false;
unsigned long motionTime = 0;
bool lockNotificationSent = false;
bool appUnlockGraceActive = false;
unsigned long lastAppUnlockTime = 0;

// Offline safe mode
bool disconnectTimerRunning = false;
unsigned long disconnectStart = 0;
bool inOfflineSafeMode = false;
bool preDisconnectLocked = false;
bool savedPreDisconnect = false;

// BLE
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* statusChar = nullptr;
bool bleProvisioned = false;

// ─── Forward declarations ─────────────────────────────────────────────────────
void setLockedState(bool lockIt);
void sendAck(const char* command);
void sendEvent(const char* event, const char* message);
void resetMotionCycle();
bool pirTemporarilyDisabled();

// ─── Helpers ──────────────────────────────────────────────────────────────────
String generateHex(int bytes) {
  String result = "";
  for (int i = 0; i < bytes; i++) {
    uint8_t r = (uint8_t)(esp_random() & 0xFF);
    if (r < 16) result += "0";
    result += String(r, HEX);
  }
  return result;
}

void blinkLed(int times, int msOn = 100, int msOff = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(msOn);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(msOff);
  }
}

void setBleStatus(const String& status) {
  if (statusChar) {
    statusChar->setValue(status.c_str());
    statusChar->notify();
  }
}

// ─── Lock control ─────────────────────────────────────────────────────────────
void setLockedState(bool lockIt) {
  locked = lockIt;
  digitalWrite(RELAY_PIN, lockIt ? HIGH : LOW);  // HIGH = locked, LOW = unlocked
  Serial.printf("[LOCK] %s\n", lockIt ? "LOCKED" : "UNLOCKED");
}

void resetMotionCycle() {
  motionDetected = false;
  motionTime = 0;
  lockNotificationSent = false;
}

bool pirTemporarilyDisabled() {
  if (!appUnlockGraceActive || locked) return false;
  if (millis() - lastAppUnlockTime < PIR_IGNORE_TIME) return true;
  appUnlockGraceActive = false;
  return false;
}

// ─── Command handlers ─────────────────────────────────────────────────────────
void doAdminUnlock() {
  // App/admin unlock — activates PIR grace period
  setLockedState(false);
  lastAppUnlockTime = millis();
  appUnlockGraceActive = true;
  resetMotionCycle();
  sendAck("unlock");
}

void doGuestUnlock() {
  // Guest web-page unlock — code already validated by server.
  // Guest unlocks do not receive the admin PIR grace period.
  setLockedState(false);
  appUnlockGraceActive = false;
  lastAppUnlockTime = 0;
  resetMotionCycle();
  sendAck("unlock_guest");
}

void doLock() {
  setLockedState(true);
  appUnlockGraceActive = false;
  resetMotionCycle();
  sendAck("lock");
}

// ─── WebSocket messaging ──────────────────────────────────────────────────────
void sendAck(const char* command) {
  if (!wsAuthenticated) return;
  StaticJsonDocument<128> doc;
  doc["type"] = "ack";
  doc["command"] = command;
  doc["locked"] = locked;
  String msg;
  serializeJson(doc, msg);
  wsClient.sendTXT(msg);
}

void sendHeartbeat() {
  if (!wsAuthenticated) return;
  StaticJsonDocument<64> doc;
  doc["type"] = "heartbeat";
  doc["locked"] = locked;
  String msg;
  serializeJson(doc, msg);
  wsClient.sendTXT(msg);
}

void sendEvent(const char* event, const char* message) {
  if (!wsAuthenticated) return;
  StaticJsonDocument<256> doc;
  doc["type"] = "event";
  doc["event"] = event;
  doc["message"] = message;
  doc["locked"] = locked;
  String msg;
  serializeJson(doc, msg);
  wsClient.sendTXT(msg);
}

// ─── BLE callbacks ────────────────────────────────────────────────────────────
class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    String value = String(pChar->getValue().c_str());
    if (value != "connect") return;

    setBleStatus("connecting");

    NimBLEService* svc = bleServer->getServiceByUUID(LOCKBOX_SERVICE_UUID);
    wifiSsid = String(svc->getCharacteristic(WIFI_SSID_UUID)->getValue().c_str());
    wifiPassword = String(svc->getCharacteristic(WIFI_PASS_UUID)->getValue().c_str());
    serverUrl = String(svc->getCharacteristic(SERVER_URL_UUID)->getValue().c_str());

    prefs.begin("lockbox", false);
    prefs.putString("wifiSsid", wifiSsid);
    prefs.putString("wifiPass", wifiPassword);
    prefs.putString("serverUrl", serverUrl);
    prefs.end();

    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      setBleStatus("connected");
      bleProvisioned = true;
      blinkLed(3, 200, 200);
      delay(1000);
      ESP.restart();
    } else {
      setBleStatus("wifi_failed");
    }
  }
};

// ─── BLE setup mode ───────────────────────────────────────────────────────────
void startBleSetupMode() {
  Serial.println("[BLE] Starting setup mode...");
  blinkLed(2, 500, 200);

  NimBLEDevice::init("Lockbox-" + deviceId.substring(0, 4));
  bleServer = NimBLEDevice::createServer();

  NimBLEService* service = bleServer->createService(LOCKBOX_SERVICE_UUID);

  NimBLECharacteristic* idChar = service->createCharacteristic(DEVICE_ID_UUID, NIMBLE_PROPERTY::READ);
  idChar->setValue(deviceId.c_str());

  NimBLECharacteristic* secretChar = service->createCharacteristic(DEVICE_SECRET_UUID, NIMBLE_PROPERTY::READ);
  secretChar->setValue(deviceSecret.c_str());

  service->createCharacteristic(WIFI_SSID_UUID, NIMBLE_PROPERTY::WRITE);
  service->createCharacteristic(WIFI_PASS_UUID, NIMBLE_PROPERTY::WRITE);
  service->createCharacteristic(SERVER_URL_UUID, NIMBLE_PROPERTY::WRITE);

  statusChar = service->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusChar->setValue("ready");

  NimBLECharacteristic* cmdChar = service->createCharacteristic(COMMAND_UUID, NIMBLE_PROPERTY::WRITE);
  cmdChar->setCallbacks(new CommandCallbacks());

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(LOCKBOX_SERVICE_UUID);
  advertising->start();

  Serial.println("[BLE] Advertising as: Lockbox-" + deviceId.substring(0, 4));
  setBleStatus("ready");
}

// ─── WebSocket events ─────────────────────────────────────────────────────────
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      {
        Serial.println("[WS] Connected — authenticating...");
        wsAuthenticated = false;
        StaticJsonDocument<256> doc;
        doc["type"] = "auth";
        doc["deviceId"] = deviceId;
        doc["secret"] = deviceSecret;
        String msg;
        serializeJson(doc, msg);
        wsClient.sendTXT(msg);
        break;
      }

    case WStype_TEXT:
      {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, payload, length)) break;

        const char* msgType = doc["type"];
        if (!msgType) break;

        if (strcmp(msgType, "auth_ok") == 0) {
          wsAuthenticated = true;
          Serial.println("[WS] Authenticated");
          blinkLed(2, 100, 100);

          // Exit offline safe mode and restore pre-disconnect state
          if (inOfflineSafeMode) {
            Serial.printf("[WS] Reconnected — restoring state: %s\n",
                          preDisconnectLocked ? "LOCKED" : "UNLOCKED");
            setLockedState(preDisconnectLocked);
            sendAck(preDisconnectLocked ? "lock" : "unlock");
            inOfflineSafeMode = false;
            savedPreDisconnect = false;
            disconnectTimerRunning = false;
          }

          // Execute any pending command saved while device was offline
          const char* cmd = doc["pendingCommand"];
          if (cmd && strcmp(cmd, "unlock") == 0) doAdminUnlock();
          else if (cmd && strcmp(cmd, "unlock_guest") == 0) doGuestUnlock();
          else if (cmd && strcmp(cmd, "lock") == 0) doLock();

        } else if (strcmp(msgType, "auth_error") == 0) {
          Serial.println("[WS] Auth failed — check credentials");
          wsClient.disconnect();

        } else if (strcmp(msgType, "command") == 0) {
          const char* cmd = doc["command"];
          if (cmd && strcmp(cmd, "unlock") == 0) doAdminUnlock();
          else if (cmd && strcmp(cmd, "unlock_guest") == 0) doGuestUnlock();
          else if (cmd && strcmp(cmd, "lock") == 0) doLock();

        } else if (strcmp(msgType, "heartbeat_ack") == 0) {
          // Server may include a pending command in the heartbeat ack
          const char* cmd = doc["command"];
          if (cmd && strcmp(cmd, "unlock") == 0) doAdminUnlock();
          else if (cmd && strcmp(cmd, "unlock_guest") == 0) doGuestUnlock();
          else if (cmd && strcmp(cmd, "lock") == 0) doLock();

        } else if (strcmp(msgType, "code_update") == 0) {
          Serial.println("[WS] Access code updated by server");
        }
        break;
      }

    case WStype_DISCONNECTED:
      if (wsAuthenticated) {
        // Save state at moment of disconnect for restore on reconnect
        if (!savedPreDisconnect) {
          preDisconnectLocked = locked;
          savedPreDisconnect = true;
          Serial.printf("[WS] Disconnected — saved state: %s\n",
                        preDisconnectLocked ? "LOCKED" : "UNLOCKED");
        }
        disconnectTimerRunning = true;
        disconnectStart = millis();
      }
      wsAuthenticated = false;
      Serial.println("[WS] Disconnected — reconnecting automatically");
      break;

    default:
      break;
  }
}

// ─── WebSocket connection ──────────────────────────────────────────────────────
void connectWebSocket() {
  String url = serverUrl;
  bool useSsl = url.startsWith("wss://");
  url.replace("wss://", "");
  url.replace("ws://", "");

  int slashIdx = url.indexOf('/');
  String host = (slashIdx > 0) ? url.substring(0, slashIdx) : url;
  String path = (slashIdx > 0) ? url.substring(slashIdx) : "/ws";
  int port = useSsl ? 443 : 80;

  Serial.printf("[WS] Connecting to %s:%d%s\n", host.c_str(), port, path.c_str());

  if (useSsl) wsClient.beginSSL(host.c_str(), port, path.c_str());
  else wsClient.begin(host.c_str(), port, path.c_str());

  wsClient.onEvent(onWsEvent);
  wsClient.setReconnectInterval(5000);
  wsClient.enableHeartbeat(15000, 3000, 2);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Package Lockbox starting...");

  pinMode(PIR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // Initial state: UNLOCKED
  setLockedState(false);
  digitalWrite(LED_PIN, LOW);

  // Load persisted credentials + WiFi settings
  prefs.begin("lockbox", true);
  deviceId = prefs.getString("deviceId", "");
  deviceSecret = prefs.getString("deviceSecret", "");
  wifiSsid = prefs.getString("wifiSsid", "");
  wifiPassword = prefs.getString("wifiPass", "");
  serverUrl = prefs.getString("serverUrl", "");
  prefs.end();

  // First boot — generate unique device identity and authentication secret.
  if (deviceId.isEmpty()) {
    deviceId = generateHex(8);       // 16-char hex
    deviceSecret = generateHex(16);  // 32-char hex

    prefs.begin("lockbox", false);
    prefs.putString("deviceId", deviceId);
    prefs.putString("deviceSecret", deviceSecret);
    prefs.end();

    Serial.println("[BOOT] First boot — device ID: " + deviceId);
  } else {
    Serial.println("[BOOT] Device ID: " + deviceId);
  }

  // No WiFi saved — enter BLE setup mode
  if (wifiSsid.isEmpty()) {
    Serial.println("[BOOT] No WiFi — entering BLE setup mode");
    startBleSetupMode();
    return;
  }

  // Normal boot — connect WiFi
  Serial.println("[WIFI] Connecting to: " + wifiSsid);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    blinkLed(1, 50, 0);
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Failed — entering BLE setup mode");
    startBleSetupMode();
    return;
  }

  Serial.println("[WIFI] Connected: " + WiFi.localIP().toString());
  blinkLed(3, 100, 100);
  connectWebSocket();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  // BLE setup mode — just spin
  if (bleServer && !bleProvisioned) {
    delay(10);
    return;
  }

  wsClient.loop();

  // ── Disconnect grace period / offline safe mode ──────────────────────────
  if (disconnectTimerRunning && !wsAuthenticated && !inOfflineSafeMode) {
    if (millis() - disconnectStart >= DISCONNECT_GRACE_MS) {
      Serial.println("[WS] Offline grace expired — entering safe mode (unlocking)");
      inOfflineSafeMode = true;
      disconnectTimerRunning = false;
      if (locked) setLockedState(false);
      resetMotionCycle();
    }
  }

  // ── Heartbeat ────────────────────────────────────────────────────────────
  if (wsAuthenticated && millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

  // ── PIR motion → auto-lock ────────────────────────────────────────────────
  if (!locked && !motionDetected && !pirTemporarilyDisabled()) {
    if (digitalRead(PIR_PIN) == HIGH) {
      motionDetected = true;
      motionTime = millis();
      Serial.println("[PIR] Motion detected — lock timer started");
    }
  }

  if (motionDetected && !locked) {
    if (pirTemporarilyDisabled()) {
      Serial.println("[PIR] Cancelled — recently unlocked from app");
      resetMotionCycle();
      return;
    }

    if (millis() - motionTime >= LOCK_DELAY_TIME) {
      setLockedState(true);
      sendAck("lock");

      if (!lockNotificationSent) {
        sendEvent("box_locked", "Your lockbox locked after motion was detected.");
        lockNotificationSent = true;
        Serial.println("[PIR] Auto-lock complete, notification sent");
      }

      resetMotionCycle();
    }
  }
}
