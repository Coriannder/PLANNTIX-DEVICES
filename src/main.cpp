#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <EEPROM.h>

#define DHTPIN D7       // Pin de datos del DHT22
#define DHTTYPE DHT22   // Tipo de sensor

#define RESET_PIN 0     // Botón Flash del NodeMCU (GPIO 0 / D3)
#define RELAY_PIN D1    // Pin para controlar actuadores (Ej: Ventilador o Luz)

DHT dht(DHTPIN, DHTTYPE);

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

// Schedule Settings
bool lightIsOn = false;         
String lightMode = "manual";    
int lightOnHour = 6;
int lightOnMin = 0;
int lightOffHour = 18;
int lightOffMin = 0;

// Variables para el sobremuestreo del sensor
int consecutiveSensorFailures = 0;
float sumTemp = 0;
float sumHum = 0;
int readCount = 0;

const int MAX_TOKEN_LEN = 64;

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

  // 1. Configurar WiFiManager (Portal Cautivo)
  WiFiManager wm;
  wm.setAPStaticIPConfig(IPAddress(10, 0, 1, 1), IPAddress(10, 0, 1, 1), IPAddress(255, 255, 255, 0));
  wm.setCaptivePortalEnable(true);
  wm.setConfigPortalTimeout(180); // 3 minutos de tiempo de espera
  
  // Agregar campo personalizado para el PIN
  WiFiManagerParameter custom_pin("pin", "C&oacute;digo de Vinculaci&oacute;n (6 d&iacute;gitos)", "", 7);
  wm.addParameter(&custom_pin);

  if (!isLinked) {
    Serial.println("Dispositivo no vinculado. Forzando portal cautivo para pedir el PIN...");
    if (!wm.startConfigPortal("PLANNTIX-Config")) {
      Serial.println("Timeout en el portal. Reiniciando...");
      delay(3000);
      ESP.restart();
    }
  } else {
    Serial.println("Conectando al Wi-Fi guardado...");
    if (!wm.autoConnect("PLANNTIX-Config")) {
      Serial.println("Fallo al conectar al Wi-Fi. Reiniciando placa...");
      delay(3000);
      ESP.restart();
    }
  }

  Serial.println("\n¡Wi-Fi Conectado exitosamente!");

  String providedPin = custom_pin.getValue();
  if (!isLinked) {
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

  // Sincronizar hora para validación de tokens SSL/JWT y reloj interno
  Serial.print("Sincronizando hora con internet (NTP UTC-3)...");
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  int ntpTimeout = 0;
  while (now < 8 * 3600 * 2 && ntpTimeout < 60) { // 60 iteraciones x 500ms = 30 segundos
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    ntpTimeout++;
  }
  
  if (now < 8 * 3600 * 2) {
    Serial.println("\n[ERROR CRÍTICO] Timeout NTP (30s). No se pudo sincronizar la hora.");
    Serial.println("Posible bloqueo del puerto 123 (NTP) en la red local o requiere portal cautivo de hotel/red pública.");
    Serial.println("Reiniciando placa para reintentar en 3 segundos...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("\nHora sincronizada exitosamente.");

  // 2. Inicializar Firebase
  fbConfig.database_url = FIREBASE_HOST;
  fbConfig.signer.test_mode = true;
  
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase inicializado.");
  
  // Establecer estado inicial
  String presencePath = "/telemetry/" + deviceMac + "/status";
  if (!Firebase.RTDB.setString(&fbData, presencePath, "online")) {
    Serial.printf("Error de Firebase (Status): %s\n", fbData.errorReason().c_str());
  }

  if (isLinked) {
    if (Firebase.RTDB.beginStream(&streamData, "/telemetry/" + deviceMac + "/config/light")) {
      Serial.println("Stream configurado exitosamente!");
      Firebase.RTDB.setStreamCallback(&streamData, streamCallback, streamTimeoutCallback);
    } else {
      Serial.printf("Error al iniciar stream: %s\n", streamData.errorReason().c_str());
    }
  }
}

void loop() {
  unsigned long currentMillis = millis();

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
        if(jsonData.success && lightMode != jsonData.stringValue) { lightMode = jsonData.stringValue; changed = true; }
        
        json.get(jsonData, "isOn");
        if(jsonData.success && lightIsOn != jsonData.boolValue) { lightIsOn = jsonData.boolValue; }

        json.get(jsonData, "onTime");
        if(jsonData.success) {
          String onT = jsonData.stringValue;
          int h = onT.substring(0, 2).toInt();
          int m = onT.substring(3, 5).toInt();
          if (lightOnHour != h || lightOnMin != m) { lightOnHour = h; lightOnMin = m; changed = true; }
        }

        json.get(jsonData, "offTime");
        if(jsonData.success) {
          String offT = jsonData.stringValue;
          int h = offT.substring(0, 2).toInt();
          int m = offT.substring(3, 5).toInt();
          if (lightOffHour != h || lightOffMin != m) { lightOffHour = h; lightOffMin = m; changed = true; }
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
      
      if (onTotalMins < offTotalMins) {
        targetRelayState = (currentTotalMins >= onTotalMins && currentTotalMins < offTotalMins);
      } else {
        targetRelayState = (currentTotalMins >= onTotalMins || currentTotalMins < offTotalMins);
      }
    }
    
    static bool lastRelayState = !targetRelayState;
    digitalWrite(RELAY_PIN, targetRelayState ? HIGH : LOW);
    
    if (targetRelayState != lastRelayState) {
      lastRelayState = targetRelayState;
      Firebase.RTDB.setBool(&fbData, "/telemetry/" + deviceMac + "/latest/isLightOn", targetRelayState);
      Serial.printf("=> Cambio detectado! Notificando a Web: Relé -> %s\n", targetRelayState ? "ON" : "OFF");
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
        json.add("isLightOn", targetRelayState);
        json.add("humidity", avgHum);
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
