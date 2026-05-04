#include <SPI.h>
#include <math.h>
#include <ArduinoBLE.h>

/* 
NOTE : COLLECTE DE DONNEES ADXL355 EN SPI
*/

// Configuration des Pins SPI
const int CS_PIN = 10; 

// Registres (Adresses de base)
const int REG_XDATA3 = 0x08;
const int REG_RANGE  = 0x2C;
const int REG_POWER_CTL = 0x2D;

// Facteur d'échelle (+/- 2g) : 256000 LSB/g
const float SCALE_FACTOR = 256000.0;

void writeRegister(uint8_t reg, uint8_t value) {
  uint8_t address = (reg << 1) | 0x00; // Bit LSB à 0 pour l'écriture
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(address);
  SPI.transfer(value);
  digitalWrite(CS_PIN, HIGH);
}

void setup() {
  Serial.begin(9600);
  
  // Initialisation SPI
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  // Paramètres SPI : Max 10MHz, Mode 0 (CPOL=0, CPHA=0)
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  while (!Serial) { delay(10); }
  Serial.println("Initialisation ADXL355 (SPI)...");

  // Configuration de la plage (2g)
  writeRegister(REG_RANGE, 0x01); 
  
  // Passage en mode mesure (écriture 0x00 dans POWER_CTL)
  writeRegister(REG_POWER_CTL, 0x00);

  Serial.println("SPI configuré avec succes !");

  Serial.println("Système prêt.");
}

void loop() {

  // Lecture multiple de 9 octets à partir de XDATA3
  digitalWrite(CS_PIN, LOW);
  SPI.transfer((REG_XDATA3 << 1) | 0x01); // Bit LSB à 1 pour la lecture
  
  uint32_t x3 = SPI.transfer(0x00);
  uint32_t x2 = SPI.transfer(0x00);
  uint32_t x1 = SPI.transfer(0x00);
  
  uint32_t y3 = SPI.transfer(0x00);
  uint32_t y2 = SPI.transfer(0x00);
  uint32_t y1 = SPI.transfer(0x00);
  
  uint32_t z3 = SPI.transfer(0x00);
  uint32_t z2 = SPI.transfer(0x00);
  uint32_t z1 = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);

  // Recomposition 20 bits
  int32_t x_raw = (x3 << 12) | (x2 << 4) | (x1 >> 4);
  int32_t y_raw = (y3 << 12) | (y2 << 4) | (y1 >> 4);
  int32_t z_raw = (z3 << 12) | (z2 << 4) | (z1 >> 4);

  // Extension de signe (20 bits)
  if (x_raw & 0x00080000) x_raw |= 0xFFF00000;
  if (y_raw & 0x00080000) y_raw |= 0xFFF00000;
  if (z_raw & 0x00080000) z_raw |= 0xFFF00000;

  float ax = (float)x_raw / SCALE_FACTOR;
  float ay = (float)y_raw / SCALE_FACTOR;
  float az = (float)z_raw / SCALE_FACTOR;

  Serial.print("X: "); Serial.print(ax, 3);
  Serial.print(" Y: "); Serial.print(ay, 3);
  Serial.print(" Z: "); Serial.println(az, 3);

  delay(100);
}