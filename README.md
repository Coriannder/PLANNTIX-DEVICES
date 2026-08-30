# PLANNTIX-DEVICES

Firmware para dispositivos ESP8266 (NodeMCU v2) del ecosistema PLANNTIX: monitoreo de clima (temperatura/humedad), control de relé/iluminación, sincronización horaria resiliente (NTP + RTC DS3231) y actualizaciones remotas (Cloud OTA).

---

## 📌 Pinout y Esquema de Conexión

### Tabla de Conexiones

| Componente | Pin Componente | Pin NodeMCU | GPIO ESP8266 | Función / Protocolo |
| :--- | :--- | :--- | :--- | :--- |
| **RTC DS3231** | `SCL` | `D1` | `GPIO 5` | Reloj I2C (Clock) |
| **RTC DS3231** | `SDA` | `D2` | `GPIO 4` | Datos I2C (Data) |
| **RTC DS3231** | `VCC` | `3V3` o `VIN` | - | Alimentación (3.3V o 5V) |
| **RTC DS3231** | `GND` | `GND` | - | Tierra |
| **Módulo Relé** | `IN` / `Signal`| `D5` | `GPIO 14` | Control del Relé (Luz/Ventilación) |
| **Módulo Relé** | `VCC` | `VIN` (5V) / `3V3`| - | Alimentación de la bobina |
| **Módulo Relé** | `GND` | `GND` | - | Tierra |
| **Sensor DHT22**| `DATA` / `OUT` | `D7` | `GPIO 13` | Lectura Temperatura/Humedad |
| **Sensor DHT22**| `VCC` | `3V3` | - | Alimentación |
| **Sensor DHT22**| `GND` | `GND` | - | Tierra |
| **Botón Reset** | Integrado | `D3` (FLASH) | `GPIO 0` | Reseteo de fábrica (WiFi/Vinculación) |

---

### Diagrama de Conexión

```text
               +----------------------------------+
               |        NodeMCU v2 (ESP8266)      |
               |                                  |
               | [3V3] [GND] [D1] [D2]  [D5] [D7] |
               +---|-----|----|----|-----|----|---+
                   |     |    |    |     |    |
   +---------------+     |    |    |     |    |
   |   +-----------------+    |    |     |    |
   |   |                      |    |     |    |
+--+---+--------+             |    |     |    |
|   DS3231 RTC  |             |    |     |    |
| VCC  GND  SCL |-------------+    |     |    |
|           SDA |------------------+     |    |
+---------------+                        |    |
                                         |    |
+---------------+                        |    |
|  MÓDULO RELÉ  |                        |    |
| IN / SIGNAL   |------------------------+    |
| VCC (5V/VIN)  |                             |
| GND           |                             |
+---------------+                             |
                                              |
+---------------+                             |
| SENSOR DHT22  |                             |
| DATA / OUT    |-----------------------------+
| VCC (3.3V)    |
| GND           |
+---------------+
```

---

## ☁️ Cloud OTA (Actualizaciones Remotas vía WiFi)

El firmware verifica periódicamente (cada 15 segundos) cambios en el nodo `/telemetry/{MAC}/ota` de Firebase Realtime Database.

### Estructura en Firebase para disparar OTA

```json
{
  "telemetry": {
    "ESP_MAC_ADDRESS": {
      "ota": {
        "url": "https://github.com/usuario/repo/releases/download/v1.0.1/firmware.bin",
        "version": "1.0.1",
        "board": "esp8266"
      }
    }
  }
}
```

### Estados reportados por el dispositivo:
- `/telemetry/{MAC}/status`: `"online"` | `"updating"`
- `/telemetry/{MAC}/ota/status`: `"downloading"` | `"error: <motivo>"`

---

## ⏱️ Sincronización Horaria (NTP + RTC DS3231)

1. **Al iniciar:** Intenta sincronizar por internet (NTP).
2. **Si hay conexión:** Actualiza la hora del sistema y guarda la hora exacta en el RTC DS3231.
3. **Si falla internet / timeout NTP:** Carga la hora directamente desde el RTC para mantener los ciclos de luz funcionando sin interrupciones.
