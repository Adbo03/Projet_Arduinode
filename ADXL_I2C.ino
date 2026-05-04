#include <Wire.h>
#include <math.h>
#include <ArduinoBLE.h>

/* 
NOTE : CE CODE PERMET DE COLLECTER LES DONNEES D'UN ADXL355Z EN I2C + communication BLE
*/

// Adresse I2C de l'ADXL355 (MISO/ASEL relié au GND)
const int ADXL355_ADDR = 0x1D;

// Registres
const int REG_POWER_CTL = 0x2D;
const int REG_XDATA3 = 0x08;
const int REG_RANGE = 0x2C;

// Facteur d'échelle pour la plage par défaut (+/- 2g) : 256000 LSB/g
const float SCALE_FACTOR = 256000;

// Configuration BLE
BLEService acceloService("181A"); // Service standard "Environmental Sensing"
BLEFloatCharacteristic xAcceloChar("2A58", BLERead | BLENotify);
BLEFloatCharacteristic yAcceloChar("2A59", BLERead | BLENotify);
BLEFloatCharacteristic zAcceloChar("2A5A", BLERead | BLENotify);

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Attendre que le port série soit prêt
  while (!Serial) { delay(10); }
  Serial.println("Initialisation de l'ADXL355...");
  
  Wire.beginTransmission(ADXL355_ADDR);
  Wire.write(REG_RANGE);
  Wire.write(0x01); // 0x01 pour 2g, 0x02 pour 4g, 0x03 pour 8g
  Wire.endTransmission();

  // Il faut écrire 0x00 dans POWER_CTL pour passer en mode de mesure.
  Wire.beginTransmission(ADXL355_ADDR);
  Wire.write(REG_POWER_CTL);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    Serial.println("Erreur de communication I2C.");
    while(1); // Bloque le programme si le capteur n'est pas détecté
  }
   
  Serial.println("ADXL355 prêt en mode mesure.");

  // Initialisation du BLE
  if (!BLE.begin()) {
    Serial.println("Échec du démarrage du BLE !");
    while (1);
  }

  BLE.setLocalName("NanoESP32_ADXL");
  BLE.setAdvertisedService(acceloService);

  // Ajouter les caractéristiques au service
  acceloService.addCharacteristic(xAcceloChar);
  acceloService.addCharacteristic(yAcceloChar);
  acceloService.addCharacteristic(zAcceloChar);

  BLE.addService(acceloService);
  BLE.advertise();

  Serial.println("Bluetooth activé, en attente de connexion...");
}

void loop() {
  BLEDevice central = BLE.central();

  if (central) {
    Serial.print("Connecté à : ");
    Serial.println(central.address());

    float ax = 0.0;
    float ay = 0.0;
    float az = 0.0;

    while (central.connected()) {
      Wire.beginTransmission(ADXL355_ADDR);
      Wire.write(REG_XDATA3);
      Wire.endTransmission(false);
      Wire.requestFrom(ADXL355_ADDR, 9);

      if (Wire.available() == 9) {
        // Lecture des octets
        uint32_t x3 = Wire.read();
        uint32_t x2 = Wire.read();
        uint32_t x1 = Wire.read();
        
        uint32_t y3 = Wire.read();
        uint32_t y2 = Wire.read();
        uint32_t y1 = Wire.read();
        
        uint32_t z3 = Wire.read();
        uint32_t z2 = Wire.read();
        uint32_t z1 = Wire.read();

        // Recomposition des données 20 bits (alignées à gauche dans 3 octets)
        int32_t x_raw = (x3 << 12) | (x2 << 4) | (x1 >> 4);
        int32_t y_raw = (y3 << 12) | (y2 << 4) | (y1 >> 4);
        int32_t z_raw = (z3 << 12) | (z2 << 4) | (z1 >> 4);

        // Extension de signe pour les nombres négatifs (complément à deux sur 20 bits)
        if (x_raw & 0x00080000) x_raw |= 0xFFF00000;
        if (y_raw & 0x00080000) y_raw |= 0xFFF00000;
        if (z_raw & 0x00080000) z_raw |= 0xFFF00000;

        // Conversion en g (gravité)
        ax = (float)x_raw / SCALE_FACTOR;
        ay = (float)y_raw / SCALE_FACTOR;
        az = (float)z_raw / SCALE_FACTOR;

        // Calcul de l'inclinaison (Roulis et Tangage) en degrés
        // Note : az doit être différent de 0 pour éviter une division par zéro dans de rares cas.
        float roll = atan2(ay, az) * 180.0 / PI;
        float pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

        // Affichage des données
        Serial.print("Accélérations [g] -> X: ");
        Serial.print(ax, 4);
        Serial.print(" | Y: ");
        Serial.print(ay, 4);
        Serial.print(" | Z: ");
        Serial.print(az, 4);
        
        Serial.print("  ||  Inclinaison [deg] -> Pitch: ");
        Serial.print(pitch, 2);
        Serial.print(" | Roll: ");
        Serial.println(roll, 2);
      }

      // 2. Mettre à jour les caractéristiques Bluetooth
      xAcceloChar.writeValue(ax);
      yAcceloChar.writeValue(ay);
      zAcceloChar.writeValue(az);

      delay(100); // Fréquence d'envoi
    }

    Serial.print("Déconnecté de : ");
    Serial.println(central.address());
  }

}