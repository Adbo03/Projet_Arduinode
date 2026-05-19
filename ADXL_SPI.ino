#include <ArduinoBLE.h>
#include <SPI.h>
#include "SdFat.h"
#include <TinyGPS++.h>
#include <math.h>
#include <stdlib.h>

// Paramétrage
#define ADXL_CS 10
#define SD_CS 9 
#define SCK_SD 22
#define MOSI_SD 23
#define MISO_SD 21

#define ODR_FREQUENCY 4000 // 4kHz
#define TICK_US 1000000 / ODR_FREQUENCY
#define PPS_PIN 2

// Bus SPI dédié pour communiquer avec le lecteur SD
SPIClass sdSPI(HSPI);

// Structure de données (16 octets par échantillonnages)
struct Sample {
  int32_t x, y, z;
  uint32_t sub_sec_us; // Microsecondes écoulées depuis le dernier front PPS
};

// Double buffering
const int BUF_SIZE = 128; 
Sample buffer[2][BUF_SIZE];
volatile int activeBuf = 0;
volatile int bufIdx = 0;
volatile bool fullFlag = false;

SdFat sd;
SdFile file;
TinyGPSPlus gps;
hw_timer_t * timer = NULL;

volatile uint8_t currentMode = 0; 
volatile uint32_t pps_micros = 0;
float lastLat = 0, lastLon = 0;
unsigned long lastBleUpdate = 0;

// Adresses des registres
const int REG_XDATA3 = 0x08;
const int REG_RANGE  = 0x2C;
const int REG_POWER_CTL = 0x2D;

// Facteur d'échelle (+/- 2g) : 256000 LSB/g
const float SCALE_FACTOR = 256000.0;

BLEService adxlService("19b10000-e8f2-537e-4f6c-d104768a1214"); 

// UUID modifiable (128 bits) 
BLEFloatCharacteristic xAcceloChar("2A58", BLERead | BLENotify);
BLEFloatCharacteristic yAcceloChar("2A59", BLERead | BLENotify);
BLEFloatCharacteristic zAcceloChar("2A5A", BLERead | BLENotify);

BLEFloatCharacteristic pitchChar("2A5B", BLERead | BLENotify);  // Rotation axe Y
BLEFloatCharacteristic rollChar("2A5C", BLERead | BLENotify);   // Rotation axe X

BLEByteCharacteristic modeChar("2A5D", BLERead | BLEWrite);     // mode (0: Temps Réel, 1: Enregistrement)

int32_t x_raw = 0;
int32_t y_raw = 0;
int32_t z_raw = 0;

void IRAM_ATTR SD_handler() {

  if (currentMode == 1){
    ADXL_ReadRaw();

    // Calcul de l'offset exact en microsecondes par rapport à la seconde courante
    uint32_t current_micros = micros();
    uint32_t offset_us = current_micros - pps_micros;

    buffer[activeBuf][bufIdx] = {x_raw, y_raw, z_raw, offset_us};
    bufIdx++;

    if (bufIdx >= BUF_SIZE) {
      bufIdx = 0;
      activeBuf = (activeBuf == 0) ? 1 : 0;
      fullFlag = true;
    }
  }

}

void IRAM_ATTR PPS_handler() {
  pps_micros = micros(); 
}

void ADXL_ReadRaw(){
  // 10MHz, Mode 0 (CPOL=0, CPHA=0)
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  
  // Lecture de XDATA3 (9 octets)
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

  SPI.endTransaction();

  // Reformatage des données
  x_raw = (x3 << 12) | (x2 << 4) | (x1 >> 4);
  y_raw = (y3 << 12) | (y2 << 4) | (y1 >> 4);
  z_raw = (z3 << 12) | (z2 << 4) | (z1 >> 4);

  if (x_raw & 0x00080000) x_raw |= 0xFFF00000;
  if (y_raw & 0x00080000) y_raw |= 0xFFF00000;
  if (z_raw & 0x00080000) z_raw |= 0xFFF00000;
}

void writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(ADXL_CS, LOW);
  SPI.transfer((reg << 1) | 0x00);
  SPI.transfer(value);
  digitalWrite(ADXL_CS, HIGH);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, D0, D1); // UART GPS
  delay(1000); 

  // 4. Configurer la fréquence à 10 Hz (100ms entre chaque mesure)
  Serial1.println("$PMTK220,100*2F");

  // Filtrage pour récupérer que l'heure et la position GPS
  Serial1.println("$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28");

  Serial.println("--- DEMARRAGE DU SYSTEME ---");

  pinMode(ADXL_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);

  digitalWrite(ADXL_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  SPI.begin();
  sdSPI.begin(SCK_SD, MISO_SD, MOSI_SD, SD_CS);
  
  Serial.println("Bus SPI initialise.");

  Serial.println("Tentative initialisation SD...");
  
  // On utilise SdSpiConfig pour forcer la vitesse à 16MHz (ou 10MHz pour plus de stabilité)
  if (!sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(10), &sdSPI))) {
      Serial.println("Erreur Carte SD !");
      sd.initErrorPrint(&Serial); 
      while(1);
  }

  // Configuration du Timer ESP32 pour le 4kHz (mode Enregistrement)
  timer = timerBegin(0, 80, true); // Diviseur 80 = 1MHz (1 tick = 1us)
  timerAttachInterrupt(timer, &SD_handler, true);
  timerAlarmWrite(timer, TICK_US, true);
  timerAlarmEnable(timer);

  pinMode(PPS_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), PPS_handler, RISING);

  if (!BLE.begin()) {
    Serial.println("Echec de l'initialisation du module BLE !");
    while (1);
  }

  // Creation du service
  BLE.setLocalName("ADXL355Z");
  BLE.setAdvertisedService(adxlService);

  // Ajout des caractéristiques au service
  adxlService.addCharacteristic(xAcceloChar);
  adxlService.addCharacteristic(yAcceloChar);
  adxlService.addCharacteristic(zAcceloChar);
  adxlService.addCharacteristic(pitchChar); 
  adxlService.addCharacteristic(rollChar);
  adxlService.addCharacteristic(modeChar);

  // Ajout du service
  BLE.addService(adxlService);
  
  // Initialisation des charactéristiques
  xAcceloChar.writeValue(0);
  yAcceloChar.writeValue(0);
  zAcceloChar.writeValue(0);
  pitchChar.writeValue(0);
  rollChar.writeValue(0);
  modeChar.writeValue(currentMode);

  // publication du service BLE
  BLE.advertise();
  Serial.println("Configuration BLE terminée.");

  Serial.println("Initialisation ADXL355 (SPI)...");

  // Configuration de la précision de l'accéléromètre (2g)
  writeRegister(REG_RANGE, 0x01); 
  writeRegister(REG_POWER_CTL, 0x00);

  Serial.println("Système prêt.");
}

void loop() {

  while (Serial1.available() > 0) {

    if (gps.encode(Serial1.read())) {
      if (gps.location.isValid()) {
        lastLat = gps.location.lat();
        lastLon = gps.location.lng();
      }
    }
  }

  BLEDevice central = BLE.central();

  if (central) {
    if (central.connected()){
      if(modeChar.written()){
        currentMode = modeChar.value();

        if(currentMode == 0){
          timerAlarmDisable(timer);
          detachInterrupt(digitalPinToInterrupt(PPS_PIN));
          Serial.println("Début du stream...");
        }
        
        else if(currentMode == 1){
          timerAlarmEnable(timer);
          attachInterrupt(digitalPinToInterrupt(PPS_PIN), PPS_handler, RISING);
          Serial.println("Début du stockage des données...");
          
          // On vide les buffers avant de stocker les nouvelles données
          activeBuf = 0;
          bufIdx = 0;
          fullFlag = false;
        }
      }

      // Mise à jour Bluetooth
      if(currentMode == 0){ 
        
        if(millis() - lastBleUpdate >= 1){
          lastBleUpdate = millis();
          ADXL_ReadRaw();

          float ax = ((float)x_raw) / SCALE_FACTOR;
          float ay = ((float)y_raw) / SCALE_FACTOR;
          float az = ((float)z_raw) / SCALE_FACTOR;

          float pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / PI;
          float roll = atan2(ay, az) * 180.0 / PI;
          
          xAcceloChar.writeValue(ax);
          yAcceloChar.writeValue(ay);
          zAcceloChar.writeValue(az);
          pitchChar.writeValue(pitch); 
          rollChar.writeValue(roll);
        }
      }
    }
  }

  // Mode Enregistrement 
  if(currentMode == 1){

    if (fullFlag) {
      int bufToSave = (activeBuf == 0) ? 1 : 0;

      if (!file.isOpen()) {

        if (!file.open("dataRaw.bin", O_RDWR | O_CREAT | O_AT_END)) {
          Serial.println("Echec critique : impossible de créer dataRaw.bin");
        }
        
        else{
          // On écrit une petite entête GPS au début du fichier
          file.print("GPS:"); 
          file.print(lastLat, 6); 
          file.print(","); 
          file.print(lastLon, 6);
          file.print(",");

          // Formatage manuel de l'heure HH:MM:SS
          if (gps.time.hour() < 10) file.print('0'); file.print(gps.time.hour());
          file.print(':');
          if (gps.time.minute() < 10) file.print('0'); file.print(gps.time.minute());
          file.print(':');
          if (gps.time.second() < 10) file.print('0'); file.print(gps.time.second());
          
          file.println(); // Fin de la ligne GPS
        }
      }
      
      if(file.isOpen()){
        file.write((const uint8_t*)&buffer[bufToSave], sizeof(buffer[0]));
        file.sync();
        fullFlag = false;
      }

    }
  }
}

