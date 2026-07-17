#include <Arduino.h>

void setup() {
  // Inicialización del puerto serie
  Serial.begin(115200);
  
  // Configurar el pin del LED integrado como salida
  pinMode(LED_BUILTIN, OUTPUT);
  
  Serial.println("¡Dispositivo ESP8266 Iniciado con éxito!");
}

void loop() {
  // Parpadeo del LED integrado (activo en LOW en ESP8266)
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("LED Encendido");
  delay(1000);
  
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("LED Apagado");
  delay(1000);
}
