#include <Arduino.h> 

#include <Wire.h> 

#include <SPI.h> 

#include <RadioLib.h> 

 

const int SENSOR_PIN = 6; 

int valorAire = 2414; 

int valorAgua = 1195; 

 

const int PIN_NSS = 23; 

const int PIN_BUSY = 19; 

const int PIN_DIO1 = 15; 

const int PIN_RST = RADIOLIB_NC; 

 

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY); 

LoRaWANNode node(&radio, &EU868); 

 

uint64_t joinEUI = 0x0000000000000000; 

uint64_t devEUI = 0x336B678922AAEFCC; 

uint8_t appKey[] = { 0x3C, 0xFE, 0xC1, 0xAE, 0x6A, 0x10, 0x40, 0xC7, 0xD8, 0x1F, 0x88, 0xE6, 0x84, 0xB1, 0x0C, 0xB6 }; 

uint8_t nwkKey[] = { 0x3C, 0xFE, 0xC1, 0xAE, 0x6A, 0x10, 0x40, 0xC7, 0xD8, 0x1F, 0x88, 0xE6, 0x84, 0xB1, 0x0C, 0xB6 }; 

 

void escribirRegistro(uint8_t devAddr, uint8_t reg, uint8_t val) { 

Wire.beginTransmission(devAddr); 

Wire.write(reg); 

Wire.write(val); 

Wire.endTransmission(); 

} 

 

void forzarAlimentacionRF() { 

Wire.begin(); 

Wire.setTimeOut(100); 

 

uint8_t mask = (1 << 5) | (1 << 6) | (1 << 7); 

uint8_t expansores[] = {0x43, 0x44}; 

for (int i = 0; i < 2; i++) { 

uint8_t addr = expansores[i]; 

escribirRegistro(addr, 0x03, (uint8_t)~mask); 

escribirRegistro(addr, 0x07, 0x00); 

escribirRegistro(addr, 0x01, mask); 

} 

delay(50); 

} 

 

void setup() { 

Serial.begin(115200); 

delay(1500); 

 

analogReadResolution(12); 

pinMode(SENSOR_PIN, INPUT); 

 

Serial.println("\n=========================================="); 

Serial.println(" Arduino Nesso N1 - LoRaWAN Enlace "); 

Serial.println("=========================================="); 

 

forzarAlimentacionRF(); 

SPI.begin(); 

 

int state = radio.begin(); 

if (state != RADIOLIB_ERR_NONE) { 

Serial.print("Error SX1262: "); 

Serial.println(state); 

while (true); 

} 

 

radio.setDio2AsRfSwitch(true); 

 

// Inicializar OTAA 

node.beginOTAA(joinEUI, devEUI, nwkKey, appKey); 

 

Serial.println("Uniendo a ChirpStack..."); 

state = node.activateOTAA(); 

while (state != RADIOLIB_ERR_NONE && state != RADIOLIB_LORAWAN_MODE_OTAA) { 

Serial.print("Fallo en Join (Código: "); 

Serial.print(state); 

Serial.println("). Reintentando en 6s..."); 

delay(6000); 

state = node.activateOTAA(); 

} 

 

Serial.println("\n>>> ¡NODO CONECTADO! SESIÓN ESTABLECIDA <<<"); 

} 

 

void loop() { 

// 1. Muestreo sensor de humedad 

long suma = 0; 

for (int i = 0; i < 25; i++) { 

suma += analogRead(SENSOR_PIN); 

delay(10); 

} 

int valorCrudo = suma / 25; 

 

float humedad = map(valorCrudo, valorAire, valorAgua, 0, 100); 

humedad = constrain(humedad, 0.0, 100.0); 

 

Serial.print("Lectura Cruda: "); 

Serial.print(valorCrudo); 

Serial.print(" | Humedad: "); 

Serial.print(humedad, 1); 

Serial.println(" %"); 

 

// 2. Empaquetar a 2 bytes (uint16_t) 

uint16_t humedadInt = (uint16_t)(humedad * 100); 

uint8_t payload[2]; 

payload[0] = (humedadInt >> 8) & 0xFF; 

payload[1] = humedadInt & 0xFF; 

 

// 3. Enviar uplink (FPort 1) 

Serial.println("Enviando paquete a ChirpStack..."); 

int state = node.sendReceive(payload, sizeof(payload), 1); 

 

if (state == RADIOLIB_ERR_NONE) { 

Serial.println("Uplink enviado con éxito."); 

} else { 

Serial.print("Estado de envío: "); 

Serial.println(state); 

} 

 

delay(60000); // Enviar cada minuto 

} 
