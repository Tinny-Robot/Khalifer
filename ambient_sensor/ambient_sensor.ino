/*
 * Cold-room telemetry node
 * ESP32 + DHT22 + SIM800L  ->  HTTPS POST every 60s
 *
 * Arduino IDE:
 *   Board: ESP32 Dev Module (or your generic ESP32)
 *   Libraries (Library Manager):
 *     - TinyGSM by Volodymyr Shymanskyy
 *     - ESP_SSLClient by Mobizt
 *     - DHT sensor library by Adafruit
 *     - Adafruit Unified Sensor
 *
 * Wiring (common GND between both bucks is mandatory)
 *   DHT22 +    -> ESP32 3.3V
 *   DHT22 -    -> GND
 *   DHT22 OUT  -> GPIO 21
 *   SIM800 VCC -> 4.0-4.2V, dedicated LM2596
 *   SIM800 TX  -> ESP32 GPIO 16 (RX)
 *   SIM800 RX  -> ESP32 GPIO 17 (TX)
 *   SIM800 GND -> GND
 *   SIM800 RST -> GPIO 4 only if physically wired
 *
 * Power:
 *   12V input -> LM2596 #1 -> 5V -> ESP32 VIN
 *             -> LM2596 #2 -> 4.0V -> SIM800L
 *   Keep the SIM800L supply short and low impedance.
 *   Bulk capacitors should be placed close to the SIM800L.
 *
 * UART levels:
 *   ESP32 GPIO is 3.3V even when the board is fed 5V on VIN.
 *   SIM800 TX -> ESP32 RX is safe as-is.
 *   ESP32 TX -> SIM800 RX should be level-safe; a divider/series resistor
 *   may be used depending on the SIM800L board.
 */


// ---------------- libraries / modem ----------------

#include <Arduino.h>
#include <DHT.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <ESP_SSLClient.h>
#include <stdint.h>

// ---------------- user config ----------------

static const char DEVICE_ID[] = "6a902454481962452192348c";  // sent as "unit"

// Leave empty to auto-pick a Nigerian APN from the SIM (MTN/Airtel/Glo/9mobile).
// Set only if auto-detect fails, e.g. "web.gprs.mtnnigeria.net"
static const char APN_OVERRIDE[] = "";
static const char APN_USER_OVERRIDE[] = "";
static const char APN_PASS_OVERRIDE[] = "";

static const char HOST[] = "soltech-backend-drab.vercel.app";
static const char PATH[] = "/api/v1/telemetry";
static const uint16_t HTTPS_PORT = 443;

static const uint32_t POST_INTERVAL_MS = 60000;

#define PIN_DHT             21  // DHT22 OUT -> GPIO 21
#define DHT_TYPE            DHT22
#define PIN_MODEM_RX       16  // ESP32 RX  <- SIM800 TX
#define PIN_MODEM_TX       17  // ESP32 TX  -> SIM800 RX
#define PIN_MODEM_PWRKEY   -1  // set to 4 if you wire PWRKEY; -1 = unused
#define PIN_MODEM_RST      -1  // set if RST is wired; -1 = unused

#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024
#define TINY_GSM_YIELD_MS  2
#define TINY_GSM_YIELD()   { delay(1); esp_task_wdt_reset(); }

#define SerialMon Serial
#define SerialAT  Serial2

#define SSLCLIENT_INSECURE_ONLY
#define SSLCLIENT_HALF_DUPLEX

#include <TinyGsmClient.h>



// ---------------- constants ----------------

static const uint32_t WDT_TIMEOUT_S       = 180;
static const uint32_t NETWORK_TIMEOUT_MS  = 90000;
static const uint32_t GPRS_TIMEOUT_MS     = 60000;
static const uint32_t TLS_TIMEOUT_MS      = 60000;
static const uint32_t HTTP_TIMEOUT_MS     = 45000;
static const uint8_t  SENSOR_RETRIES      = 3;
static const uint8_t  POST_ATTEMPTS       = 2;
static const uint8_t  FAILS_BEFORE_GPRS   = 2;
static const uint8_t  FAILS_BEFORE_MODEM  = 4;
static const uint8_t  FAILS_BEFORE_REBOOT = 6;

struct CarrierApn {
  const char* mnc;
  const char* name;
  const char* apn;
  const char* user;
  const char* pass;
};

static const CarrierApn kCarriers[] = {
  {"20", "Airtel",  "internet.ng.airtel.com",     "",        ""},
  {"30", "MTN",     "web.gprs.mtnnigeria.net",    "web",     "web"},
  {"50", "Glo",     "gloflat",                    "flat",    "flat"},
  {"60", "9mobile", "9mobile",                    "9mobile", "9mobile"},
};

static const char* const kFallbackApns[] = {
  "internet",
  "web",
  "web.gprs.mtnnigeria.net",
  "internet.ng.airtel.com",
  "gloflat",
  "9mobile",
};

TinyGsm        modem(SerialAT);
TinyGsmClient  gsmClient(modem);
ESP_SSLClient  sslClient;
DHT             dht(PIN_DHT, DHT_TYPE);

static char   gApn[40]  = "";
static char   gUser[16] = "";
static char   gPass[16] = "";
static uint8_t gFailStreak = 0;

// ---------------- watchdog / wait ----------------

static void feedWatchdog() {
  esp_task_wdt_reset();
}

static void waitWithWatchdog(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    feedWatchdog();
    delay(50);
  }
}

static void watchdogBegin() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t cfg = {
      .timeout_ms = WDT_TIMEOUT_S * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
    esp_task_wdt_init(&cfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// ---------------- DHT22 sensor ----------------

static bool dht22Begin() {
  dht.begin();
  // Give the sensor time to stabilize after startup.
  waitWithWatchdog(2000);
  return true;
}

static bool readSensor(float* temperatureC, int* humidityPercent) {
  for (uint8_t i = 0; i < SENSOR_RETRIES; i++) {
    feedWatchdog();

    const float humidity = dht.readHumidity();
    const float temperature = dht.readTemperature();

    if (!isnan(humidity) && !isnan(temperature) &&
        temperature >= -40.0f && temperature <= 80.0f &&
        humidity >= 0.0f && humidity <= 100.0f) {
      *temperatureC = temperature;
      *humidityPercent = (int)(humidity + 0.5f);

      if (*humidityPercent > 100) {
        *humidityPercent = 100;
      }

      return true;
    }

    SerialMon.printf("DHT22 read failed (%u/%u)\\n",
                     i + 1, SENSOR_RETRIES);

    // DHT22 should not be read more frequently than about once every
    // two seconds. This also gives the sensor time to recover from a
    // failed checksum/read.
    waitWithWatchdog(2000);
  }

  SerialMon.println("DHT22 read failed after all retries");
  return false;
}

// ---------------- modem power / UART ----------------

static void modemPulsePwrKey() {
  if (PIN_MODEM_PWRKEY < 0) {
    return;
  }
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  delay(1100);  // SIM800 power-toggle pulse
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  waitWithWatchdog(5000);
}

static void modemHardwareReset() {
  if (PIN_MODEM_RST < 0) {
    return;
  }
  pinMode(PIN_MODEM_RST, OUTPUT);
  digitalWrite(PIN_MODEM_RST, LOW);
  delay(200);
  digitalWrite(PIN_MODEM_RST, HIGH);
  waitWithWatchdog(4000);
}

static bool modemAtAtBaud(uint32_t baud) {
  SerialAT.end();
  delay(50);
  SerialAT.begin(baud, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
  delay(200);
  for (uint8_t i = 0; i < 5; i++) {
    feedWatchdog();
    if (modem.testAT(1000)) {
      return true;
    }
  }
  return false;
}

static bool modemLockBaud() {
  static const uint32_t kBauds[] = {9600, 115200, 57600, 19200, 38400};
  for (uint8_t i = 0; i < sizeof(kBauds) / sizeof(kBauds[0]); i++) {
    SerialMon.printf("Modem AT @ %lu ...\n", (unsigned long)kBauds[i]);
    if (modemAtAtBaud(kBauds[i])) {
      if (kBauds[i] != 9600) {
        modem.setBaud(9600);
        waitWithWatchdog(300);
        SerialAT.begin(9600, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
        waitWithWatchdog(200);
        if (!modem.testAT(1000)) {
          SerialMon.println("Failed to switch modem to 9600");
          return false;
        }
      }
      SerialMon.println("Modem UART OK at 9600");
      return true;
    }
  }
  return false;
}

static bool modemPowerUp() {
  if (PIN_MODEM_RST >= 0) {
    pinMode(PIN_MODEM_RST, OUTPUT);
    digitalWrite(PIN_MODEM_RST, HIGH);
  }
  if (PIN_MODEM_PWRKEY >= 0) {
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  }

  waitWithWatchdog(2500);
  if (modemLockBaud()) {
    return true;
  }

  SerialMon.println("No AT response - pulsing PWRKEY / RST");
  modemHardwareReset();
  modemPulsePwrKey();
  return modemLockBaud();
}

// ---------------- APN / GPRS ----------------

static void setApn(const char* apn, const char* user, const char* pass) {
  strncpy(gApn, apn ? apn : "", sizeof(gApn) - 1);
  strncpy(gUser, user ? user : "", sizeof(gUser) - 1);
  strncpy(gPass, pass ? pass : "", sizeof(gPass) - 1);
  gApn[sizeof(gApn) - 1] = 0;
  gUser[sizeof(gUser) - 1] = 0;
  gPass[sizeof(gPass) - 1] = 0;
}

static bool lookupNigerianApn() {
  const String imsi = modem.getIMSI();
  SerialMon.print("IMSI: ");
  SerialMon.println(imsi.length() ? imsi : "(none)");

  String mnc = "";
  if (imsi.length() >= 5 && imsi.startsWith("621")) {
    mnc = imsi.substring(3, 5);
  }

  const String ops = modem.getOperator();
  SerialMon.print("Operator: ");
  SerialMon.println(ops.length() ? ops : "(none)");

  if (mnc.length() == 0) {
    String lower = ops;
    lower.toLowerCase();
    if (lower.indexOf("mtn") >= 0) {
      mnc = "30";
    } else if (lower.indexOf("airtel") >= 0) {
      mnc = "20";
    } else if (lower.indexOf("glo") >= 0 || lower.indexOf("globacom") >= 0) {
      mnc = "50";
    } else if (lower.indexOf("9mobile") >= 0 || lower.indexOf("etisalat") >= 0) {
      mnc = "60";
    }
  }

  for (uint8_t i = 0; i < sizeof(kCarriers) / sizeof(kCarriers[0]); i++) {
    if (mnc == kCarriers[i].mnc) {
      setApn(kCarriers[i].apn, kCarriers[i].user, kCarriers[i].pass);
      SerialMon.printf("APN auto: %s (%s)\n", kCarriers[i].name, gApn);
      return true;
    }
  }
  return false;
}

static bool gprsConnectOnce(const char* apn, const char* user, const char* pass) {
  SerialMon.printf("GPRS attach APN='%s'\n", apn);
  modem.gprsDisconnect();
  waitWithWatchdog(500);
  const bool ok = modem.gprsConnect(apn, user, pass);
  if (ok && modem.isGprsConnected()) {
    setApn(apn, user, pass);
    SerialMon.print("GPRS IP: ");
    SerialMon.println(modem.getLocalIP());
    return true;
  }
  return false;
}

static bool gprsConnectWithFallbacks() {
  if (gApn[0] != 0) {
    if (gprsConnectOnce(gApn, gUser, gPass)) {
      return true;
    }
    if ((gUser[0] != 0 || gPass[0] != 0) && gprsConnectOnce(gApn, "", "")) {
      return true;
    }
  }

  if (APN_OVERRIDE[0] != 0) {
    if (gprsConnectOnce(APN_OVERRIDE, APN_USER_OVERRIDE, APN_PASS_OVERRIDE)) {
      return true;
    }
  } else if (lookupNigerianApn()) {
    if (gprsConnectOnce(gApn, gUser, gPass)) {
      return true;
    }
    if ((gUser[0] != 0 || gPass[0] != 0) && gprsConnectOnce(gApn, "", "")) {
      return true;
    }
  }

  for (uint8_t i = 0; i < sizeof(kFallbackApns) / sizeof(kFallbackApns[0]); i++) {
    feedWatchdog();
    if (strcmp(kFallbackApns[i], gApn) == 0) {
      continue;
    }
    if (gprsConnectOnce(kFallbackApns[i], "", "")) {
      return true;
    }
  }
  return false;
}

static bool waitForCellNetwork() {
  SerialMon.println("Waiting for GSM registration...");
  if (!modem.waitForNetwork(NETWORK_TIMEOUT_MS, true)) {
    SerialMon.println("Network registration failed");
    return false;
  }

  const int csq = modem.getSignalQuality();
  SerialMon.printf("Registered. CSQ=%d (0-31, 99=unknown)\n", csq);
  if (csq == 0 || csq == 99) {
    SerialMon.println("Warning: very weak / unknown signal");
  }
  return true;
}

static bool modemBringUp() {
  feedWatchdog();
  if (!modemPowerUp()) {
    SerialMon.println("Modem not responding");
    return false;
  }

  if (!modem.init()) {
    SerialMon.println("modem.init failed, trying restart");
    if (!modem.restart()) {
      return false;
    }
  }

  SerialMon.print("Modem: ");
  SerialMon.println(modem.getModemInfo());

  const SimStatus sim = modem.getSimStatus();
  if (sim != SIM_READY) {
    SerialMon.printf("SIM not ready (%d). Waiting...\n", (int)sim);
    waitWithWatchdog(4000);
    if (modem.getSimStatus() != SIM_READY) {
      SerialMon.println("Insert a working 2G SIM and reset");
      return false;
    }
  }

  if (!waitForCellNetwork()) {
    return false;
  }
  return gprsConnectWithFallbacks();
}

static bool ensureGprs() {
  if (modem.isNetworkConnected() && modem.isGprsConnected()) {
    return true;
  }
  SerialMon.println("GPRS dropped - reconnecting");
  if (!modem.isNetworkConnected() && !waitForCellNetwork()) {
    return false;
  }
  if (gApn[0] != 0) {
    if (gprsConnectOnce(gApn, gUser, gPass)) {
      return true;
    }
    if ((gUser[0] != 0 || gPass[0] != 0) && gprsConnectOnce(gApn, "", "")) {
      return true;
    }
  }
  return gprsConnectWithFallbacks();
}

static bool modemFullReset() {
  SerialMon.println("Full modem reset");
  gsmClient.stop();
  sslClient.stop();
  modem.gprsDisconnect();
  waitWithWatchdog(500);
  modem.restart();
  waitWithWatchdog(4000);
  return modemBringUp();
}

// ---------------- HTTPS POST ----------------

static void sslPrepare() {
  sslClient.setInsecure();  // TLS 1.2 on ESP32; skip CA pin so Vercel cert rotation cannot brick the node
  sslClient.setBufferSizes(2048, 1024);
  sslClient.setDebugLevel(0);
  sslClient.setClient(&gsmClient);
}

static bool bodySaysSuccess(const char* body) {
  const char* p = strstr(body, "\"success\"");
  if (!p) {
    return false;
  }
  p = strchr(p, ':');
  if (!p) {
    return false;
  }
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  return strncmp(p, "true", 4) == 0;
}

static int parseStatusCode(const char* response) {
  const char* p = strstr(response, "HTTP/");
  if (!p) {
    return 0;
  }
  p = strchr(p, ' ');
  if (!p) {
    return 0;
  }
  return atoi(p + 1);
}

static const char* findBody(const char* response) {
  const char* body = strstr(response, "\r\n\r\n");
  if (body) {
    return body + 4;
  }
  body = strstr(response, "\n\n");
  return body ? body + 2 : response;
}

static bool httpsPost(float temperatureC, int humidityPercent) {
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"unit\":\"%s\",\"temperatureC\":%.2f,\"humidityPercent\":%d}",
           DEVICE_ID, temperatureC, humidityPercent);

  SerialMon.print("POST ");
  SerialMon.println(payload);

  if (!ensureGprs()) {
    SerialMon.println("No GPRS");
    return false;
  }

  sslPrepare();
  gsmClient.setTimeout(TLS_TIMEOUT_MS);
  sslClient.setTimeout(TLS_TIMEOUT_MS);

  SerialMon.printf("TLS connect %s:%u ...\n", HOST, HTTPS_PORT);
  if (!sslClient.connect(HOST, HTTPS_PORT)) {
    SerialMon.println("TLS connect failed");
    sslClient.stop();
    gsmClient.stop();
    return false;
  }

  sslClient.print("POST ");
  sslClient.print(PATH);
  sslClient.print(" HTTP/1.1\r\n");
  sslClient.print("Host: ");
  sslClient.print(HOST);
  sslClient.print("\r\n");
  sslClient.print("User-Agent: coldroom-dht22/1.0\r\n");
  sslClient.print("Accept: application/json\r\n");
  sslClient.print("Content-Type: application/json\r\n");
  sslClient.print("Content-Length: ");
  sslClient.print(strlen(payload));
  sslClient.print("\r\nConnection: close\r\n\r\n");
  sslClient.print(payload);
  sslClient.flush();

  char response[1024];
  size_t n = 0;
  const uint32_t start = millis();
  bool sawData = false;

  while (millis() - start < HTTP_TIMEOUT_MS && n < sizeof(response) - 1) {
    feedWatchdog();
    while (sslClient.available() && n < sizeof(response) - 1) {
      response[n++] = (char)sslClient.read();
      sawData = true;
    }
    if (sawData && !sslClient.connected() && !sslClient.available()) {
      break;
    }
    delay(10);
  }
  response[n] = 0;
  sslClient.stop();
  gsmClient.stop();

  if (n == 0) {
    SerialMon.println("Empty HTTP response");
    return false;
  }

  const int status = parseStatusCode(response);
  const char* body = findBody(response);
  SerialMon.printf("HTTP %d (%u bytes)\n", status, (unsigned)n);
  SerialMon.println(body);

  if (status != 200 && status != 201) {
    return false;
  }
  if (!bodySaysSuccess(body)) {
    SerialMon.println("200 but success != true");
    return false;
  }
  return true;
}

static bool postLatest(float temperatureC, int humidityPercent) {
  for (uint8_t attempt = 1; attempt <= POST_ATTEMPTS; attempt++) {
    feedWatchdog();
    if (httpsPost(temperatureC, humidityPercent)) {
      return true;
    }
    SerialMon.printf("Post attempt %u/%u failed\n", attempt, POST_ATTEMPTS);
    waitWithWatchdog(1500);
  }
  return false;
}

static void onPostOutcome(bool ok) {
  if (ok) {
    gFailStreak = 0;
    SerialMon.println("Telemetry OK");
    return;
  }

  gFailStreak++;
  SerialMon.printf("Fail streak %u\n", gFailStreak);

  if (gFailStreak == FAILS_BEFORE_GPRS) {
    ensureGprs();
  } else if (gFailStreak == FAILS_BEFORE_MODEM) {
    modemFullReset();
  } else if (gFailStreak >= FAILS_BEFORE_REBOOT) {
    SerialMon.println("Too many failures - rebooting");
    waitWithWatchdog(500);
    ESP.restart();
  }
}

// ---------------- Arduino entry ----------------

void setup() {
  SerialMon.begin(115200);
  delay(200);
  SerialMon.println();
  SerialMon.println("Cold-room DHT22 node boot");
  SerialMon.printf("unit=%s interval=%lus\n", DEVICE_ID, POST_INTERVAL_MS / 1000);

  watchdogBegin();

  if (!dht22Begin()) {
    SerialMon.println("DHT22 initialization failed");
  } else {
    SerialMon.println("DHT22 OK");
  }

  uint8_t tries = 0;
  while (!modemBringUp()) {
    tries++;
    SerialMon.printf("Modem bring-up failed (%u). Retrying...\n", tries);
    waitWithWatchdog(5000 + (tries > 3 ? 15000 : 0));
    if (tries >= 8) {
      SerialMon.println("Modem bring-up giving up - reboot");
      ESP.restart();
    }
  }
}

void loop() {
  const uint32_t cycleStart = millis();
  feedWatchdog();

  float temperatureC = 0;
  int humidityPercent = 0;
  if (!readSensor(&temperatureC, &humidityPercent)) {
    SerialMon.println("No valid reading this cycle");
  } else {
    SerialMon.printf("DHT22 %.2f C  %d %%RH\n", temperatureC, humidityPercent);
    onPostOutcome(postLatest(temperatureC, humidityPercent));
  }

  const uint32_t elapsed = millis() - cycleStart;
  if (elapsed < POST_INTERVAL_MS) {
    SerialMon.printf("Sleeping %lu ms\n", (unsigned long)(POST_INTERVAL_MS - elapsed));
    waitWithWatchdog(POST_INTERVAL_MS - elapsed);
  }
}
