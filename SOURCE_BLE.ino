#include <NimBLEDevice.h>
#include <SPI.h>
#include <math.h>
#include <stdlib.h>
#include <map>

// Source
#define SOURCE_PIN  6
#define SOURCE_RES  8
#define TABLE_SIZE  128

// Modes
#define ON    1
#define OFF   0

// // Frequences
// #define FREQ_10Hz   0
// #define FREQ_20Hz   1
// #define FREQ_30Hz   2
// #define FREQ_40Hz   3
// #define FREQ_50Hz   4
// #define FREQ_60Hz   5
// #define FREQ_70Hz   6
// #define FREQ_80Hz   7
// #define FREQ_90Hz   8
// #define FREQ_100Hz  9

const int tabHz[10] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
uint8_t sineTable[TABLE_SIZE];
volatile uint8_t currentMode = OFF; 
volatile uint8_t currentFreq = 0;   

volatile float samplesPerSec = (float) TABLE_SIZE * (float) tabHz[currentFreq];
volatile unsigned long microsPerSample = (unsigned long) (1000000.0 / samplesPerSec);

// Objets et pointeurs BLE 
NimBLEServer* pServer = NULL;
NimBLECharacteristic* pModeChar = NULL;
NimBLECharacteristic* pFrequencyChar = NULL;

volatile bool deviceConnected = false;
bool wasConnected = false;
volatile bool modeWritten = false;
volatile bool freqWritten = false;

//SemaphoreHandle_t timerSourceFlag;

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


// void IRAM_ATTR Source_handler() {
//   BaseType_t xHigherPriorityTaskWoken = pdFALSE;
//   xSemaphoreGiveFromISR(timerSampleFlag, &xHigherPriorityTaskWoken);
//   if (xHigherPriorityTaskWoken) {
//     portYIELD_FROM_ISR();
//   }
// }

void SINE_Task(void* pv) { 
  int i = 0;
  unsigned long lastUpdate = 0;
  
  while(1){

    if(currentMode == ON){
      unsigned long now = micros();
      
      // Mise à jour de l'échantillon à l'intervalle calculé
      if(now - lastUpdate >= microsPerSample){
        lastUpdate = now;
        ledcWrite(0, sineTable[i]);             
        i++;
        if(i > TABLE_SIZE - 1) i = 0;
      }
      
      taskYIELD();
    } 
    
    else {
      i = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    // if (xSemaphoreTake(timerSourceFlag, portMAX_DELAY) == pdTRUE){
    //   if(activeSource){
    //     ledcWrite(0, sineTable[i]);
    //     i++;
    //     if(i > 255) i = 0;
    //   } 
      
    //   else {
    //     i = 0;
    //   }
    // }
  }
}

void setupSineTable() {
  for (int i = 0; i < TABLE_SIZE; ++i) {
    float angle = (2.0 * PI * i) / TABLE_SIZE;
    float s = sin(angle);                 
    uint8_t v = (uint8_t)( (s * 0.5 + 0.5) * ((1 << SOURCE_RES) - 1)); 
    sineTable[i] = v;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); 

  // timerSourceFlag = xSemaphoreCreateBinary();
  
  xTaskCreatePinnedToCore(SINE_Task, "SINE_Task", 4096, NULL, 10, NULL, 1);

  // Configuration BLE 
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

  // Timer pour générer le sinus de la source
  // timerSource = timerBegin(1, 80, true); 
  // timerAttachInterrupt(timerSource, &Source_handler, true);
  // timerAlarmWrite(timerSource, microsPerSample, true);
  // timerAlarmEnable(timerSource);
  
  ledcSetup(0, 40000, SOURCE_RES); 
  ledcAttachPin(SOURCE_PIN, 0);
  ledcWrite(0, TABLE_SIZE/2 - 1);

  setupSineTable();

  delay(1000);

  pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));
  pFrequencyChar->setValue((uint8_t*) &currentFreq, (size_t) sizeof(currentFreq));

  Serial.println("Système prêt.");
}

void loop() {

  if (deviceConnected) {
    
    if(modeWritten){
      modeWritten = false;
      
      if(currentMode == OFF) ledcWrite(0, TABLE_SIZE/2 - 1); 
      
      pModeChar->setValue((uint8_t*) &currentMode, (size_t) sizeof(currentMode));
    }


    if(freqWritten){
      freqWritten = false;
      
      samplesPerSec = (float) TABLE_SIZE * (float) tabHz[currentFreq];
      microsPerSample = (unsigned long)(1000000.0 / samplesPerSec);

      // timerAlarmDisable(timerSource);
      // timerAlarmWrite(timerSource, microsPerSample, true);
      // timerAlarmEnable(timerSource);
      
      pFrequencyChar->setValue((uint8_t*) &currentFreq, (size_t) sizeof(currentFreq));    
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

