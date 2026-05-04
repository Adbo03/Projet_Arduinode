#include <ArduinoBLE.h>
#include <SPI.h>
#include <math.h>

// Configuration of SPI Pins
const int CS_PIN = 10; 

// Registers addresses
const int REG_XDATA3 = 0x08;
const int REG_RANGE  = 0x2C;
const int REG_POWER_CTL = 0x2D;

// Scale factor (+/- 2g) : 256000 LSB/g
const float SCALE_FACTOR = 256000.0;

BLEService adxlService("19b10000-e8f2-537e-4f6c-d104768a1214"); // Bluetooth® Low Energy LED Service

// Bluetooth® Low Energy LED Switch Characteristic - custom 128-bit UUID, read and writable by central
BLEByteCharacteristic switchCharacteristic("19b10000-e8f2-537e-4f6c-d104768a1214", BLERead | BLEWrite);
BLEFloatCharacteristic xAcceloChar("2A58", BLERead);
BLEFloatCharacteristic yAcceloChar("2A59", BLERead);
BLEFloatCharacteristic zAcceloChar("2A5A", BLERead);

void writeRegister(uint8_t reg, uint8_t value) {
  uint8_t address = (reg << 1) | 0x00; 
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(address);
  SPI.transfer(value);
  digitalWrite(CS_PIN, HIGH);
}

const int ledPin = LED_BUILTIN; // internal LED pin

void setup() {
  Serial.begin(9600);

  // set LED pin to output mode
  pinMode(ledPin, OUTPUT);

  // SPI Initialisation
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  // Parameters : Max 10MHz, Mode 0 (CPOL=0, CPHA=0)
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  while (!Serial) { delay(10); }

  // BLE initialization
  if (!BLE.begin()) {
    Serial.println("starting Bluetooth® Low Energy module failed!");
    while (1);
  }

  // set advertised local name and service UUID:
  BLE.setLocalName("ADXL355Z");
  BLE.setAdvertisedService(adxlService);

  // add the characteristic to the service
  adxlService.addCharacteristic(switchCharacteristic);
  adxlService.addCharacteristic(xAcceloChar);
  adxlService.addCharacteristic(yAcceloChar);
  adxlService.addCharacteristic(zAcceloChar);

  // add service
  BLE.addService(adxlService);

  // set the initial value for the characeristic:
  switchCharacteristic.writeValue(0);
  xAcceloChar.writeValue(0);
  yAcceloChar.writeValue(0);
  zAcceloChar.writeValue(0);

  // start advertising
  BLE.advertise();
  Serial.println("BLE configuration done.");

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

      // if the remote device wrote to the characteristic,
      // use the value to control the LED:
      if (switchCharacteristic.written()) {
        if (switchCharacteristic.value()) {   // any value other than 0
          Serial.println("LED on");
          digitalWrite(ledPin, HIGH);         // will turn the LED on
        } else {                              // a 0 value
          Serial.println(F("LED off"));
          digitalWrite(ledPin, LOW);          // will turn the LED off
        }
      }

      
    }
    // the central has disconnected
    Serial.println("Disconnected from central: ");
  }
}