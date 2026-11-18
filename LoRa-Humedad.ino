#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <Arduino_Nesso_N1.h>
#include <esp_sleep.h>

// ---------------- Intervalo de envío ----------------
const uint32_t INTERVALO_MS = 60UL * 1000UL;   // 1 minuto

// ---------------- Ahorro de energía ----------------
// 0 = espera con delay(100) (el monitor serie y la subida de código funcionan bien)
// 1 = light sleep entre comprobaciones del botón (consume mucho menos, pero el puerto USB se corta mientras duerme)
#define USAR_LIGHT_SLEEP 0

// 1 = imprime por serie el estado del botón cada segundo (para depurar). Ponlo a 0 cuando funcione.
#define DEPURAR_BOTON 1

// ---------------- Pantalla: apagada salvo al medir/enviar y al pulsar el botón amarillo (KEY1) ----------------
const uint32_t PANTALLA_MS = 10000;   // tiempo encendida tras medir/enviar o tras pulsar
const uint8_t  BRILLO_ON   = 128;     // 0-255
bool pantallaOn = false;
uint32_t tPantalla = 0;

// Último dato conocido (lo que se enseña en pantalla)
float ultimaHumedad = 0;
const char* ultimoEstado = "Sin datos";
uint16_t ultimoColor = TFT_YELLOW;

// ---------------- Sensor de humedad ----------------
const int SENSOR_PIN = 6;      // GPIO6 (HAT D3, ADC) - señal del sensor
const int SENSOR_PWR_PIN = 2;  // GPIO2 (HAT D2, pin 5 del HAT) - alimenta el sensor (cable rojo aquí, ya NO en 3V3)
const uint32_t SENSOR_WARMUP_MS = 200;   // tiempo de estabilización del sensor antes de medir
int valorAire = 2414;          // recalibra si cambias la alimentación del sensor
int valorAgua = 1195;

// ---------------- SX1262 ----------------
const int PIN_NSS  = 23;
const int PIN_BUSY = 19;
const int PIN_DIO1 = 15;

// El NRST NO es un GPIO del ESP32: está en el expansor I2C (P7), se gestiona a mano
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, RADIOLIB_NC, PIN_BUSY);
LoRaWANNode node(&radio, &EU868);

Preferences store;   // memoria NVS para guardar la persistencia LoRaWAN entre reinicios
NessoBattery battery;

// ---------------- Persistencia LoRaWAN (RadioLib 8.x) ----------------
// RadioLib 8 usa callbacks: la librería nos da el buffer y nosotros lo guardamos/leemos en NVS.
// Esto conserva los DevNonce usados (ChirpStack rechaza repetidos) y la sesión.
void guardarPersistencia(uint8_t* buf, size_t len) {
  store.putBytes("persist", buf, len);
}

void restaurarPersistencia(uint8_t* buf, size_t len) {
  if (store.isKey("persist") && store.getBytesLength("persist") == len) {
    store.getBytes("persist", buf, len);
  }
}

uint64_t joinEUI = 0x0000000000000000;
uint64_t devEUI  = 0xAD48DBF9D02C09E7;
uint8_t appKey[] = { 0x9A, 0x4D, 0x89, 0x1E, 0xBB, 0x99, 0x80, 0xBB, 0x34, 0x75, 0x03, 0xD9, 0x33, 0xC9, 0xD4, 0x48 };
uint8_t nwkKey[] = { 0x9A, 0x4D, 0x89, 0x1E, 0xBB, 0x99, 0x80, 0xBB, 0x34, 0x75, 0x03, 0xD9, 0x33, 0xC9, 0xD4, 0x48 };

// ---------------- Expansor PI4IOE5V6408 (E0, 0x43) ----------------
const uint8_t EXP_ADDR     = 0x43;
const uint8_t REG_DIR      = 0x03;  // 1 = salida
const uint8_t REG_OUT      = 0x05;  // nivel de salida
const uint8_t REG_HIZ      = 0x07;  // 1 = alta impedancia (0 = push-pull activo)
const uint8_t REG_IN       = 0x0F;  // estado de las entradas
const uint8_t KEY1_BIT     = 0;     // P0 = KEY1 (botón amarillo)
const uint8_t LORA_LNA_EN  = 5;     // P5
const uint8_t LORA_ANT_SW  = 6;     // P6
const uint8_t LORA_RST     = 7;     // P7

uint8_t leerReg(uint8_t reg) {
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(EXP_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void escribirReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Modifica solo un bit (no pisa botones, etc.)
void setBit(uint8_t reg, uint8_t bit, bool v) {
  uint8_t r = leerReg(reg);
  if (v) r |= (1 << bit); else r &= ~(1 << bit);
  escribirReg(reg, r);
}

void expansorSalida(uint8_t bit, bool nivel) {
  setBit(REG_OUT, bit, nivel);   // primero el nivel...
  setBit(REG_HIZ, bit, false);   // ...quitar alta impedancia...
  setBit(REG_DIR, bit, true);    // ...y luego pasarlo a salida
}

// Botón amarillo (KEY1): pulsado = nivel bajo. Si te funciona al revés, quita el "!"
bool botonAmarillo() {
  return !(leerReg(REG_IN) & (1 << KEY1_BIT));
}

void iniciarControlLoRa() {
  Wire.begin();  // asegura el bus I2C para hablar con el expansor
  // LNA y conmutador de antena activos
  expansorSalida(LORA_LNA_EN, true);
  expansorSalida(LORA_ANT_SW, true);

  // Reset del SX1262: bajo 100 ms, alto 100 ms
  expansorSalida(LORA_RST, false);
  delay(100);
  setBit(REG_OUT, LORA_RST, true);
  delay(100);
}

// ---------------- Ahorro de energía del módulo LoRa ----------------
// Duerme el SX1262 (warm start: conserva la configuración) y apaga LNA y switch de antena
void dormirLoRa() {
  radio.sleep();                         // SX1262 -> modo sleep
  setBit(REG_OUT, LORA_LNA_EN, false);   // LNA apagado
  setBit(REG_OUT, LORA_ANT_SW, false);   // switch de antena apagado
  Serial.println("LoRa dormido.");
}

// Despierta: reactiva LNA y switch. El SX1262 sale de sleep solo al recibir
// la siguiente transacción SPI (NSS a nivel bajo), que hace RadioLib al transmitir.
void despertarLoRa() {
  setBit(REG_OUT, LORA_LNA_EN, true);
  setBit(REG_OUT, LORA_ANT_SW, true);
  delay(5);                              // pequeño margen de estabilización
}

// ---------------- Pantalla ----------------
NessoDisplay display;   // la libreria no tiene un objeto global "Nesso": se crea asi

// Despierta el controlador y enciende la luz (úsala antes de dibujar mensajes sueltos)
void pantallaEncender() {
  display.wakeup();
  display.setBrightness(BRILLO_ON);
  digitalWrite(LCD_BACKLIGHT, HIGH);   // luz encendida
  pantallaOn = true;
  tPantalla = millis();
}

// Apaga la luz y manda el controlador a dormir
void pantallaApagar() {
  display.setBrightness(0);
  display.sleep();
  digitalWrite(LCD_BACKLIGHT, LOW);    // luz apagada
  pantallaOn = false;
}

// Dibuja la información actual: humedad, estado del último envío y batería (arriba a la derecha)
void pintarInfo() {
  int nivel = battery.getChargeLevel();   // 0-100 %
  uint16_t colorBat = (nivel > 50) ? TFT_GREEN : (nivel > 20 ? TFT_YELLOW : TFT_RED);

  display.fillScreen(TFT_BLACK);

  // Cabecera: título a la izquierda, batería a la derecha
  display.setTextDatum(top_left);
  display.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
  display.setTextSize(1);
  display.drawString("NESSO N1 - SENSOR", 15, 10);

  display.setTextDatum(top_right);
  display.setTextColor(colorBat, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("BAT " + String(nivel) + "%", 230, 6);

  display.setTextDatum(top_left);
  display.drawFastHLine(10, 28, 220, TFT_DARKGREY);

  // Humedad
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setTextSize(4);
  display.drawString(String(ultimaHumedad, 1) + "%", 25, 50);

  // Estado del último envío
  display.setTextColor(ultimoColor, TFT_BLACK);
  display.setTextSize(2);
  display.drawString(ultimoEstado, 15, 105);
}

// Muestra la info actual y mantiene la pantalla encendida PANTALLA_MS.
// Si estaba apagada: despierta, dibuja y solo entonces enciende la luz (sin parpadeos).
void pantallaMostrar() {
  if (!pantallaOn) display.wakeup();
  pintarInfo();
  display.setBrightness(BRILLO_ON);
  digitalWrite(LCD_BACKLIGHT, HIGH);
  pantallaOn = true;
  tPantalla = millis();
}

void mensajePantalla(const char* txt, uint16_t color) {
  display.setTextDatum(top_left);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(color, TFT_BLACK);
  display.setTextSize(2);
  display.drawString(txt, 15, 50);
}

// Enciende la pantalla al pulsar el botón y la apaga pasado PANTALLA_MS
void gestionarPantalla() {
#if DEPURAR_BOTON
  static uint32_t tDbg = 0;
  if (millis() - tDbg > 1000) {
    tDbg = millis();
    //Serial.print("REG_IN=0b");
    //Serial.print(leerReg(REG_IN), BIN);
    //Serial.print(" pantallaOn=");
    //Serial.println(pantallaOn);
  }
#endif

  if (botonAmarillo()) {
    if (!pantallaOn) {
      pantallaMostrar();        // despierta, dibuja el dato actual y enciende
    } else {
      tPantalla = millis();     // ya encendida: reinicia el tiempo
    }
  }
  if (pantallaOn && (millis() - tPantalla > PANTALLA_MS)) {
    pantallaApagar();
  }
}

// Espera corta entre comprobaciones del botón
void esperaCorta() {
#if USAR_LIGHT_SLEEP
  esp_sleep_enable_timer_wakeup(100000ULL);   // 100 ms
  esp_light_sleep_start();
#else
  delay(100);
#endif
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(1500);

  battery.begin();
  battery.enableCharge();

  analogReadResolution(12);
  pinMode(SENSOR_PIN, INPUT);
  pinMode(SENSOR_PWR_PIN, OUTPUT);
  digitalWrite(SENSOR_PWR_PIN, LOW);   // sensor apagado por defecto

  display.begin();               // inicializa la pantalla
  pinMode(LCD_BACKLIGHT, OUTPUT);
  display.setRotation(1);
  pantallaEncender();            // encendida solo durante el arranque y el join
  mensajePantalla("Iniciando LoRa...", TFT_YELLOW);

  Serial.println("\n== Nesso N1 - LoRaWAN ==");

  // 1. Poner el módulo LoRa en estado válido (reset + LNA + switch) ANTES de radio.begin()
  iniciarControlLoRa();

  // 2. SPI (compartido con pantalla y flash)
  SPI.begin();

  // 3. Radio (RadioLib 8.x: begin recibe una estructura de configuración)
  ConfigLoRa_t cfg;   // valores por defecto; LoRaWANNode fija sus parámetros al hacer el join
  int state = radio.begin(cfg);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("Error SX1262: ");
    Serial.println(state);
    mensajePantalla("Error SX1262", TFT_RED);
    while (true) delay(1000);
  }
  radio.setDio2AsRfSwitch(true);

  // 4. Join OTAA
  mensajePantalla("Uniendo ChirpStack...", TFT_ORANGE);

  // NVS y callbacks de persistencia ANTES de restaurar
  store.begin("lorawan", false);
  store.remove("nonces");   // clave del formato antiguo (RadioLib 7.x), ya no sirve
  node.setCallbackStorePersistence(guardarPersistencia);
  node.setCallbackRestorePersistence(restaurarPersistencia);

  node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);

  int r = node.loadBuffers();   // restaura DevNonce y sesión guardados (si hay)
  Serial.print("Persistencia restaurada, codigo: ");
  Serial.println(r);

  Serial.println("Uniendo a ChirpStack...");
  state = node.activateOTAA();

  // En RadioLib el join correcto devuelve NEW_SESSION (no ERR_NONE)
  while (state != RADIOLIB_LORAWAN_NEW_SESSION && state != RADIOLIB_LORAWAN_SESSION_RESTORED) {
    Serial.print("Fallo en Join (codigo: ");
    Serial.print(state);
    Serial.println("). Reintentando en 30s...");
    radio.sleep();             // dormir el radio entre intentos de join
    delay(30000);
    state = node.activateOTAA();
  }

  node.setADR(false);        // sin ADR: el data rate no baja solo a SF7
  node.setDatarate(3);       // DR3 = SF9, más margen de enlace

  Serial.println(">>> NODO CONECTADO <<<");
  mensajePantalla("Conectado!", TFT_GREEN);
  delay(1200);

  pantallaApagar();          // a partir de aquí, pantalla apagada salvo al medir/enviar o pulsar el botón
}

// ---------------- Loop ----------------
void loop() {
  uint32_t inicio = millis();

  // Lectura del sensor: encender, esperar, medir, apagar
  digitalWrite(SENSOR_PWR_PIN, HIGH);
  delay(SENSOR_WARMUP_MS);
  long suma = 0;
  for (int i = 0; i < 25; i++) {
    suma += analogRead(SENSOR_PIN);
    delay(10);
  }
  digitalWrite(SENSOR_PWR_PIN, LOW);
  int valorCrudo = suma / 25;

  float humedad = map(valorCrudo, valorAire, valorAgua, 0, 100);
  humedad = constrain(humedad, 0.0, 100.0);
  ultimaHumedad = humedad;

  Serial.print("Lectura cruda: ");
  Serial.print(valorCrudo);
  Serial.print(" | Humedad: ");
  Serial.print(humedad, 1);
  Serial.println(" %");

  // Encender la pantalla al medir, mostrando "Enviando..."
  ultimoEstado = "Enviando...";
  ultimoColor = TFT_YELLOW;
  pantallaMostrar();

  // Payload: humedad * 100 en 2 bytes, big-endian
  uint16_t humedadInt = (uint16_t)(humedad * 100);
  uint8_t payload[2] = { (uint8_t)(humedadInt >> 8), (uint8_t)(humedadInt & 0xFF) };

  // Despertar LoRa, enviar y volver a dormirlo
  despertarLoRa();
  int state = node.sendReceive(payload, sizeof(payload), 1);
  dormirLoRa();

  if (state >= 0) {            // 0 = enviado; >0 = enviado + downlink recibido
    Serial.println("Uplink enviado.");
    ultimoEstado = "LoRa: OK";
    ultimoColor = TFT_GREEN;
  } else {
    Serial.print("Error de envio: ");
    Serial.println(state);
    ultimoEstado = "LoRa: Error";
    ultimoColor = TFT_RED;
  }

  pantallaMostrar();     // muestra LoRa: OK / Error y la batería actual; reinicia el temporizador

  // Espera hasta completar el intervalo, comprobando el botón cada 100 ms
  while (millis() - inicio < INTERVALO_MS) {
    gestionarPantalla();
    esperaCorta();
  }
}
