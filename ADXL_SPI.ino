#include <NimBLEDevice.h>
#include <SPI.h>
#include "SdFat.h"
#include <TinyGPS++.h>
#include <math.h>
#include <stdlib.h>
#include <map>

#define TRUE 1
#define FALSE 0

// Paramétrage
#define ADXL_CS 10
#define SD_CS 9 
#define SCK_SD 22
#define MOSI_SD 23
#define MISO_SD 21

#define PPS_PIN 2
#define SOURCE_PIN 6

// Modes
#define LIVE 0
#define RECORD 1

// Plages de mesure
#define _2g 0
#define _4g 1
#define _8g 2

// Frequences d'échantillonnage
#define _4000Hz   0
#define _2000Hz   1
#define _1000Hz   2
#define _500Hz    3
#define _250Hz    4
#define _125Hz    5
#define _62_5Hz   6
#define _31_25Hz  7
#define _15_625Hz 8
#define _7_813Hz  9
#define _3_906Hz  10

// Frequences signal source
#define SOURCE_10Hz   0
#define SOURCE_20Hz   1
#define SOURCE_30Hz   2
#define SOURCE_40Hz   3
#define SOURCE_50Hz   4
#define SOURCE_60Hz   5
#define SOURCE_70Hz   6
#define SOURCE_80Hz   7
#define SOURCE_90Hz   8
#define SOURCE_100Hz  9


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

const int tabHz[11] = {4000, 2000, 1000, 500, 250, 125, 62.5, 31.25, 15.625, 7.813, 3.906};
const int tabSourcefreq[10] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
uint8_t sineTable[256];
volatile uint8_t currentMode = LIVE; 
volatile uint8_t currentRange = _2g;
volatile uint8_t currentFreq = _4000Hz;
volatile uint8_t currentSource_freq = SOURCE_10Hz;   
volatile uint8_t activeSource = FALSE;

volatile int32_t TICK_US = 1000000/tabHz[currentFreq];

volatile uint32_t pps_micros = 0;
float lastLat = 0, lastLon = 0;
char hours[3], minutes[3], seconds[3], title[25];
char interval[3];

volatile float samplesPerSec = 256.0 * (float) tabSourcefreq[currentSource_freq];
volatile unsigned long microsPerSample = (unsigned long)(1000000.0 / samplesPerSec);

// Adresses des registres
const int REG_XDATA3 = 0x08;
const int REG_RANGE  = 0x2C;
const int REG_POWER_CTL = 0x2D;
const int REG_FILTER = 0x28;

// Objets et pointeurs BLE 
NimBLEServer* pServer = NULL;
NimBLECharacteristic* pRawDataChar = NULL;
NimBLECharacteristic* pModeChar = NULL;
NimBLECharacteristic* pRangeChar = NULL;
NimBLECharacteristic* pFrequencyChar = NULL;
NimBLECharacteristic* pSourceFreqChar = NULL;
NimBLECharacteristic* pSourceStatusChar = NULL;


volatile bool deviceConnected = false;
bool wasConnected = false;
volatile bool modeWritten = false;
volatile bool rangeWritten = false;
volatile bool freqWritten = false;
volatile bool source_freqWritten = false;
volatile bool source_statusWritten = false;
bool envoiInit = false;

int32_t x_raw = 0;
int32_t y_raw = 0;
int32_t z_raw = 0;

SemaphoreHandle_t timerSemaphore;

class MyServerCallbacks: public NimBLEServerCallbacks {
  public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo){
      deviceConnected = true;
      // Paramètres : Handle, Min Interval, Max Interval, Latency, Timeout
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

class RangeCallbacks: public NimBLECharacteristicCallbacks {
    public:
      void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {

          if(currentRange != (uint8_t)value[0]){
            currentRange = (uint8_t)value[0];
            rangeWritten = true;
          }

          else{
            rangeWritten = false;
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

class Source_FreqCallbacks: public NimBLECharacteristicCallbacks {
    public:
      void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
      
          if(currentSource_freq != (uint8_t)value[0]){
            currentSource_freq = (uint8_t)value[0];
            source_freqWritten = true;
          }

          else{
            source_freqWritten = false;
          }

        }
      }
};

class Source_StatusCallbacks: public NimBLECharacteristicCallbacks {
    public:
      void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
      
          if(activeSource != (uint8_t) value[0]){
            activeSource = (uint8_t) value[0];
            source_statusWritten = true;
          }

          else{
            source_statusWritten = false;
          }

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


void ADXL_Task(void * pvParameters) {
  while(1) {
    if (xSemaphoreTake(timerSemaphore, portMAX_DELAY) == pdTRUE) {
      ADXL_ReadRaw();

      if(currentMode == LIVE){
        bufferLIVE[activeBuf][bufIdx] = {x_raw, y_raw, z_raw};
        bufIdx++;

        // taille du paquet proportionnel à la fréquence
        if (bufIdx >= (BUF_SIZE_LIVE >> currentFreq)) {
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

void SINE_Task(void* pv) { 
  int i = 0;
  unsigned long lastUpdate = 0;
  
  while(1){
    if(activeSource){
      unsigned long now = micros();
      
      // Mise à jour de l'échantillon à l'intervalle calculé
      if(now - lastUpdate >= microsPerSample){
        lastUpdate = now;
        ledcWrite(0, sineTable[i]);             
        i++;
        if(i > 255) i = 0;
      }
      
      // Empêche le Watchdog de faire redémarrer l'ESP32
      taskYIELD(); 
      
    } else {
      // En cas de pause, on centre le signal (127 = 0V alternatif) et on libère le CPU
      ledcWrite(0, 127);
      i = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(ADXL_CS, LOW);
  SPI.transfer((reg << 1) | 0x00);
  SPI.transfer(value);
  digitalWrite(ADXL_CS, HIGH);
}

void setupSineTable() {
  for (int i = 0; i < 256; ++i) {
    float angle = (2.0 * PI * i) / 256;
    float s = sin(angle);                 
    uint8_t v = (uint8_t)( (s * 0.5 + 0.5) * 255.0 ); 
    sineTable[i] = v;
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, D0, D1); // UART GPS
  delay(1000); 

  // Configuration GPS de la fréquence à 10 Hz
  Serial1.println("$PMTK220,100*2F");

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
  xTaskCreatePinnedToCore(SINE_Task, "SINE_Task", 4096, NULL, 9, NULL, 1);

  // Configuration BLE 
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
  
  pRangeChar = pService->createCharacteristic(
                "19b10003-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );

  pFrequencyChar = pService->createCharacteristic(
                "19b10004-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );

  pSourceFreqChar = pService->createCharacteristic(
                "19b10005-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );

  pSourceStatusChar = pService->createCharacteristic(
                "19b10006-e8f2-537e-4f6c-d104768a1214",
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
              );

  pModeChar->setCallbacks(new ModeCallbacks());
  pRangeChar->setCallbacks(new RangeCallbacks());
  pFrequencyChar->setCallbacks(new FreqCallbacks());
  pSourceFreqChar->setCallbacks(new Source_FreqCallbacks());
  pSourceStatusChar->setCallbacks(new Source_StatusCallbacks());

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

  // Configuration de PPS_PIN
  pinMode(PPS_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), PPS_handler, RISING);


  if (!file.open("init.bin", O_RDWR | O_CREAT | O_AT_END)) {
    Serial.println("Echec critique : impossible de créer le fichier binaire.");
  }
  file.remove();
  file.close();
  
  ledcSetup(0, 40000, 8); 
  ledcAttachPin(SOURCE_PIN, 0);
  ledcWrite(0, 127);

  setupSineTable();

  delay(1000);

  pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));
  pRangeChar->setValue((uint8_t*) &currentRange, (size_t) sizeof(currentRange));
  pFrequencyChar->setValue((uint8_t*) &currentFreq, (size_t) sizeof(currentFreq));
  pSourceFreqChar->setValue((uint8_t*) &currentSource_freq, (size_t) sizeof(currentSource_freq));
  pSourceStatusChar->setValue((uint8_t*) &activeSource, (size_t) sizeof(activeSource));

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

      timerAlarmDisable(timer);

      activeBuf = 0;
      bufIdx = 0;
      fullFlag = false;

      if(currentMode == LIVE){
        if(file.isOpen()) 
          file.close();

        Serial.println("Début du stream...");
      }
      
      else if(currentMode == RECORD){

        Serial.println("Début du stockage des données...");

        if (!file.isOpen()) {

          snprintf(hours, sizeof(hours), "%02d", gps.time.hour());
          snprintf(minutes, sizeof(minutes), "%02d", gps.time.minute());
          snprintf(seconds, sizeof(seconds),"%02d", gps.time.second());

          if(currentRange == _2g) sprintf(interval, "2g");
          else if(currentRange == _4g) sprintf(interval, "4g");
          else if(currentRange == _8g) sprintf(interval, "8g");

          snprintf(title, sizeof(title), "data_%s_%s_%s_UTC_%s.bin", hours, minutes, seconds, interval);

          if (!file.open(title, O_RDWR | O_CREAT | O_AT_END)) {
            Serial.println("Echec critique : impossible de créer le fichier binaire.");
          }
          
          else{
          
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
            file.print(",");

            file.print(interval);
            
            file.println(); 
          }
        }
      }

      timerAlarmEnable(timer);
    }

    if(rangeWritten){
      rangeWritten = false;
      
      pRangeChar->setValue((uint8_t*) &currentRange, (size_t) sizeof(currentRange));

      timerAlarmDisable(timer);

      if(currentRange == _2g){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_RANGE, 0x01); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentRange == _4g){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_RANGE, 0x02); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentRange == _8g){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_RANGE, 0x03); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      if(currentMode == RECORD){

        // Ouverture d'un nouveau fichier pour ne pas mélanger les intervalles de mesures
        if(file.isOpen()) file.close();

        snprintf(hours, sizeof(hours), "%02d", gps.time.hour());
        snprintf(minutes, sizeof(minutes), "%02d", gps.time.minute());
        snprintf(seconds, sizeof(seconds),"%02d", gps.time.second());

        if(currentRange == _2g) sprintf(interval, "2g");
        else if(currentRange == _4g) sprintf(interval, "4g");
        else if(currentRange == _8g) sprintf(interval, "8g");

        snprintf(title, sizeof(title), "data_%s_%s_%s_UTC_%s.bin", hours, minutes, seconds, interval);

        if (!file.open(title, O_RDWR | O_CREAT | O_AT_END)) {
          Serial.println("Echec critique : impossible de créer le fichier binaire.");
        }
        
        else{
    
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
          file.print(",");

          file.print(interval);
          
          file.println(); 
        }
        
      }
      
      timerAlarmEnable(timer);
    }

    if(freqWritten){
      freqWritten = false;
      
      pFrequencyChar->setValue((uint8_t*) &currentFreq, (size_t) sizeof(currentFreq));

      timerAlarmDisable(timer);

      if(currentFreq == _4000Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x00); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _2000Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x01); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _1000Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x02); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _500Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x03); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _250Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x04); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _125Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x05); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _62_5Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x06); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _31_25Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x07); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _15_625Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x08); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _7_813Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x09); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      else if(currentFreq == _3_906Hz){
        writeRegister(REG_POWER_CTL, 0x01);
        writeRegister(REG_FILTER, 0x0A); 
        writeRegister(REG_POWER_CTL, 0x00);
      }

      TICK_US = 1000000/tabHz[currentFreq];

      timerAlarmWrite(timer, TICK_US, true);
      timerAlarmEnable(timer);
    }

    if(source_freqWritten){
      source_freqWritten = false;
      
      samplesPerSec = 256.0 * (float) tabSourcefreq[currentSource_freq];
      microsPerSample = (unsigned long)(1000000.0 / samplesPerSec);
      
      pSourceFreqChar->setValue((uint8_t*) &currentSource_freq, (size_t) sizeof(currentSource_freq));    
    }

    if(source_statusWritten){
      source_statusWritten = false;
    
      if(!activeSource){
        ledcWrite(0, 127); 
      }

      pSourceStatusChar->setValue((uint8_t*) &activeSource, (size_t) sizeof(activeSource));
    }

    // Mise à jour BLE
    if(currentMode == LIVE && fullFlag){ 
      int bufToSave = (activeBuf == 0) ? 2 : activeBuf - 1;
      fullFlag = false;

      uint8_t* ptrBuf = (uint8_t*) bufferLIVE[bufToSave];

      pRawDataChar->setValue(ptrBuf, std::max(1, (int) (BUF_SIZE_LIVE >> currentFreq))*12);
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
  if (!deviceConnected && wasConnected) {
    delay(500); 
    NimBLEDevice::startAdvertising();
    wasConnected = false;
  }
  if (deviceConnected && !wasConnected) {
    wasConnected = true;
  }
}

