#include <NimBLEDevice.h>
#include <SPI.h>
#include <math.h>
#include <stdlib.h>
#include <map>
#include "SdFat.h"
#include <TinyGPS++.h>

// Signal
#define SOURCE_PIN        6
#define SOURCE_RES        8
#define TABLE_SIZE        128

// Modes
#define ON                1
#define OFF               0

// SPI
#define SD_CS             10

// GPS
#define PPS_PIN           2

#define MAX_NB_IMPULSE    3

// Constantes et variables globales
const int tabHz[13] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 150, 200, 250};
uint8_t rickerTable[TABLE_SIZE];
volatile uint8_t currentMode = OFF; 
volatile uint8_t currentFreq = 0;   
volatile float samplesPerSec = ((float)TABLE_SIZE * PI * (float) tabHz[currentFreq]) / 6.0f;
volatile unsigned long microsPerSample = (unsigned long)(1000000.0f / samplesPerSec); 
volatile int impulsesToLog = 0;

// Objets et variables gps/sd
SdFat sd;
SdFile file;
TinyGPSPlus gps;
volatile uint32_t pps_micros = 0;
int sampleID = 0;
float lastLat = 0, lastLon = 0;
char hours[3], minutes[3], seconds[3], micros_s[7], day[3], month[3], year[5];

// Objets et pointeurs BLE 
NimBLEServer* pServer = NULL;
NimBLECharacteristic* pModeChar = NULL;
NimBLECharacteristic* pFrequencyChar = NULL;

volatile bool deviceConnected = false;
bool wasConnected = false;
volatile bool modeWritten = false;
volatile bool freqWritten = false;

/* - - - Callbacks BLE - - - */

class MyServerCallbacks: public NimBLEServerCallbacks {
  public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo){
      deviceConnected = true;
      pServer->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 400);
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
      
          if(currentMode != (uint8_t)value[0]){
            currentMode = (uint8_t)value[0];
            modeWritten = true;
          }

          else{
            modeWritten = false;
          }

        }
      }
};

class FreqCallbacks: public NimBLECharacteristicCallbacks {
    public:
      void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
      
          if(currentFreq != (uint8_t)value[0]){
            currentFreq = (uint8_t)value[0];
            freqWritten = true;
          }

          else{
            freqWritten = false;
          }

        }
      }
};

/* - - - Génération du signal - - - */

void RICKER_Task(void* pv) { 
  int i = 0;
  int nb_impulse = 0;
  unsigned long lastUpdate = 0;
  bool impulseStarted = false;
  
  while(1){

    if(currentMode == ON){
      unsigned long now = micros();
      
      if(nb_impulse < MAX_NB_IMPULSE){

        if(!impulseStarted) {
            impulsesToLog++; 
            impulseStarted = true;
        }

        if(now - lastUpdate >= microsPerSample){
          lastUpdate = now;
          ledcWrite(0, rickerTable[i]);             
          i++;
          if(i > TABLE_SIZE - 1){
            i = 0;
            nb_impulse++;
            impulseStarted = false;
          }
        }

      }

      else{
        currentMode = OFF;
        pinMode(SOURCE_PIN, OUTPUT);
        digitalWrite(SOURCE_PIN, LOW);

        if(impulsesToLog == 0)  pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));
      }

      taskYIELD();
    } 
    
    else {
      i = 0;
      nb_impulse = 0;
      impulseStarted = false;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void setupRickerTable() {
  int maxPWM = (1 << SOURCE_RES) - 1; 
  
  float minVal = -0.4463f;
  float maxVal = 1.0f;
  float range = maxVal - minVal; 

  for (int i = 0; i < TABLE_SIZE; ++i) {
    
    float x = -3.0f + (6.0f * (float)i) / (float)(TABLE_SIZE - 1);
    
    float y = (1.0f - 2.0f * x * x) * exp(-x * x);
    
    // Normalisation et mapping entre 0 et maxPWM 
    uint16_t v = (uint16_t)(((y - minVal) / range) * maxPWM);
    rickerTable[i] = v;
  }
}

/* - - - Gestionnaire d'interruptions PPS - - - */
void IRAM_ATTR PPS_handler() {
  pps_micros = micros(); 
}

/* - - - Configuration globale de la source - - - */
void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, D0, D1); // UART GPS
  delay(1000); 
  
  // Configuration GPS 
  Serial1.println("$PMTK220,100*2F");
  Serial1.println("$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28");

  // Configuration SPI (module SD)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin();

  if (!sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(10), &SPI))) {
    Serial.println("Erreur Carte SD !");
    sd.initErrorPrint(&Serial); 
    while(1);
  }

  xTaskCreatePinnedToCore(RICKER_Task, "Ricker_Task", 4096, NULL, 10, NULL, 1);

  NimBLEDevice::init("Nodequake"); 
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService* pService = pServer->createService("18b10000-e8f2-537e-4f6c-d104768a1214");

  pModeChar = pService->createCharacteristic(
                "18b10001-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );

  pFrequencyChar = pService->createCharacteristic(
                "18b10002-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );

  pModeChar->setCallbacks(new ModeCallbacks());
  pFrequencyChar->setCallbacks(new FreqCallbacks());

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->setName("Nodequake");
  pAdvertising->start();
  
  ledcSetup(0, 40000, SOURCE_RES); 
  ledcWrite(0, TABLE_SIZE/2 - 1);

  pinMode(SOURCE_PIN, OUTPUT);
  digitalWrite(SOURCE_PIN, LOW);

  setupRickerTable();

  // Configuration de PPS_PIN
  pinMode(PPS_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), PPS_handler, RISING);

  delay(1000);

  pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));
  pFrequencyChar->setValue((uint8_t*) &currentFreq, (size_t) sizeof(currentFreq));

  Serial.println("Système prêt.");
}

void loop() {

  if (deviceConnected) {
    
    if (impulsesToLog > 0) {
      impulsesToLog--;
      sampleID++;

      if(currentMode == OFF && impulsesToLog == 0)  pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));

      unsigned long startAttempt = millis();
      bool gpsFound = false;

      while (millis() - startAttempt < 2000) {
        while (Serial1.available() > 0) {
          if (gps.encode(Serial1.read())) {
            if (gps.location.isValid() && gps.time.isValid() && gps.date.isValid()) {
              lastLat = gps.location.lat();
              lastLon = gps.location.lng();
              gpsFound = true;
              break;
            }
          }
        }
        if (gpsFound) break;
      }

      snprintf(day, sizeof(day), "%02d", gps.date.day());
      snprintf(month, sizeof(month), "%02d", gps.date.month());
      snprintf(year, sizeof(year), "%04d", gps.date.year());

      snprintf(hours, sizeof(hours), "%02d", gps.time.hour());
      snprintf(minutes, sizeof(minutes), "%02d", gps.time.minute());
      snprintf(seconds, sizeof(seconds),"%02d", gps.time.second());
      snprintf(micros_s, sizeof(micros_s),"%06d", micros() - pps_micros);

      bool fileAlreadyExists = sd.exists("Source.csv");

      if (!file.open("Source.csv", O_RDWR | O_CREAT | O_AT_END)){
        Serial.println("Echec critique : impossible d'ouvrir Source.csv");
      }
      else {
        if(!fileAlreadyExists){
          file.println("Sample, Latitude, Longitude, Time (UTC), Date");  
          sampleID = 1;           
        }

        file.print(sampleID); 
        file.print(", ");
        
        file.print(lastLat, 6); 
        file.print(",");
        file.print(lastLon, 6); 
        file.print(",");
        
        file.print(hours); 
        file.print(':');
        file.print(minutes); 
        file.print(':');
        file.print(seconds); 
        file.print('.');
        file.print(micros_s);
        file.print(",");

        file.print(day);
        file.print('/');
        file.print(month);
        file.print('/');
        file.print(year);

        file.println();
        file.close(); 
      }
    }

    if(modeWritten){
      modeWritten = false;

      if(currentMode == ON) ledcAttachPin(SOURCE_PIN, 0);

      else{
        pinMode(SOURCE_PIN, OUTPUT);
        digitalWrite(SOURCE_PIN, LOW);
      }

      pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));
    }

    if(freqWritten){
      freqWritten = false;
      
      samplesPerSec = (float) TABLE_SIZE * (float) tabHz[currentFreq];
      microsPerSample = (unsigned long)(1000000.0 / samplesPerSec);
      
      pFrequencyChar->setValue((uint8_t*) &currentFreq, (size_t) sizeof(currentFreq));    
    }
  }

  // Gestion automatique de la reconnexion
  if (!deviceConnected && wasConnected) {
    delay(500); 
    NimBLEDevice::startAdvertising();
    wasConnected = false;
  }
  if (deviceConnected && !wasConnected) {
    wasConnected = true;
  }

}

