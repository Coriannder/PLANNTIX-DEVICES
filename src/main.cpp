#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <EEPROM.h> 
#include <RTClib.h>
#include <sys/time.h>

// Identidad de Firmware
#define BOARD_TYPE "esp8266"
#define FIRMWARE_VERSION "1.0.0"

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

// Estilos y UI Chlorophyll Glass para WiFiManager
const char PLANNTIX_CUSTOM_HEAD[] = 
"<style>"
":root{--bg:#0c1324;--surface:#181f31;--surface-low:#141b2c;--border:#2e3447;--text:#dce2fa;--text-dim:#bbcabf;--primary:#4edea3;--primary-dark:#003824;--glow:rgba(78,222,163,0.25);}"
"*{box-sizing:border-box;}"
"html,body{background:radial-gradient(ellipse at top,#1c263d 0%,#0c1324 75%)!important;background-color:#0c1324!important;background-attachment:fixed!important;color:var(--text)!important;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif!important;margin:0;padding:20px;display:flex;justify-content:center;min-height:100vh;}"
"div,.wrap,.c,form{background:transparent!important;border:none!important;box-shadow:none!important;max-width:380px!important;width:100%!important;margin:0 auto!important;}"
"h1{color:#4edea3!important;font-size:24px!important;font-weight:800!important;letter-spacing:1.5px!important;text-align:center!important;margin:10px 0 2px 0!important;display:flex;align-items:center;justify-content:center;gap:8px;background:transparent!important;}"
"h1::before{content:'🌱';font-size:22px;}"
"h3{color:var(--text-dim)!important;font-size:13px!important;font-weight:400!important;text-align:center!important;margin:0 0 16px 0!important;background:transparent!important;}"
"label{display:block!important;color:var(--text-dim)!important;font-size:13px!important;margin-bottom:6px!important;text-align:left!important;}"
"input,select{width:100%!important;background-color:var(--surface-low)!important;border:1px solid var(--border)!important;color:var(--text)!important;padding:12px 14px!important;border-radius:10px!important;font-size:14px!important;outline:none!important;box-shadow:none!important;margin-bottom:14px!important;}"
"select{-webkit-appearance:none!important;appearance:none!important;background-image:url(\"data:image/svg+xml;charset=UTF-8,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='%234edea3'%3e%3cpath d='M7 10l5 5 5-5z'/%3e%3c/svg%3e\")!important;background-repeat:no-repeat!important;background-position:right 12px center!important;background-size:20px!important;cursor:pointer!important;}"
"select:focus,input:focus{border-color:var(--primary)!important;box-shadow:0 0 10px var(--glow)!important;}"
"option{background-color:#141b2c!important;color:#dce2fa!important;padding:8px!important;}"
"input#pin{text-align:center!important;letter-spacing:6px!important;font-size:20px!important;font-weight:700!important;border-color:rgba(78,222,163,0.5)!important;background-color:var(--surface)!important;color:var(--primary)!important;}"
"button,input[type='submit']{width:100%!important;background-color:var(--primary)!important;color:var(--primary-dark)!important;border:none!important;padding:13px!important;border-radius:10px!important;font-size:15px!important;font-weight:700!important;cursor:pointer!important;margin-top:10px!important;box-shadow:0 4px 12px var(--glow)!important;}"
"button:hover,input[type='submit']:hover{filter:brightness(1.08);}"
"</style>"
"<script>"
"if(location.pathname==='/'||location.pathname===''){location.replace('/wifi');}"
"</script>";

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
const int EEPROM_WIFI_COUNT_ADDR = 99;
const int EEPROM_WIFI_START_ADDR = 100;

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
    if (strlen(net.ssid) > 0) {
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
  if (targetBoard != "" && targetBoard != BOARD_TYPE && targetBoard != "ESP8266_NODEMCU") {
    Serial.printf("[OTA] Rechazado: La actualización es para placa '%s', este dispositivo es '%s'\n", targetBoard.c_str(), BOARD_TYPE);
    Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "idle");
    return;
  }

  if (targetVersion == FIRMWARE_VERSION) {
    Serial.printf("[OTA] El dispositivo ya está en la versión objetivo (%s). Omitiendo.\n", FIRMWARE_VERSION);
    Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "idle");
    return;
  }

  Serial.printf("\n[OTA] Iniciando actualización a v%s desde:\n%s\n", targetVersion.c_str(), url.c_str());

  // Notificar a Firebase el cambio de estado
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/status", "updating");
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "downloading");

  // Apagar relé por seguridad durante la actualización
  digitalWrite(RELAY_PIN, LOW);

  // Liberar memoria RAM de Firebase para que el cliente SSL tenga espacio suficiente
  fbData.clear();
  streamData.clear();

  WiFi.setSleepMode(WIFI_NONE_SLEEP); // Máxima velocidad WiFi para la descarga
  ESPhttpUpdate.setClientTimeout(30000);
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret;

  if (url.startsWith("https://")) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    secureClient.setBufferSizes(4096, 512);
    secureClient.setTimeout(30000);
    ret = ESPhttpUpdate.update(secureClient, url);
  } else {
    WiFiClient plainClient;
    plainClient.setTimeout(30000);
    ret = ESPhttpUpdate.update(plainClient, url);
  }

  switch (ret) {
    case HTTP_UPDATE_FAILED: {
      String errorMsg = ESPhttpUpdate.getLastErrorString();
      int errorCode = ESPhttpUpdate.getLastError();
      Serial.printf("[OTA ERROR] Falló la actualización (%d): %s\n", errorCode, errorMsg.c_str());
      
      // Notificar error en Firebase y limpiar estado pendiente para evitar bucles
      Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/status", "online");
      Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "error: " + errorMsg);
      break;
    }
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] No hay actualizaciones disponibles.");
      Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/status", "online");
      Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/ota/status", "idle");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] ¡Actualización exitosa! Reiniciando...");
      break;
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
  
  EEPROM.begin(512);
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

  // Obtener y formatear la dirección MAC (será el ID único)
  deviceMac = WiFi.macAddress();
  deviceMac.replace(":", "_");
  deviceMac.toLowerCase();
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

  // Si ya está vinculado, intentamos conexión automática con Multi-WiFi
  bool connectedViaMulti = false;
  if (isLinked) {
    Serial.print("Buscando redes Multi-WiFi guardadas (timeout 5s)... ");
    uint8_t status = wifiMulti.run(5000);
    if (status == WL_CONNECTED) {
      connectedViaMulti = true;
      Serial.printf("\n¡Conectado exitosamente a la mejor red: %s!\n", WiFi.SSID().c_str());
    } else {
      Serial.println("\nNinguna red guardada al alcance. Abriendo portal de configuración...");
      WiFi.disconnect();
      delay(100);
    }
  }

  // Si no se conectó por Multi-WiFi (o no está vinculado), lanzamos el portal
  if (!connectedViaMulti) {
    WiFiManager wm;
    wm.setTitle("PLANNTIX");
    wm.setClass("invert");
    wm.setCustomHeadElement(PLANNTIX_CUSTOM_HEAD);
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
      bool valid = (providedPin.length() == 6);
      for (unsigned int i = 0; i < providedPin.length(); i++) {
        if (!isdigit(providedPin[i])) valid = false;
      }

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
  fbConfig.signer.test_mode = true;
  
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase inicializado.");
  
  // Establecer estado inicial e info del firmware
  String presencePath = "/telemetry/" + deviceMac + "/status";
  Firebase.RTDB.setString(&fbData, presencePath, "online");
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/info/version", FIRMWARE_VERSION);
  Firebase.RTDB.setString(&fbData, "/telemetry/" + deviceMac + "/info/board", BOARD_TYPE);
  Firebase.RTDB.setBool(&fbData, "/telemetry/" + deviceMac + "/latest/isLightOn", digitalRead(RELAY_PIN) == HIGH);

  if (isLinked) {
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

  // 0. VERIFICAR COMANDOS DE ACTUALIZACIÓN CLOUD OTA (Cada 15 segundos)
  if (currentMillis - lastOtaCheckTime > 15000) {
    lastOtaCheckTime = currentMillis;
    String otaPath = "/telemetry/" + deviceMac + "/ota";
    if (Firebase.RTDB.getJSON(&fbData, otaPath)) {
      FirebaseJsonData jsonData;
      FirebaseJson &json = fbData.jsonObject();

      String otaStatus = "";
      json.get(jsonData, "status");
      if (jsonData.success) otaStatus = jsonData.stringValue;

      // Solo proceder si hay una orden pendiente de ejecución
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

          if (otaVersion != "" && otaVersion != FIRMWARE_VERSION) {
            performCloudOTA(otaUrl, otaVersion, otaBoard);
          }
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
        json.add("version", FIRMWARE_VERSION);
        json.add("board", BOARD_TYPE);
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
