/* ================================================================
   GLOBAL CONFIG - EDIT ONLY THIS SECTION
   ================================================================ */
#define BLE_SERVICE_UUID     "0000aaaa-0000-1000-8000-00805f9b34fb"
#define BLE_CHAR_UUID        "0000bbbb-0000-1000-8000-00805f9b34fb"

// Advertised BLE name = BLE_NAME_PREFIX + 12-hex-char MAC.
// Adv packet budget (flags + service UUID + name) is ~20 bytes, MAC takes 12,
// so the prefix has ~8 chars left. static_assert below catches an oversized
// prefix at compile time instead of it silently getting truncated at runtime.
#define BLE_NAME_PREFIX       "wictmgs-"      // <-- set here, max ~8 chars

// OTA URL stored as hex bytes instead of a plain string, so it doesn't show up
// on a quick strings/grep pass of the source or the compiled binary. This is
// light obfuscation only (not encryption) - decoded back to a string at runtime.
// Decodes to: "compute.widyahong.com/wictmgs/electronics/recovery/firmware.bin"
static const uint8_t OTA_PATH_HEX[] = {
  0x63, 0x6F, 0x6D, 0x70, 0x75, 0x74, 0x65, 0x2E, 0x77, 0x69, 0x64, 0x79,
  0x61, 0x68, 0x6F, 0x6E, 0x67, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x77, 0x69,
  0x63, 0x74, 0x6D, 0x67, 0x73, 0x2F, 0x65, 0x6C, 0x65, 0x63, 0x74, 0x72,
  0x6F, 0x6E, 0x69, 0x63, 0x73, 0x2F, 0x72, 0x65, 0x63, 0x6F, 0x76, 0x65,
  0x72, 0x79, 0x2F, 0x66, 0x69, 0x72, 0x6D, 0x77, 0x61, 0x72, 0x65, 0x2E,
  0x62, 0x69, 0x6E,
};
static const size_t OTA_PATH_HEX_LEN = sizeof(OTA_PATH_HEX);

#define NVS_NAMESPACE          "security"
#define NVS_KEY_MAC            "boundmac"
#define WIFI_CONNECT_TIMEOUT_MS 15000

// --- Feature toggles ---
// Use 1/0 (not true/false) so #if actually strips disabled code at compile
// time (saves flash), unlike a runtime "if (flag)" which still compiles both
// branches and just skips one of them at runtime.
#define ENABLE_SERIAL_PRINT    1     // 0 = drop all Serial calls from the binary
#define ENABLE_BLE_NOTIFY      1     // 0 = drop BLE notify calls (independent of Serial)
#define ENABLE_HTTPS           1     // 0 = plain HTTP, drops WiFiClientSecure code
#define ENABLE_DEVICE_BINDING  1     // 0 = drop MAC anti-clone binding entirely
#define SERIAL_BAUD_RATE       115200
/* ================================================================ */

#include <WiFi.h>
#if ENABLE_HTTPS
  #include <WiFiClientSecure.h>
#endif
#include <HTTPUpdate.h>
#if ENABLE_DEVICE_BINDING
  #include <Preferences.h>
#endif
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Prefix + 12-char MAC must fit the ~20-char adv name budget.
static_assert((sizeof(BLE_NAME_PREFIX) - 1) <= (20 - 12),
              "BLE_NAME_PREFIX too long - MAC needs 12 of the ~20 available chars");

BLEServer         *pServer         = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;
bool deviceLocked    = false;
bool bleActive       = false;

bool   wifiRequest = false;
bool   otaRequest  = false;
String pendingSsid, pendingPass;

/* ---------- Logging: Serial and BLE notify are independent toggles ---------- */
#if ENABLE_SERIAL_PRINT
  #define LOG_SERIAL(msg) Serial.println(msg)
#else
  #define LOG_SERIAL(msg)
#endif

#if ENABLE_BLE_NOTIFY
static inline void notifyBLE(const String &msg) {
  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue((uint8_t*)msg.c_str(), msg.length());
    pCharacteristic->notify();
  }
}
#else
static inline void notifyBLE(const String &msg) {}
#endif

// Single call site for "log this everywhere"; each sink can be disabled on its own.
static inline void deviceLog(const String &msg) {
  LOG_SERIAL(msg);
  notifyBLE(msg);
}

/* ---------- MAC helper (used for binding + BLE name) ---------- */
static String getMacHex() {
  uint64_t mac = ESP.getEfuseMac();
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%012llX", mac);
  return String(macStr);
}

/* ---------- Anti-clone: bind firmware to this chip's MAC ---------- */
#if ENABLE_DEVICE_BINDING
void checkDeviceBinding() {
  String mac = getMacHex();

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  String stored = prefs.getString(NVS_KEY_MAC, "");

  if (stored.length() == 0) {
    prefs.putString(NVS_KEY_MAC, mac);
    deviceLog("[SEC] first boot, bound " + mac);
    deviceLocked = false;
  } else if (stored == mac) {
    deviceLog("[SEC] MAC ok");
    deviceLocked = false;
  } else {
    deviceLog("[SEC] MAC mismatch, locked");
    deviceLocked = true;
  }
  prefs.end();
}
#else
void checkDeviceBinding() {
  deviceLocked = false;
}
#endif

/* ---------- BLE server callbacks ---------- */
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { deviceConnected = true; }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    if (bleActive) {
      BLEDevice::startAdvertising();
    }
  }
};

/* ---------- BLE write handler: WIFI:ssid|pass or OTA ---------- */
class WriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String value = String(pChar->getValue().c_str());
    value.trim();
    if (value.length() == 0) return;

    if (value.startsWith("WIFI:")) {
      String rest = value.substring(5);
      int sep = rest.indexOf('|');
      if (sep > 0) {
        pendingSsid = rest.substring(0, sep);
        pendingPass = rest.substring(sep + 1);
        wifiRequest = true;
      } else {
        deviceLog("ERR: use WIFI:ssid|pass");
      }
    } else if (value == "OTA") {
      otaRequest = true;
    } else {
      deviceLog("ECHO: " + value);
    }
  }
};

/* ---------- Bring BLE up ---------- */
void setupBLE() {
  String bleName = String(BLE_NAME_PREFIX) + getMacHex();

  BLEDevice::init(bleName.c_str());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
      BLE_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setCallbacks(new WriteCallbacks());

  pService->start();
  pServer->getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);
  pServer->getAdvertising()->start();

  bleActive = true;
  deviceLog("[BLE] adv as " + bleName);
}

/* ---------- Tear BLE down to free RAM/flash during OTA ---------- */
void teardownBLE() {
  if (!bleActive) return;
  LOG_SERIAL("[BLE] off for OTA");
  deviceConnected = false;
  BLEDevice::deinit(true);
  pServer = nullptr;
  pCharacteristic = nullptr;
  bleActive = false;
}

/* ---------- Decode OTA_PATH_HEX back to a string + prepend scheme ---------- */
String buildOtaUrl() {
#if ENABLE_HTTPS
  String scheme = "https://";
#else
  String scheme = "http://";
#endif

  String path;
  path.reserve(OTA_PATH_HEX_LEN);
  for (size_t i = 0; i < OTA_PATH_HEX_LEN; i++) {
    path += (char)OTA_PATH_HEX[i];
  }
  return scheme + path;
}

/* ---------- OTA update; BLE goes down first, back up again if OTA fails ---------- */
void doOTA() {
  deviceLog("OTA: start");
  if (WiFi.status() != WL_CONNECTED) {
    deviceLog("OTA: no WiFi");
    return;
  }

  teardownBLE(); // nothing to notify during the update anyway

  String url = buildOtaUrl();
  t_httpUpdate_return ret;
  httpUpdate.rebootOnUpdate(true);

#if ENABLE_HTTPS
  WiFiClientSecure client;
  client.setInsecure();
  ret = httpUpdate.update(client, url);
#else
  WiFiClient client;
  ret = httpUpdate.update(client, url);
#endif

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      LOG_SERIAL("OTA fail: " + httpUpdate.getLastErrorString());
      setupBLE(); // bring BLE back so client can be told / retry
      deviceLog("OTA failed");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      setupBLE();
      deviceLog("OTA: none");
      break;
    case HTTP_UPDATE_OK:
      // Reboots automatically (rebootOnUpdate(true)); no need to restart BLE.
      LOG_SERIAL("OTA ok, rebooting");
      break;
  }
}

void setup() {
#if ENABLE_SERIAL_PRINT
  Serial.begin(SERIAL_BAUD_RATE);
#endif

  checkDeviceBinding();
  if (deviceLocked) {
    LOG_SERIAL("[SEC] halted");
    return; // BLE/WiFi/OTA never start on a MAC-mismatched clone
  }

  setupBLE();
}

void loop() {
  if (deviceLocked) {
    return;
  }

  if (wifiRequest) {
    wifiRequest = false;
    deviceLog("WiFi: connecting " + pendingSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(pendingSsid.c_str(), pendingPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      deviceLog("WiFi ok: " + WiFi.localIP().toString());
    } else {
      deviceLog("WiFi fail");
    }
  }

  if (otaRequest) {
    otaRequest = false;
    doOTA();
  }
}
