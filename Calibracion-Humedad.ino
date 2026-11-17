/* 

* Arduino Nesso N1 - Sensor Humedad DFROBOT v1.0 

* Pin de Señal: Pin 4 (GPIO 6) del conector HAT J4 

*/ 

 

const int SENSOR_PIN = 6; // GPIO 6 (Pin 4 del HAT J4) 

 

// Calibración del sensor con valores extremos: 

int valorAire = 3220; // Lectura en aire 

int valorAgua = 0; // Lectura mínima sumergido en agua 

 

void setup() { 

Serial.begin(115200); 

delay(1500); 

 

analogReadResolution(12); 

pinMode(SENSOR_PIN, INPUT); 

 

Serial.println("--- Sistema de Humedad N1 Listo (GPIO 6) ---"); 

} 

 

void loop() { 

// Promedio de 25 muestras para estabilizar la lectura 

long suma = 0; 

for (int i = 0; i < 25; i++) { 

suma += analogRead(SENSOR_PIN); 

delay(10); 

} 

int valorCrudo = suma / 25; 

 

// Mapeo inverso: a menor lectura analógica, mayor humedad 

float humedad = map(valorCrudo, valorAire, valorAgua, 0, 100); 

humedad = constrain(humedad, 0.0, 100.0); 

 

Serial.print("Raw ADC: "); 

Serial.print(valorCrudo); 

Serial.print(" | Humedad: "); 

Serial.print(humedad, 1); 

Serial.println(" %"); 

 

delay(1000); 

} 
