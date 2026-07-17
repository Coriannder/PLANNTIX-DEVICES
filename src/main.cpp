#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <DHT.h>
#include <FirebaseESP8266.h>

#define DHTPIN D7       // Pin de datos del DHT22
#define DHTTYPE DHT22   // Tipo de sensor

DHT dht(DHTPIN, DHTTYPE);

// Configuración de Firebase
#define FIREBASE_HOST "plantix-9c6a4-default-rtdb.firebaseio.com"

FirebaseData fbData;
FirebaseConfig fbConfig;
FirebaseAuth fbAuth;

String deviceMac = "";

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Iniciando PLANNTIX-DEVICES ---");
  
  dht.begin();

  // 1. Configurar WiFiManager (Portal Cautivo)
  WiFiManager wm;
  Serial.println("Conectando al Wi-Fi o abriendo portal cautivo...");
  
  // Intenta conectarse al Wi-Fi guardado. Si falla, genera la red "PLANNTIX-Config"
  bool connected = wm.autoConnect("PLANNTIX-Config");

  if (!connected) {
    Serial.println("Fallo al conectar al Wi-Fi. Reiniciando placa...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("\n¡Wi-Fi Conectado exitosamente!");
  
  // Obtener y formatear la dirección MAC (será el ID único del dispositivo)
  deviceMac = WiFi.macAddress();
  deviceMac.replace(":", "_"); // Reemplazar dos puntos para evitar problemas en rutas de Firebase
  Serial.print("ID del dispositivo (MAC): ");
  Serial.println(deviceMac);

  // 2. Inicializar Firebase
  fbConfig.database_url = FIREBASE_HOST;
  // Si tu base de datos tiene reglas restrictivas, podrías requerir un token o secret
  // fbConfig.signer.tokens.legacy_token = "TU_DATABASE_SECRET"; 
  
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase inicializado.");
}

void loop() {
  // Leer el sensor cada 5 segundos
  delay(5000);

  float humedad = dht.readHumidity();
  float temperatura = dht.readTemperature();

  // Validar lecturas
  if (isnan(humedad) || isnan(temperatura)) {
    Serial.println("Error al leer el sensor DHT22. ¡Revisá las conexiones!");
    return;
  }

  // Mostrar en consola local
  Serial.print("Humedad: ");
  Serial.print(humedad);
  Serial.print("%  |  Temperatura: ");
  Serial.print(temperatura);
  Serial.println("°C");

  // 3. Crear JSON y enviar a Firebase Realtime Database
  FirebaseJson json;
  json.add("temperature", temperatura);
  json.add("humidity", humedad);
  
  // Ruta en Firebase según convención de PLANNTIX: /telemetry/[MAC]/latest
  String path = "/telemetry/" + deviceMac + "/latest";
  
  Serial.print("Enviando datos a Firebase en ");
  Serial.println(path);

  if (Firebase.setJSON(fbData, path, json)) {
    Serial.println("¡Datos subidos con éxito!");
  } else {
    Serial.print("Error al subir datos: ");
    Serial.println(fbData.errorReason());
  }
}
