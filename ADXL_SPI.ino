#include <ArduinoBLE.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

// Configuration of SPI Pins
const int ADXL_CS = 10;
const int SD_CS = 9; 

// Registers addresses
const int REG_XDATA3 = 0x08;
const int REG_RANGE  = 0x2C;
const int REG_POWER_CTL = 0x2D;

// Scale factor (+/- 2g) : 256000 LSB/g
const float SCALE_FACTOR = 256000.0;

BLEService adxlService("19b10000-e8f2-537e-4f6c-d104768a1214"); // Bluetooth® Low Energy LED Service

// Bluetooth® Low Energy LED Switch Characteristic - custom 128-bit UUID, read and writable by central
BLEFloatCharacteristic xAcceloChar("2A58", BLERead | BLENotify);
BLEFloatCharacteristic yAcceloChar("2A59", BLERead | BLENotify);
BLEFloatCharacteristic zAcceloChar("2A5A", BLERead | BLENotify);

BLEFloatCharacteristic pitchChar("2A5B", BLERead | BLENotify);  // Y rotation
BLEFloatCharacteristic rollChar("2A5C", BLERead | BLENotify);   // X rotation

BLEByteCharacteristic modeChar("2A5D", BLERead | BLEWrite);     // mode (0: Stream, 1: Record, 2: Read)

int currentMode = 0;
File myFile;

void writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(ADXL_CS, LOW);
  SPI.transfer((reg << 1) | 0x00);
  SPI.transfer(value);
  digitalWrite(ADXL_CS, HIGH);
}

void readSDToBLE() {
  myFile = SD.open("adxl355_data.csv");
  if (myFile) {
    Serial.println("Début transmission SD via BLE...");
    while (myFile.available()) {

      float ax = myFile.parseFloat();
      float ay = myFile.parseFloat();
      float az = myFile.parseFloat();
      float pitch = myFile.parseFloat();
      float roll = myFile.parseFloat();

      xAcceloChar.writeValue(ax);
      yAcceloChar.writeValue(ay);
      zAcceloChar.writeValue(az);
      pitchChar.writeValue(pitch);
      rollChar.writeValue(roll);

      delay(100); 
    }
    myFile.close();
    Serial.println("Fin de transmission.");
  }
  currentMode = 0; // Repasse en mode stream après lecture
  modeChar.writeValue(0);
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Laisse le temps au port série de se connecter
  Serial.println("--- DEMARRAGE DU SYSTEME ---");

  // 1. Configurer TOUTES les broches CS en sortie tout de suite
  pinMode(ADXL_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);

  // 2. Désactiver explicitement les deux (état HIGH)
  digitalWrite(ADXL_CS, HIGH);
  digitalWrite(SD_CS, HIGH);
  Serial.println("Pins CS desactivees.");

  // 3. Initialiser le SPI
  SPI.begin();
  Serial.println("Bus SPI initialise.");

  // 4. Initialiser la SD
  Serial.println("Tentative initialisation SD...");
  
  if (!SD.begin(SD_CS)) {
    Serial.println("ECHEC : Carte SD non trouvee ou erreur de cablage.");
    while(1);
  } else {
    Serial.println("SUCCES : Carte SD prête.");
  }

  // BLE initialization
  if (!BLE.begin()) {
    Serial.println("Echec de l'initialisation du module BLE !");
    while (1);
  }

  // set advertised local name and service UUID:
  BLE.setLocalName("ADXL355Z");
  BLE.setAdvertisedService(adxlService);

  // add the characteristic to the service
  adxlService.addCharacteristic(xAcceloChar);
  adxlService.addCharacteristic(yAcceloChar);
  adxlService.addCharacteristic(zAcceloChar);
  adxlService.addCharacteristic(pitchChar); 
  adxlService.addCharacteristic(rollChar);
  adxlService.addCharacteristic(modeChar);

  // add service
  BLE.addService(adxlService);
  
  // set the initial value for the characeristic:
  xAcceloChar.writeValue(0);
  yAcceloChar.writeValue(0);
  zAcceloChar.writeValue(0);
  pitchChar.writeValue(0);
  rollChar.writeValue(0);
  modeChar.writeValue(currentMode);

  // start advertising
  BLE.advertise();
  Serial.println("Configuration BLE terminée.");

  Serial.println("Initialisation ADXL355 (SPI)...");
  // Range configuration (2g)
  writeRegister(REG_RANGE, 0x01); 
  
  // Switch to measurement mode (écriture 0x00 dans POWER_CTL)
  writeRegister(REG_POWER_CTL, 0x00);

  Serial.println("Système prêt.");
}

void loop() {
  // wait for a Bluetooth® Low Energy central
  BLEDevice central = BLE.central();

  // check if a central is connected to this peripheral
  if (central) {
    Serial.print("Connected to central: ");
    // print the central's MAC address:
    Serial.println(central.address());

    // while the central is still connected to peripheral:
    while (central.connected()) {
      
      Serial.print("MODE : ");
      Serial.println(currentMode);

      if(modeChar.written()){
        currentMode = modeChar.value();

        if(currentMode == 2) readSDToBLE();  // Read from SD card 
      }

      // Read from XDATA3 (9 bytes)
      digitalWrite(ADXL_CS, LOW);
      SPI.transfer((REG_XDATA3 << 1) | 0x01); 
      
      uint32_t x3 = SPI.transfer(0x00);
      uint32_t x2 = SPI.transfer(0x00);
      uint32_t x1 = SPI.transfer(0x00);
      
      uint32_t y3 = SPI.transfer(0x00);
      uint32_t y2 = SPI.transfer(0x00);
      uint32_t y1 = SPI.transfer(0x00);
      
      uint32_t z3 = SPI.transfer(0x00);
      uint32_t z2 = SPI.transfer(0x00);
      uint32_t z1 = SPI.transfer(0x00);
      digitalWrite(ADXL_CS, HIGH);

      // Organising the data
      int32_t x_raw = (x3 << 12) | (x2 << 4) | (x1 >> 4);
      int32_t y_raw = (y3 << 12) | (y2 << 4) | (y1 >> 4);
      int32_t z_raw = (z3 << 12) | (z2 << 4) | (z1 >> 4);

      // Sign extension (20 bits)
      if (x_raw & 0x00080000) x_raw |= 0xFFF00000;
      if (y_raw & 0x00080000) y_raw |= 0xFFF00000;
      if (z_raw & 0x00080000) z_raw |= 0xFFF00000;

      float ax = ((float)x_raw) / SCALE_FACTOR;
      float ay = ((float)y_raw) / SCALE_FACTOR;
      float az = ((float)z_raw) / SCALE_FACTOR;

      float pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
      float roll = atan2(ay, az) * 180.0 / PI;

      // Bluetooth Update

      // MODE 0 : STREAM LIVE DATA
      if(currentMode == 0){ 
        xAcceloChar.writeValue(ax);
        yAcceloChar.writeValue(ay);
        zAcceloChar.writeValue(az);
        pitchChar.writeValue(pitch); 
        rollChar.writeValue(roll);
      }

      // MODE 1 : RECORD LIVE DATA ON THE SD CARD
      else if(currentMode == 1){
        myFile = SD.open("adxl355_data.csv", FILE_WRITE);
        if (myFile) {
          myFile.print(ax, 4); myFile.print(",");
          myFile.print(ay, 4); myFile.print(",");
          myFile.print(az, 4); myFile.print(",");
          myFile.print(pitch, 2); myFile.print(",");
          myFile.println(roll, 2);
          myFile.close();
          Serial.print("Ecriture dans la carte SD.... ");
        }
      }

      Serial.print("X: "); Serial.print(ax, 3);
      Serial.print(" Y: "); Serial.print(ay, 3);
      Serial.print(" Z: "); Serial.println(az, 3);
      Serial.print("  |  Pitch: "); Serial.print(pitch, 2);
      Serial.print("° Roll: "); Serial.print(roll, 2);
      Serial.println("°");

      delay(100);
    }

    // the central has disconnected
    Serial.println("Disconnected from central: ");
  }
}

