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
BLEFloatCharacteristic xAcceloChar("2A58", BLERead | BLENotify);
BLEFloatCharacteristic yAcceloChar("2A59", BLERead | BLENotify);
BLEFloatCharacteristic zAcceloChar("2A5A", BLERead | BLENotify);

void writeRegister(uint8_t reg, uint8_t value) {
  uint8_t address = (reg << 1) | 0x00; 
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(address);
  SPI.transfer(value);
  digitalWrite(CS_PIN, HIGH);
}

void setup() {
  Serial.begin(9600);

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
  adxlService.addCharacteristic(xAcceloChar);
  adxlService.addCharacteristic(yAcceloChar);
  adxlService.addCharacteristic(zAcceloChar);

  // add service
  BLE.addService(adxlService);

  // set the initial value for the characeristic:
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

      // Read from XDATA3 (9 bytes)
      digitalWrite(CS_PIN, LOW);
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
      digitalWrite(CS_PIN, HIGH);

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

      // Bluetooth Update
      xAcceloChar.writeValue(ax);
      yAcceloChar.writeValue(ay);
      zAcceloChar.writeValue(az);

      Serial.print("X: "); Serial.print(ax, 3);
      Serial.print(" Y: "); Serial.print(ay, 3);
      Serial.print(" Z: "); Serial.println(az, 3);

      delay(100);

    }
    // the central has disconnected
    Serial.println("Disconnected from central: ");
  }
}
