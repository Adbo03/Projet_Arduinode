#include <NimBLEDevice.h>
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

// Modes
#define LIVE 0
#define RECORD 1

// Bus SPI dédié pour communiquer avec le lecteur SD
SPIClass sdSPI(HSPI);

struct Sample_SD {
  int32_t x, y, z;
  uint32_t sub_sec_us; // Microsecondes écoulées depuis le dernier front PPS
};

struct Sample_LIVE {
  int32_t x, y, z;
};

// Buffers 
const int BUF_SIZE_SD = 32;
Sample_SD bufferSD[2][BUF_SIZE_SD];

const int BUF_SIZE_LIVE = 42; 
Sample_LIVE bufferLIVE[3][BUF_SIZE_LIVE];

volatile int activeBuf = 0;
volatile int bufIdx = 0;
volatile bool fullFlag = false;

SdFat sd;
SdFile file;
TinyGPSPlus gps;
hw_timer_t * timer = NULL;

volatile uint8_t currentMode = LIVE; 
volatile uint32_t pps_micros = 0;
float lastLat = 0, lastLon = 0;
char hours[3], minutes[3], seconds[3];
char title[25];

// Adresses des registres
const int REG_XDATA3 = 0x08;
const int REG_RANGE  = 0x2C;
const int REG_POWER_CTL = 0x2D;

// Facteur d'échelle (+/- 2g) : 256000 LSB/g
const float SCALE_FACTOR = 256000.0;

// Objets et pointeurs BLE 
NimBLEServer* pServer = NULL;
NimBLECharacteristic* pRawDataChar = NULL;
NimBLECharacteristic* pModeChar = NULL;

volatile bool deviceConnected = false;
volatile bool modeWritten = false;

int32_t x_raw = 0;
int32_t y_raw = 0;
int32_t z_raw = 0;

SemaphoreHandle_t timerSemaphore;

// --- GESTIONNAIRES DE CALLBACKS BLE ---
class MyServerCallbacks: public NimBLEServerCallbacks {
  public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo){
      deviceConnected = true;
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason){
      deviceConnected = false;
    }
};

class ModeCallbacks: public NimBLECharacteristicCallbacks {
    public:
      void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
          currentMode = (uint8_t)value[0];
          modeWritten = true;
        }
      }
};

void IRAM_ATTR Sample_handler() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(timerSemaphore, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
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

void ADXL_Task(void * pvParameters) {
  while(1) {
    if (xSemaphoreTake(timerSemaphore, portMAX_DELAY) == pdTRUE) {
      ADXL_ReadRaw();

      if(currentMode == LIVE){
        bufferLIVE[activeBuf][bufIdx] = {x_raw, y_raw, z_raw};
        bufIdx++;

        if (bufIdx >= BUF_SIZE_LIVE) {
          bufIdx = 0;
          activeBuf = (activeBuf + 1) % 3;
          fullFlag = true;
        }
      }

      else if (currentMode == RECORD){
        uint32_t current_micros = micros();
        uint32_t offset_us = current_micros - pps_micros;

        bufferSD[activeBuf][bufIdx] = {x_raw, y_raw, z_raw, offset_us};
        bufIdx++;

        if (bufIdx >= BUF_SIZE_SD) {
          bufIdx = 0;
          activeBuf = (activeBuf == 0) ? 1 : 0;
          fullFlag = true;
        }
      }
    }
  }
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

  Serial.println("Bus SPI initialise.\nTentative initialisation SD...");
  
  if (!sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(10), &sdSPI))) {
      Serial.println("Erreur Carte SD !");
      sd.initErrorPrint(&Serial); 
      while(1);
  }

  timerSemaphore = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(ADXL_Task, "ADXL_Task", 4096, NULL, 10, NULL, 1);

  // --- CONFIGURATION BLE  ---
  NimBLEDevice::init("ADXL355Z");
  NimBLEDevice::setMTU(512); 

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService* pService = pServer->createService("19b10000-e8f2-537e-4f6c-d104768a1214");

  pRawDataChar = pService->createCharacteristic(
                   "19b10001-e8f2-537e-4f6c-d104768a1214",
                   NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
                 );

  pModeChar = pService->createCharacteristic(
                "19b10002-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );
  pModeChar->setCallbacks(new ModeCallbacks());

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->setName("ADXL355Z");
  pAdvertising->setPreferredParams(0x06, 0x12); 
  pAdvertising->start();
  
  Serial.println("Configuration BLE terminée.");

  // Configuration de la précision de l'accéléromètre (2g)
  writeRegister(REG_RANGE, 0x01); 
  writeRegister(REG_POWER_CTL, 0x00);

  // Configuration du Timer ESP32 pour le 4kHz (mode Enregistrement)
  timer = timerBegin(0, 80, true); // Diviseur 80 = 1MHz (1 tick = 1us)
  timerAttachInterrupt(timer, &Sample_handler, true);
  timerAlarmWrite(timer, TICK_US, true);
  timerAlarmEnable(timer);

  pinMode(PPS_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), PPS_handler, RISING);

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


  if (deviceConnected) {
      
      if(modeWritten){
        modeWritten = false;
        pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));

        if(currentMode == LIVE){
          Serial.println("Début du stream...");

          if(file.isOpen()) file.close();

          // On vide les buffers pour stocker les nouvelles données
          activeBuf = 0;
          bufIdx = 0;
          fullFlag = false;
        }
        
        else if(currentMode == RECORD){
  
          Serial.println("Début du stockage des données...");
          
          // On vide les buffers pour stocker les nouvelles données
          activeBuf = 0;
          bufIdx = 0;
          fullFlag = false;

          if (!file.isOpen()) {

            snprintf(hours, sizeof(hours), "%02d", gps.time.hour());
            snprintf(minutes, sizeof(minutes), "%02d", gps.time.minute());
            snprintf(seconds, sizeof(seconds),"%02d", gps.time.second());

            snprintf(title, sizeof(title), "data_%s_%s_%s_UTC.bin", hours, minutes, seconds);

            if (!file.open(title, O_RDWR | O_CREAT | O_AT_END)) {
              Serial.println("Echec critique : impossible de créer dataRaw.bin");
            }
            
            else{
              // On écrit une petite entête GPS au début du fichier
              file.print("GPS:"); 
              file.print(lastLat, 6); 
              file.print(","); 
              file.print(lastLon, 6);
              file.print(",");

              file.print(hours);
              file.print(':');
              file.print(minutes);
              file.print(':');
              file.print(seconds);
              
              file.println(); 
            }
          }
          
        }
      }

      // Mise à jour BLE
      if(currentMode == LIVE && fullFlag){ 
          int bufToSave = (activeBuf == 0) ? 2 : activeBuf - 1;
          fullFlag = false;
   
          uint8_t* ptrBuf = (uint8_t*) bufferLIVE[bufToSave];

          pRawDataChar->setValue(ptrBuf, 504);
          pRawDataChar->notify();
      }
  }

  // Mode Enregistrement 
  if(currentMode == RECORD && fullFlag){
    int bufToSave = (activeBuf == 0) ? 1 : 0;
    
    if(file.isOpen()){
      file.write((const uint8_t*)&bufferSD[bufToSave], sizeof(bufferSD[0]));
      fullFlag = false;
    }

  }

  // Gestion automatique de la reconnexion (Advertising)
  static bool wasConnected = false;
  if (!deviceConnected && wasConnected) {
    delay(500); 
    NimBLEDevice::startAdvertising();
    wasConnected = false;
  }
  if (deviceConnected && !wasConnected) {
    wasConnected = true;
  }
}

