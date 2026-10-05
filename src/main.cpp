#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <EEPROM.h> 
#include <RTClib.h>
#include <sys/time.h>
#include "portal_head.h" // Generado desde portal/portal.css en build (scripts/embed_portal.py)
#include "device_credentials.h" // Credenciales de la cuenta robot (NO commiteado)

// Identidad de Firmware
#define BOARD_TYPE "esp8266"
#define FIRMWARE_VERSION "1.2.0"

#define DHTPIN D7       // Pin de datos del DHT22
#define DHTTYPE DHT22   // Tipo de sensor

#define RESET_PIN 0     // Botón Flash del NodeMCU (GPIO 0 / D3)
#define RELAY_PIN D5    // Pin para controlar actuadores (Ej: Ventilador o Luz). Liberamos D1 (SCL) y D2 (SDA) para el RTC

DHT dht(DHTPIN, DHTTYPE);
RTC_DS3231 rtc;


// Configuración de Firebase
#define FIREBASE_HOST "plantix-9c6a4-default-rtdb.firebaseio.com"

FirebaseData fbData;
FirebaseData streamData;
FirebaseConfig fbConfig;
FirebaseAuth fbAuth;

String deviceMac = "";
bool isLinked = false;
String pairingPin = "";
bool pinUploaded = false;
String deviceToken = "";

unsigned long lastSensorReadTime = 0;
unsigned long lastHistoryUploadTime = 0;
unsigned long buttonPressStartTime = 0;
bool isButtonPressed = false;
volatile bool forceConfigUpdate = true;
bool isOverrideActive = false;
unsigned long overrideStartTime = 0;
bool overrideRelayState = false;


// Schedule Settings
bool lightIsOn = false;         
String lightMode = "manual";    
int lightOnHour = 6;
int lightOnMin = 0;
int lightOffHour = 18;
int lightOffMin = 0;

// Estilos y UI Chlorophyll Glass para WiFiManager.
// El CSS vive en portal/portal.css y se embebe en src/portal_head.h durante el build.
// Para previsualizar en el navegador: abrir portal/portal-preview.html

// Variables para el sobremuestreo del sensor
int consecutiveSensorFailures = 0;
float sumTemp = 0;
float sumHum = 0;
int readCount = 0;

const int MAX_TOKEN_LEN = 64;

// Multi-WiFi Configuración
ESP8266WiFiMulti wifiMulti;

struct SavedWiFi {
  char ssid[33];
  char pass[65];
};

const int MAX_SAVED_WIFI = 3;
const int EEPROM_WIFI_COUNT_ADDR = 100;
const int EEPROM_WIFI_START_ADDR = 101;
const int EEPROM_OTA_FLAG_ADDR = 400;
const int EEPROM_OTA_URL_ADDR = 401;

void saveWiFiCredentials(String ssid, String pass) {
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) return;
  
  byte count = EEPROM.read(EEPROM_WIFI_COUNT_ADDR);
  if (count > MAX_SAVED_WIFI) count = 0;
  
  SavedWiFi list[MAX_SAVED_WIFI];
  // Leer redes existentes
  for (int i = 0; i < count; i++) {
    int addr = EEPROM_WIFI_START_ADDR + (i * sizeof(SavedWiFi));
    EEPROM.get(addr, list[i]);
    // Si ya existe la red, actualizar contraseña
    if (String(list[i].ssid) == ssid) {
      strncpy(list[i].pass, pass.c_str(), sizeof(list[i].pass) - 1);
      list[i].pass[sizeof(list[i].pass) - 1] = '\0';
      EEPROM.put(addr, list[i]);
      EEPROM.commit();
      Serial.printf("[Multi-WiFi] Contraseña actualizada para red: %s\n", ssid.c_str());
      return;
    }
  }
  
  // Guardar nueva red
  int targetIndex = 0;
  if (count < MAX_SAVED_WIFI) {
    targetIndex = count;
    count++;
    EEPROM.write(EEPROM_WIFI_COUNT_ADDR, count);
  } else {
    // Si llegamos al máximo, desplazamos para guardar la más reciente
    for (int i = 0; i < MAX_SAVED_WIFI - 1; i++) {
      list[i] = list[i + 1];
      int addr = EEPROM_WIFI_START_ADDR + (i * sizeof(SavedWiFi));
      EEPROM.put(addr, list[i]);
    }
    targetIndex = MAX_SAVED_WIFI - 1;
  }
  
  SavedWiFi newNet;
  memset(&newNet, 0, sizeof(SavedWiFi));
  strncpy(newNet.ssid, ssid.c_str(), sizeof(newNet.ssid) - 1);
  strncpy(newNet.pass, pass.c_str(), sizeof(newNet.pass) - 1);
  
  int addr = EEPROM_WIFI_START_ADDR + (targetIndex * sizeof(SavedWiFi));
  EEPROM.put(addr, newNet);
  EEPROM.commit();
  Serial.printf("[Multi-WiFi] Red guardada exitosamente (%d/%d): %s\n", count, MAX_SAVED_WIFI, ssid.c_str());
}

void loadAndRegisterMultiWiFi() {
  byte count = EEPROM.read(EEPROM_WIFI_COUNT_ADDR);
  if (count > MAX_SAVED_WIFI) count = 0;
  
  Serial.printf("[Multi-WiFi] Cargando %d redes guardadas...\n", count);
  for (int i = 0; i < count; i++) {
    SavedWiFi net;
    int addr = EEPROM_WIFI_START_ADDR + (i * sizeof(SavedWiFi));
    EEPROM.get(addr, net);
    bool valid = (strlen(net.ssid) > 0 && strlen(net.ssid) <= 32);
    for (size_t s = 0; s < strlen(net.ssid); s++) {
      if ((unsigned char)net.ssid[s] < 32 || (unsigned char)net.ssid[s] > 126) valid = false;
    }
    if (valid) {
      wifiMulti.addAP(net.ssid, net.pass);
      Serial.printf("  -> Red %d: %s\n", i + 1, net.ssid);
    }
  }
  
  // También agregar la red que tenga guardada el SDK por defecto
  String defaultSSID = WiFi.SSID();
  String defaultPSK = WiFi.psk();
  if (defaultSSID.length() > 0) {
    wifiMulti.addAP(defaultSSID.c_str(), defaultPSK.c_str());
    Serial.printf("  -> Red SDK: %s\n", defaultSSID.c_str());
  }
}

void saveTokenToEEPROM(String token) {
  for (int i = 0; i < MAX_TOKEN_LEN; ++i) {
    if (i < (int)token.length()) {
      EEPROM.write(i, token[i]);
    } else {
      EEPROM.write(i, 0);
    }
  }
  EEPROM.commit();
  Serial.println("Token guardado en EEPROM.");
}

String loadTokenFromEEPROM() {
  String token = "";
  for (int i = 0; i < MAX_TOKEN_LEN; ++i) {
    char c = EEPROM.read(i);
    if (c == 0 || c == 255) break; // Si está vacío o borrado
    token += c;
  }
  return token;
}

void saveScheduleToEEPROM() {
  EEPROM.write(64, lightMode == "auto" ? 1 : 0);
  EEPROM.write(65, lightOnHour);
  EEPROM.write(66, lightOnMin);
  EEPROM.write(67, lightOffHour);
  EEPROM.write(68, lightOffMin);
  EEPROM.commit();
  Serial.println("Horarios guardados en EEPROM.");
}

void loadScheduleFromEEPROM() {
  byte mode = EEPROM.read(64);
  lightMode = (mode == 1) ? "auto" : "manual";
  
  lightOnHour = EEPROM.read(65);
  lightOnMin = EEPROM.read(66);
  lightOffHour = EEPROM.read(67);
  lightOffMin = EEPROM.read(68);
  
  if (lightOnHour > 23) lightOnHour = 6;
  if (lightOnMin > 59) lightOnMin = 0;
  if (lightOffHour > 23) lightOffHour = 18;
  if (lightOffMin > 59) lightOffMin = 0;
  
  Serial.printf("Horarios cargados de EEPROM: Modo=%s, ON=%02d:%02d, OFF=%02d:%02d\n", 
                lightMode.c_str(), lightOnHour, lightOnMin, lightOffHour, lightOffMin);
}

void streamCallback(FirebaseStream data) {
  forceConfigUpdate = true;
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, reconectando...");
}

void performCloudOTA(String url, String targetVersion, String targetBoard) {
  if (targetBoard != "" && targetBoard != BOARD_TYPE) {
    Serial.printf("[OTA] Rechazado: La actualización es para placa '%s', este dispositivo es '%s'\n", targetBoard.c_str(), BOARD_TYPE);
    return;
  }

  if (targetVersion == FIRMWARE_VERSION) {
    Serial.printf("[OTA] El dispositivo ya está en la versión objetivo (%s). Omitiendo.\n", FIRMWARE_VERSION);
    return;
  }

  Serial.printf("\n[OTA] Iniciando actualización a v%s desde:\n%s\n", targetVersion.c_str(), url.c_str());

  // Notificar a Firebase el cambio de estado
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/status", "updating");
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "downloading");

  // Guardar flag OTA (1 = pendiente) en dirección dedicada
  EEPROM.write(EEPROM_OTA_FLAG_ADDR, 1);
  // Guardar URL hasta 200 caracteres en dir dedicada
  int maxUrlLen = 200;
  for (int i = 0; i < maxUrlLen; i++) {
    if (i < (int)url.length()) {
      EEPROM.write(EEPROM_OTA_URL_ADDR + i, url[i]);
    } else {
      EEPROM.write(EEPROM_OTA_URL_ADDR + i, 0);
    }
  }
  EEPROM.commit();

  Serial.println("[OTA] Flag guardado en EEPROM. Reiniciando para OTA con memoria limpia...");
  delay(1000);
  ESP.restart();
}

String otaBootErrorMessage = "";

bool isValidPairingPin(String pin) {
  if (pin.length() != 6) return false;
  for (unsigned int i = 0; i < pin.length(); i++) {
    if (!isdigit(pin[i])) return false;
  }
  return true;
}

String htmlEscape(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  text.replace("\"", "&quot;");
  text.replace("'", "&#39;");
  return text;
}

String scanWifiOptions() {
  String options = "";
  int networks = WiFi.scanNetworks(false, true);
  if (networks > 0) {
    for (int i = 0; i < networks; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      String safeSsid = htmlEscape(ssid);
      options += "<option value='" + safeSsid + "'>" + safeSsid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  WiFi.scanDelete();
  return options;
}

String buildPortalStart() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  page += "<title>PLANNTIX</title>";
  page += PLANNTIX_CUSTOM_HEAD;
  page += "</head><body><div class='wrap'><h1>PLANNTIX</h1><h3>Vinculaci&oacute;n</h3>";
  return page;
}

String buildPinPortalPage(String message = "") {
  String page = buildPortalStart();
  page += "<p class='connected-network'>Conectado a <strong>" + htmlEscape(WiFi.SSID()) + "</strong></p>";
  page += "<form method='POST' action='/pin'>";
  page += "<label for='pin'>PIN de Vinculaci&oacute;n</label>";
  page += "<input id='pin' name='pin' maxlength='6' placeholder='123456' inputmode='numeric' pattern='[0-9]*'>";
  page += "<p style='text-align:center;margin:-4px 0 16px 0;'>Ingresa el c&oacute;digo generado en tu App PLANNTIX</p>";
  page += "<button type='submit'>Vincular</button></form>";
  page += "<a class='secondary-button' href='/wifi'>Cambiar red WiFi</a>";
  if (message.length() > 0) page += message;
  page += "</div></body></html>";
  return page;
}

String buildWifiPortalPage(String wifiOptions, String message = "") {
  String page = buildPortalStart();
  page += "<p class='connected-network'>Red actual <strong>" + htmlEscape(WiFi.SSID()) + "</strong></p>";
  if (message.length() > 0) page += message;
  page += "<form method='POST' action='/save-wifi'>";
  page += "<label for='s'>Nueva red WiFi</label>";

  if (wifiOptions.length() > 0) {
    page += "<select id='s' name='s'>";
    page += "<option value='' disabled selected>Seleccionar red...</option>";
    page += wifiOptions;
    page += "</select>";
  } else {
    page += "<input id='s' name='s' maxlength='32' placeholder='Nombre de la red'>";
  }

  page += "<label for='p'>Contrase&ntilde;a</label>";
  page += "<input id='p' name='p' type='password' maxlength='64' placeholder='Contrase&ntilde;a de la red'>";
  page += "<button type='submit'>Guardar red</button></form>";
  page += "<form method='GET' action='/'><button class='secondary' type='submit'>Volver</button></form>";
  page += "</div></body></html>";
  return page;
}

int startPinOnlyPortal(String &providedPin) {
  const IPAddress apIP(10, 0, 1, 1);
  const IPAddress netMsk(255, 255, 255, 0);
  DNSServer dnsServer;
  ESP8266WebServer server(80);
  bool pinReceived = false;
  String errorMessage = "";

  String wifiOptions = scanWifiOptions();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  if (!WiFi.softAP("PLANNTIX-Config")) {
    Serial.println("[PIN-ONLY] No se pudo iniciar el AP del portal.");
    return 0;
  }

  dnsServer.start(53, "*", apIP);

  server.on("/", HTTP_GET, [&]() {
    server.send(200, "text/html", buildPinPortalPage(errorMessage));
  });

  server.on("/pin", HTTP_POST, [&]() {
    String pin = server.arg("pin");
    if (isValidPairingPin(pin)) {
      providedPin = pin;
      pinReceived = true;
      server.send(200, "text/html", buildPinPortalPage("<div class='msg S'>PIN recibido. Continuando vinculaci&oacute;n...</div>"));
    } else {
      errorMessage = "<div class='msg D'>El PIN debe tener exactamente 6 d&iacute;gitos.</div>";
      server.send(200, "text/html", buildPinPortalPage(errorMessage));
    }
  });

  server.on("/wifi", HTTP_GET, [&]() {
    server.send(200, "text/html", buildWifiPortalPage(wifiOptions));
  });

  server.on("/change-wifi", HTTP_GET, [&]() {
    server.send(200, "text/html", buildWifiPortalPage(wifiOptions));
  });

  server.on("/save-wifi", HTTP_POST, [&]() {
    String ssid = server.arg("s");
    String pass = server.arg("p");
    if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
      server.send(200, "text/html", buildWifiPortalPage(wifiOptions, "<div class='msg D'>Red o contrase&ntilde;a inv&aacute;lida.</div>"));
      return;
    }

    Serial.printf("[PIN-ONLY] Intentando conectar a nueva red: %s\n", ssid.c_str());
    WiFi.persistent(false); // No guardar credenciales del SDK si la prueba falla.
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long connectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 15000) {
      delay(250);
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      saveWiFiCredentials(ssid, pass);
      errorMessage = "<div class='msg S'>Red guardada. Ahora ingres&aacute; el PIN.</div>";
      server.send(200, "text/html", buildPinPortalPage(errorMessage));
    } else {
      WiFi.disconnect(false);
      wifiMulti.run(8000);
      errorMessage = "<div class='msg D'>No se pudo conectar. No se guard&oacute; la red.</div>";
      server.send(200, "text/html", buildPinPortalPage(errorMessage));
    }
  });

  server.onNotFound([&]() {
    server.send(200, "text/html", buildPinPortalPage(errorMessage));
  });

  server.begin();
  Serial.println("[PIN-ONLY] Portal iniciado en http://10.0.1.1 para ingresar PIN.");

  unsigned long startTime = millis();
  while (!pinReceived && millis() - startTime < 180000) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2);
    yield();
  }

  if (pinReceived) delay(900);
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  if (pinReceived) return 1;
  return 0;
}

void executePendingOTA() {
  if (EEPROM.read(EEPROM_OTA_FLAG_ADDR) == 1) { // Hay un OTA pendiente
    EEPROM.write(EEPROM_OTA_FLAG_ADDR, 0); // Limpiar flag para no ciclar
    EEPROM.commit();

    String url = "";
    for (int i = 0; i < 200; i++) {
      char c = EEPROM.read(EEPROM_OTA_URL_ADDR + i);
      if (c == 0) break;
      url += c;
    }

    if (url.length() < 5) {
      Serial.println("[OTA BOOT] URL inválida en EEPROM.");
      return;
    }

    Serial.println("\n[OTA BOOT] Iniciando actualización limpia desde:");
    Serial.println(url);
    Serial.printf("[OTA BOOT] RAM libre antes de OTA: %u bytes\n", ESP.getFreeHeap());

    ESPhttpUpdate.setClientTimeout(60000);
    ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    ESPhttpUpdate.rebootOnUpdate(true);

    t_httpUpdate_return ret;
    if (url.startsWith("https://")) {
      WiFiClientSecure client;
      client.setInsecure();
      client.setBufferSizes(5120, 512); // Buffer de 5 KB óptimo para Fastly/GitHub CDN
      client.setTimeout(60000);
      ret = ESPhttpUpdate.update(client, url);
    } else {
      WiFiClient client;
      client.setTimeout(30000);
      ret = ESPhttpUpdate.update(client, url);
    }

    if (ret == HTTP_UPDATE_FAILED) {
      otaBootErrorMessage = ESPhttpUpdate.getLastErrorString();
      if (otaBootErrorMessage.length() == 0) {
        otaBootErrorMessage = "Error desconocido (" + String(ESPhttpUpdate.getLastError()) + ")";
      }
      Serial.printf("[OTA BOOT ERROR] (%d): %s\n", ESPhttpUpdate.getLastError(), otaBootErrorMessage.c_str());
    }
  }
}

void factoryReset() {
  Serial.println("Iniciando Reseteo de Fábrica...");
  // Borrar EEPROM
  for (int i = 0; i < 512; ++i) EEPROM.write(i, 255);
  EEPROM.commit();
  Serial.println("EEPROM borrada.");
  
  // Borrar WiFi credentials
  WiFiManager wm;
  wm.resetSettings();
  Serial.println("Credenciales Wi-Fi borradas. Reiniciando...");
  delay(1000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Iniciando PLANNTIX-DEVICES (PRO) ---");
  
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Apagado por defecto
  
  EEPROM.begin(1024);
  dht.begin();

  // Inicializar RTC DS3231
  if (!rtc.begin()) {
    Serial.println("[ADVERTENCIA] No se pudo encontrar el RTC DS3231. Verifica las conexiones.");
  } else {
    Serial.println("RTC DS3231 inicializado correctamente.");
    if (rtc.lostPower()) {
      Serial.println("[ADVERTENCIA] El RTC perdió energía. Se reajustará cuando se sincronice la hora NTP.");
    }
  }

  // Obtener y formatear la dirección MAC (será el ID único coincidente con la web)
  deviceMac = WiFi.macAddress();
  deviceMac.replace(":", "");
  deviceMac.toUpperCase();
  Serial.print("ID del dispositivo (MAC): ");
  Serial.println(deviceMac);

  // Intentar cargar el Token
  deviceToken = loadTokenFromEEPROM();
  if (deviceToken.length() > 5) {
    isLinked = true;
    Serial.println("Token encontrado en memoria. Dispositivo Vinculado.");
  } else {
    Serial.println("No hay token. Dispositivo Desvinculado.");
  }

  // Cargar horarios guardados
  loadScheduleFromEEPROM();

  // 1. Cargar redes guardadas en Multi-WiFi
  loadAndRegisterMultiWiFi();

  // Intentamos conexión automática con Multi-WiFi si hay redes guardadas.
  // Si no está vinculado pero ya tiene WiFi, mostramos solo el PIN.
  bool connectedViaMulti = false;
  Serial.print("Buscando redes Multi-WiFi guardadas (timeout 8s)... ");
  uint8_t status = wifiMulti.run(8000);

  if (status == WL_CONNECTED || WiFi.status() == WL_CONNECTED) {
    connectedViaMulti = true;
    Serial.printf("\n¡Conectado exitosamente a la mejor red: %s!\n", WiFi.SSID().c_str());
    
    if (isLinked) {
      // ¡Apenas tenemos WiFi garantizado y la RAM limpia, ejecutamos la actualización OTA si está pendiente!
      executePendingOTA();
    }
  } else {
    Serial.println("\nNinguna red guardada al alcance.");
    if (isLinked) {
      Serial.println("Abriendo portal de configuración...");
      WiFi.disconnect();
      delay(100);
    }
  }

  if (!isLinked && connectedViaMulti) {
    Serial.println("Dispositivo con WiFi pero no vinculado. Abriendo portal solo para PIN...");
    int pinPortalResult = startPinOnlyPortal(pairingPin);
    if (pinPortalResult == 1) {
      Serial.printf("PIN válido ingresado por el usuario: %s\n", pairingPin.c_str());
    } else {
      Serial.println("Timeout en el portal de PIN. Reiniciando...");
      delay(3000);
      ESP.restart();
    }
  }

  // Si no se conectó por Multi-WiFi (o no está vinculado), lanzamos el portal
  if (!connectedViaMulti) {
    WiFiManager wm;
    wm.setTitle("PLANNTIX");
    wm.setCustomHeadElement(PLANNTIX_CUSTOM_HEAD);
    wm.setAPStaticIPConfig(IPAddress(10, 0, 1, 1), IPAddress(10, 0, 1, 1), IPAddress(255, 255, 255, 0));
    wm.setCaptivePortalEnable(true);
    wm.setConfigPortalTimeout(180); // 3 minutos de tiempo de espera

    std::vector<const char *> menu = {"wifi"};
    wm.setMenu(menu);

    WiFiManagerParameter custom_pin("pin", "PIN de Vinculaci&oacute;n (6 d&iacute;gitos)", "", 7, "placeholder='123456' maxlength='6' inputmode='numeric' pattern='[0-9]*'");
    WiFiManagerParameter custom_hint("<p style='font-size:12px;color:#bbcabf;text-align:center;margin:-6px 0 16px 0;'>Ingresa el c&oacute;digo generado en tu App PLANNTIX</p>");

    if (!isLinked) {
      wm.addParameter(&custom_pin);
      wm.addParameter(&custom_hint);
    }

    Serial.println("Escaneando redes Wi-Fi del entorno...");
    WiFi.scanNetworks(); // Escaneo síncrono para que el portal tenga la lista completa lista

    if (!isLinked) {
      Serial.println("Dispositivo no vinculado. Forzando portal cautivo para pedir el PIN...");
      if (!wm.startConfigPortal("PLANNTIX-Config")) {
        Serial.println("Timeout en el portal. Reiniciando...");
        delay(3000);
        ESP.restart();
      }
    } else {
      Serial.println("Abriendo portal para configurar red Wi-Fi...");
      if (!wm.startConfigPortal("PLANNTIX-Config")) {
        Serial.println("Timeout en el portal. Reiniciando placa...");
        delay(3000);
        ESP.restart();
      }
    }

    Serial.println("\n¡Wi-Fi Conectado exitosamente!");
    
    // Guardar la red en Multi-WiFi
    saveWiFiCredentials(WiFi.SSID(), WiFi.psk());

    if (!isLinked) {
      String providedPin = custom_pin.getValue();
      bool valid = isValidPairingPin(providedPin);

      if (valid) {
        pairingPin = providedPin;
        Serial.printf("PIN válido ingresado por el usuario: %s\n", pairingPin.c_str());
      } else {
        Serial.println("\n[ERROR CRÍTICO] PIN inválido. Debe contener exactamente 6 dígitos numéricos (0-9).");
        Serial.println("Rechazando conexión. Borrando red guardada y reiniciando portal cautivo...");
        wm.resetSettings(); // Borra credenciales para forzar el portal otra vez
        delay(3000);
        ESP.restart();
      }
    }
  }

  // Sincronizar hora para validación de tokens SSL/JWT y reloj interno
  Serial.print("Sincronizando hora con internet (NTP UTC-3)...");
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  int ntpTimeout = 0;
  while (now < 8 * 3600 * 2 && ntpTimeout < 30) { // Reducimos a 15 segundos de timeout
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    ntpTimeout++;
  }
  
  if (now < 8 * 3600 * 2) {
    Serial.println("\n[ADVERTENCIA] Timeout NTP. Intentando obtener hora del RTC DS3231...");
    // Intentar leer la hora del RTC
    // Nota: Aunque rtc.lostPower() haya sido verdadero, igual contendrá una hora aproximada o previamente guardada.
    DateTime rtcTime = rtc.now();
    time_t t = rtcTime.unixtime();
    if (t > 1700000000) { // Comprobar que no sea una fecha de error (ej: año 1970/2000 inicial)
      struct timeval tv = { t, 0 };
      settimeofday(&tv, nullptr);
      now = time(nullptr);
      Serial.printf("Hora cargada desde el RTC: %02d:%02d:%02d\n", rtcTime.hour(), rtcTime.minute(), rtcTime.second());
    } else {
      Serial.println("\n[ERROR CRÍTICO] RTC no disponible o sin hora válida y NTP falló. Reiniciando...");
      delay(3000);
      ESP.restart();
    }
  } else {
    Serial.println("\nHora sincronizada exitosamente por NTP.");
    // Sincronizar el RTC con la hora NTP recién obtenida
    rtc.adjust(DateTime(now));
    Serial.println("RTC actualizado con la hora de internet (NTP).");
  }

  // 2. Inicializar Firebase
  fbConfig.database_url = FIREBASE_HOST;
  fbConfig.api_key = FIREBASE_API_KEY;
  fbAuth.user.email = DEVICE_AUTH_EMAIL;
  fbAuth.user.password = DEVICE_AUTH_PASSWORD;
  
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase inicializado.");
  
  // Establecer estado inicial e info del firmware
  String presencePath = "/telemetry/" + deviceMac + "/status";
  Firebase.RTDB.setString(&fbData, presencePath, "online");
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/info/version", FIRMWARE_VERSION);
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/latest/version", FIRMWARE_VERSION);
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/info/board", BOARD_TYPE);

  float initHum = dht.readHumidity();
  float initTemp = dht.readTemperature();
  FirebaseJson initJson;
  initJson.add("temperature", isnan(initTemp) ? 22.0 : initTemp);
  initJson.add("humidity", isnan(initHum) ? 50.0 : initHum);
  initJson.add("isLightOn", digitalRead(RELAY_PIN) == HIGH);
  initJson.add("version", FIRMWARE_VERSION);
  initJson.add("timestamp", (int)time(nullptr));
  Firebase.RTDB.setJSON(&fbData, "/telemetry/" + deviceMac + "/latest", &initJson);

  if (otaBootErrorMessage.length() > 0) {
    Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "error: " + otaBootErrorMessage);
    otaBootErrorMessage = "";
  } else {
    Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "idle");
  }

  if (isLinked) {
    // 3. Validar si el token local sigue activo en Firebase
    String tokenPath = "/telemetry/" + deviceMac + "/config/secret_token";
    if (Firebase.RTDB.getString(&fbData, tokenPath)) {
      String remoteToken = fbData.stringData();
      if (remoteToken.length() < 5 || remoteToken != deviceToken) {
        Serial.println("\n[VALIDACIÓN] Token huérfano detectado: Esta placa no está registrada en Firebase.");
        Serial.println("Borrando memoria interna y reiniciando en modo portal cautivo...");
        delay(1000);
        factoryReset();
      } else {
        Serial.println("Token de vinculación validado exitosamente con Firebase.");
      }
    }

    if (Firebase.RTDB.beginStream(&streamData, "/telemetry/" + deviceMac + "/config/light")) {
      Serial.println("Stream configurado exitosamente!");
      Firebase.RTDB.setStreamCallback(&streamData, streamCallback, streamTimeoutCallback);
    } else {
      Serial.printf("Error al iniciar stream: %s\n", streamData.errorReason().c_str());
    }
  }
}

unsigned long lastOtaCheckTime = 0;

void loop() {
  unsigned long currentMillis = millis();

  // Mantener conexión Wi-Fi activa con Multi-WiFi si se pierde la señal
  if (WiFi.status() != WL_CONNECTED) {
    wifiMulti.run();
  }

  // 0. VERIFICAR COMANDOS DE ACTUALIZACIÓN CLOUD OTA (Cada 5 segundos)
  if (currentMillis - lastOtaCheckTime > 5000) {
    lastOtaCheckTime = currentMillis;
    String otaPath = "/telemetry/" + deviceMac + "/ota";
    if (Firebase.RTDB.getJSON(&fbData, otaPath)) {
      FirebaseJsonData jsonData;
      FirebaseJson &json = fbData.jsonObject();

      String otaStatus = "";
      json.get(jsonData, "status");
      if (jsonData.success) otaStatus = jsonData.stringValue;

      // Solo iniciamos si la app web envió una orden en estado 'pending'
      if (otaStatus == "pending") {
        json.get(jsonData, "url");
        if (jsonData.success && jsonData.stringValue.length() > 10) {
          String otaUrl = jsonData.stringValue;
          String otaVersion = "";
          String otaBoard = "";

          json.get(jsonData, "version");
          if (jsonData.success) otaVersion = jsonData.stringValue;

          json.get(jsonData, "board");
          if (jsonData.success) otaBoard = jsonData.stringValue;

          // Si la versión es distinta a la actual, ejecutamos la actualización
          if (otaVersion != "" && otaVersion != FIRMWARE_VERSION) {
            performCloudOTA(otaUrl, otaVersion, otaBoard);
          }
        }
      }
    }

    // 0.1 VERIFICAR ORDEN REMOTA DE DESVINCULACIÓN (Solo si está vinculado)
    if (isLinked) {
      String unlinkPath = "/telemetry/" + deviceMac + "/config/unlink";
      if (Firebase.RTDB.getBool(&fbData, unlinkPath)) {
        if (fbData.boolData() == true) {
          Serial.println("\n[DESVINCULACIÓN REMOTA] Orden recibida desde la App Web.");
          Serial.println("Borrando memoria y reiniciando en modo fábrica...");
          Firebase.RTDB.deleteNode(&fbData, "/telemetry/" + deviceMac);
          delay(500);
          factoryReset();
        }
      }
    }
  }

  // 1. LÓGICA DE RESET DE FÁBRICA
  if (digitalRead(RESET_PIN) == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      buttonPressStartTime = currentMillis;
    } else if (currentMillis - buttonPressStartTime > 5000) {
      factoryReset(); // Resetea si se aprieta > 5 seg
    }
  } else {
    isButtonPressed = false;
  }

  // 2. LÓGICA DE APROVISIONAMIENTO Y CONTROL (NON-BLOCKING)
  if (!isLinked) {
    static unsigned long unlinkedStartTime = 0;
    if (unlinkedStartTime == 0) unlinkedStartTime = currentMillis;

    // Timeout de 180 segundos (3 minutos) si el token no llega desde la web
    if (currentMillis - unlinkedStartTime > 180000) {
      Serial.println("\n[TIMEOUT VINCULACIÓN] No se recibió el token en 180 segundos.");
      Serial.println("Limpiando registro huérfano y reiniciando en portal cautivo...");
      if (pairingPin.length() == 6) {
        Firebase.RTDB.deleteNode(&fbData, "/unlinked_devices/" + pairingPin);
      }
      delay(1000);
      ESP.restart();
    }

    // Si no está vinculado y tenemos PIN, anunciar MAC en Firebase
    if (!pinUploaded && pairingPin.length() == 6) {
      FirebaseJson pinJson;
      pinJson.add("mac", deviceMac);
      pinJson.add("timestamp", (int)time(nullptr));
      String pinPath = "/unlinked_devices/" + pairingPin;
      
      if (Firebase.RTDB.setJSON(&fbData, pinPath, &pinJson)) {
        pinUploaded = true;
        Serial.println("MAC subida a Firebase bajo el PIN ingresado.");
      } else {
        Serial.printf("Error de Firebase al subir PIN: %s\n", fbData.errorReason().c_str());
      }
    }

    // Comprobar periódicamente si llegó el token desde la web (cada 5s)
    if (currentMillis - lastSensorReadTime > 5000) {
      lastSensorReadTime = currentMillis;
      String tokenPath = "/telemetry/" + deviceMac + "/config/secret_token";
      String receivedToken = "";
      
      Serial.println("Esperando token de vinculación de Firebase...");
      if (Firebase.RTDB.getString(&fbData, tokenPath)) {
        receivedToken = fbData.stringData();
        if (receivedToken.length() > 5) {
          if (receivedToken.length() >= MAX_TOKEN_LEN) {
            Serial.println("\n[ERROR CRÍTICO] Token recibido excede capacidad de EEPROM (Máx 63 bytes).");
            Serial.println("Rechazando credencial corrompida y borrando nodo de Firebase...");
            Firebase.RTDB.deleteNode(&fbData, tokenPath);
            delay(3000);
            ESP.restart();
          } else {
            deviceToken = receivedToken;
            saveTokenToEEPROM(deviceToken);
            isLinked = true;
            Serial.println("¡Dispositivo vinculado con éxito y token asegurado!");
            // Limpiar huérfano
            if (pairingPin.length() == 6) {
              Firebase.RTDB.deleteNode(&fbData, "/unlinked_devices/" + pairingPin);
            }
          }
        }
      }
    }
  } else {
    // ---- ACTUALIZACIÓN INSTANTÁNEA POR STREAM ----
    if (forceConfigUpdate) {
      forceConfigUpdate = false;
      String configPath = "/telemetry/" + deviceMac + "/config/light";
      if (Firebase.RTDB.getJSON(&fbData, configPath)) {
        FirebaseJsonData jsonData;
        FirebaseJson &json = fbData.jsonObject();
        
        bool changed = false;
        
        json.get(jsonData, "lightMode");
        if(jsonData.success && lightMode != jsonData.stringValue) { 
          lightMode = jsonData.stringValue; 
          changed = true; 
          isOverrideActive = false; // Reset override on mode change
        }
        
        // Pre-evaluate scheduled state
        bool scheduledState = false;
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        int currentTotalMins = timeinfo->tm_hour * 60 + timeinfo->tm_min;
        int onTotalMins = lightOnHour * 60 + lightOnMin;
        int offTotalMins = lightOffHour * 60 + lightOffMin;

        json.get(jsonData, "onTime");
        if(jsonData.success) {
          String onT = jsonData.stringValue;
          int h = onT.substring(0, 2).toInt();
          int m = onT.substring(3, 5).toInt();
          if (lightOnHour != h || lightOnMin != m) { 
            lightOnHour = h; 
            lightOnMin = m; 
            changed = true; 
            isOverrideActive = false; // Reset override on schedule change
          }
        }

        json.get(jsonData, "offTime");
        if(jsonData.success) {
          String offT = jsonData.stringValue;
          int h = offT.substring(0, 2).toInt();
          int m = offT.substring(3, 5).toInt();
          if (lightOffHour != h || lightOffMin != m) { 
            lightOffHour = h; 
            lightOffMin = m; 
            changed = true; 
            isOverrideActive = false; // Reset override on schedule change
          }
        }

        if (lightMode == "auto") {
          onTotalMins = lightOnHour * 60 + lightOnMin;
          offTotalMins = lightOffHour * 60 + lightOffMin;
          if (onTotalMins < offTotalMins) {
            scheduledState = (currentTotalMins >= onTotalMins && currentTotalMins < offTotalMins);
          } else {
            scheduledState = (currentTotalMins >= onTotalMins || currentTotalMins < offTotalMins);
          }
        }

        json.get(jsonData, "isOn");
        if(jsonData.success) {
          bool newIsOn = jsonData.boolValue;
          if (lightIsOn != newIsOn) {
            lightIsOn = newIsOn;
            
            if (lightMode == "auto") {
              // If user toggled switch and it differs from schedule, activate override
              if (newIsOn != scheduledState) {
                isOverrideActive = true;
                overrideStartTime = millis();
                overrideRelayState = newIsOn;
                Serial.printf("=> Override manual activado por 5 minutos: Relé -> %s\n", newIsOn ? "ON" : "OFF");
              } else {
                isOverrideActive = false;
              }
            }
          }
        }
        
        if (changed) saveScheduleToEEPROM();
      }
    }

    // ---- EVALUACIÓN CONTINUA DEL RELÉ ----
    bool targetRelayState = false;
    if (lightMode == "manual") {
      targetRelayState = lightIsOn;
    } else {
      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);
      int currentTotalMins = timeinfo->tm_hour * 60 + timeinfo->tm_min;
      int onTotalMins = lightOnHour * 60 + lightOnMin;
      int offTotalMins = lightOffHour * 60 + lightOffMin;
      
      bool scheduledState = false;
      if (onTotalMins < offTotalMins) {
        scheduledState = (currentTotalMins >= onTotalMins && currentTotalMins < offTotalMins);
      } else {
        scheduledState = (currentTotalMins >= onTotalMins || currentTotalMins < offTotalMins);
      }

      if (isOverrideActive) {
        // 5 minutos = 300000 ms. Para pruebas podés cambiarlo a 30000 ms (30 seg)
        if (millis() - overrideStartTime > 300000) {
          isOverrideActive = false;
          targetRelayState = scheduledState;
          lightIsOn = scheduledState;
          // Actualizar Firebase para sincronizar la web
          Firebase.RTDB.setBool(&fbData, "/telemetry/" + deviceMac + "/config/light/isOn", scheduledState);
          Serial.println("=> Override manual expirado (5m). Restableciendo ciclo automático.");
        } else {
          targetRelayState = overrideRelayState;
        }
      } else {
        targetRelayState = scheduledState;
      }
    }
    
    digitalWrite(RELAY_PIN, targetRelayState ? HIGH : LOW);
    bool currentRelayPhysicalState = (digitalRead(RELAY_PIN) == HIGH);
    
    static bool lastRelayState = !currentRelayPhysicalState;
    if (currentRelayPhysicalState != lastRelayState) {
      lastRelayState = currentRelayPhysicalState;
      Firebase.RTDB.setBool(&fbData, "/telemetry/" + deviceMac + "/latest/isLightOn", currentRelayPhysicalState);
      Serial.printf("=> Cambio detectado! Notificando a Web: Relé -> %s\n", currentRelayPhysicalState ? "ON" : "OFF");
    }

    // DISPOSITIVO VINCULADO: LEER SENSORES Y ACTUADORES (cada 10s)
    if (currentMillis - lastSensorReadTime > 10000) {
      lastSensorReadTime = currentMillis;

      // Lectura del Sensor
      float humedad = dht.readHumidity();
      float temperatura = dht.readTemperature();

      if (isnan(humedad) || isnan(temperatura)) {
        Serial.println("Error al leer el sensor DHT22. (NaN)");
        consecutiveSensorFailures++;
        if (consecutiveSensorFailures >= 10) {
          Serial.println("¡Demasiados fallos consecutivos del sensor! Reiniciando placa por seguridad...");
          delay(2000);
          ESP.restart();
        }
      } else {
        consecutiveSensorFailures = 0; // Se recuperó, reiniciamos contador
        sumHum += humedad;
        sumTemp += temperatura;
        readCount++;
      }

      // Cada 3 lecturas exitosas (aprox 30s) subimos el promedio
      if (readCount >= 3) {
        float avgHum = sumHum / 3.0;
        float avgTemp = sumTemp / 3.0;
        
        sumHum = 0;
        sumTemp = 0;
        readCount = 0;

        Serial.printf("Promedio 30s -> Humedad: %.1f%%  |  Temperatura: %.1f°C\n", avgHum, avgTemp);

        // Subida de datos de telemetría
        FirebaseJson json;
        json.add("temperature", avgTemp);
        json.add("humidity", avgHum);
        json.add("isLightOn", digitalRead(RELAY_PIN) == HIGH);
        json.add("timestamp", (int)time(nullptr));
        // NOTA: No enviamos el deviceToken en texto plano aquí para no exponerlo en reposo.
        
        String path = "/telemetry/" + deviceMac + "/latest";
        if (Firebase.RTDB.setJSON(&fbData, path, &json)) {
          Serial.println("¡Promedio subido de forma segura a Firebase!");
        } else {
          Serial.printf("Error al subir datos: %s\n", fbData.errorReason().c_str());
        }

        // Subida de historial (cada 15 min = 900000 ms)
        if (currentMillis - lastHistoryUploadTime > 900000 || lastHistoryUploadTime == 0) {
          lastHistoryUploadTime = currentMillis;
          String historyPath = "/telemetry/" + deviceMac + "/history";
          if (Firebase.RTDB.pushJSON(&fbData, historyPath, &json)) {
            Serial.println("¡Punto de historial subido a Firebase!");
          } else {
            Serial.printf("Error al subir historial: %s\n", fbData.errorReason().c_str());
          }
        }
      }
    }
  }
}
