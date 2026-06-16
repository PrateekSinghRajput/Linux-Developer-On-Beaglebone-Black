#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <SPI.h>
#include <mcp2515.h>

// Pin Definitions for ESP32 C3
#define DHTPIN 4           // DHT11 connected to GPIO 4 (digital pin)
#define GAS_SENSOR_PIN 5   // MQ135 gas sensor connected to GPIO 5 (analog-capable)
#define POT_PIN 3          // Potentiometer (if needed) - GPIO 3
#define CAN_CS_PIN 5       // MCP2515 CS pin - ESP32 default GPIO 5

// DHT Configuration
#define DHTTYPE DHT11
DHT_Unified dht(DHTPIN, DHTTYPE);

// CAN Configuration
#define CAN_ID 0x123
#define CAN_CLOCK MCP_8MHZ
#define CAN_SPEED CAN_250KBPS
MCP2515 mcp2515(CAN_CS_PIN);

// Gas sensor calibration
#define RLOAD 1.720  // MQ135 load resistance in kOhm (measure yours)
float gasConcentration = 0;

void setup() {
  Serial.begin(115200);
  
  // Initialize DHT sensor
  dht.begin();
  Serial.println("--- DHT11 + MQ135 Gas Sensor + CAN Bus ---");
  Serial.println("Waiting for sensor readings...");
  
  // Initialize MCP2515 CAN
  SPI.begin();  // ESP32 default SPI: CS=5, SO=19, SI=23, SCK=13
  mcp2515.reset();
  mcp2515.setBitrate(CAN_SPEED, CAN_CLOCK);
  mcp2515.setNormalMode();
  
  sensor_t sensor;
  dht.temperature().getSensor(&sensor);
  dht.humidity().getSensor(&sensor);
  
  printf("\n--- CAN Serial Forwarder Ready ---\n");
  printf("Reading DHT11 (temp/hum) and MQ135 (gas) every 2 seconds\n");
  printf("Data sent to CAN bus ID: 0x%03X\n", CAN_ID);
}

void loop() {
  // Wait 2 seconds between DHT readings (DHT11 needs minimum delay)
  delay(2000);
  
  // Read DHT11 temperature and humidity
  sensors_event_t event;
  dht.temperature().getEvent(&event);
  float temperature = event.temperature;
  
  dht.humidity().getEvent(&event);
  float humidity = event.relative_humidity;
  
  // Read MQ135 gas sensor (analog)
  int gasRaw = analogRead(GAS_SENSOR_PIN);  // 0-4095 on ESP32 C3
  float gasVoltage = gasRaw * (3.3 / 4095.0);  // Convert to voltage
  
  // Simple gas concentration calculation (ppm approximation)
  // For accurate calibration, use MQ library with R0 measurement [web:26]
  gasConcentration = gasRaw * 0.1;  // Raw value scaled to approximate ppm
  
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C | Humidity: ");
  Serial.print(humidity);
  Serial.print(" % | Gas: ");
  Serial.print(gasConcentration);
  Serial.println(" ppm (raw)");
  
  // Create CAN frame with 3 bytes: temp, humidity, gas
  struct can_frame tx_frame;
  tx_frame.can_id = CAN_ID;
  tx_frame.can_dlc = 3;
  
  // Convert to uint8_t (0-255 range)
  tx_frame.data[0] = (uint8_t)(temperature > 255 ? 255 : temperature);
  tx_frame.data[1] = (uint8_t)(humidity > 255 ? 255 : humidity);
  tx_frame.data[2] = (uint8_t)(gasConcentration > 255 ? 255 : gasConcentration);
  
  // Send to CAN bus
  if (mcp2515.sendMessage(&tx_frame) == MCP2515::ERROR_OK) {
    printf("[SENT] CAN ID: 0x%03X | T:%.1f°C H:%.1f%% G:%.1fppm\n", 
           CAN_ID, temperature, humidity, gasConcentration);
  } else {
    printf("[ERROR] Failed to send over CAN bus.\n");
  }
}
