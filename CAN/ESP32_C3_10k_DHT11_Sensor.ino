#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <SPI.h>
#include <mcp2515.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Pin Definitions for ESP32-C3
#define DHTPIN        10          // DHT11 on GPIO 10
#define GAS_SENSOR_PIN 3          // MQ135 on GPIO 3 (analog)
#define CAN_CS_PIN    7           // MCP2515 CS on GPIO 7

// OLED I2C Pins (from your reference)
#define I2C_SDA       8
#define I2C_SCL       9
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// DHT Configuration
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// CAN Configuration
#define CAN_ID      0x123
#define CAN_CLOCK   MCP_8MHZ
#define CAN_SPEED   CAN_250KBPS

MCP2515 mcp2515(CAN_CS_PIN);

// MQ135 Calibration (10 kΩ load resistor)
#define RLOAD       10.0          // Load resistor in kOhm (10 kΩ)
float gasConcentration = 0;
float R0 = 10.0;                  // Approximate sensor resistance in clean air (kΩ)

void setup() {
  Serial.begin(115200);

  // Force I2C pins for OLED
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation failed!");
    while (1) delay(1); // Stop here if screen isn't found
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("DHT11 + MQ135 + CAN");
  display.println("System Starting...");
  display.display();

  // Initialize DHT sensor
  dht.begin();

  Serial.println("--- DHT11 + MQ135 Gas Sensor + CAN Bus (Auto) ---");
  Serial.println("Reading sensors every 2 seconds and sending over CAN");
  Serial.printf("CAN ID: 0x%03X, Speed: 250 kbps, Clock: 8 MHz\n", CAN_ID);

  // Initialize MCP2515 CAN
  SPI.begin();
  mcp2515.reset();
  mcp2515.setBitrate(CAN_SPEED, CAN_CLOCK);
  mcp2515.setNormalMode();

  Serial.println("--- CAN Bus Ready ---");

  // Update OLED
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("DHT11 + MQ135 + CAN");
  display.println("CAN Bus Ready");
  display.println("Reading sensors...");
  display.display();
}

void loop() {
  // Wait 2 seconds between readings (DHT11 needs this)
  delay(2000);

  // Read DHT11 temperature and humidity
  float temperature = dht.readTemperature();  // °C
  float humidity    = dht.readHumidity();     // %

  // Read MQ135 gas sensor (analog)
  int gasRaw = analogRead(GAS_SENSOR_PIN);    // 0–4095 on ESP32-C3
  float gasVoltage = gasRaw * (3.3 / 4095.0); // Voltage at sensor output

  // Calculate sensor resistance Rs (kΩ)
  float Rs = RLOAD * (3.3 - gasVoltage) / gasVoltage;

  // Estimate gas concentration
  float ratio = Rs / R0;
  gasConcentration = 100.0 / (ratio + 1.0); // approximate ppm

  // Serial output
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C | Humidity: ");
  Serial.print(humidity);
  Serial.print(" % | Gas: ");
  Serial.print(gasConcentration);
  Serial.println(" ppm (approx)");

  // Create CAN frame with 3 bytes: temp, humidity, gas
  struct can_frame tx_frame;
  tx_frame.can_id = CAN_ID;
  tx_frame.can_dlc = 3;

  tx_frame.data[0] = (uint8_t)(temperature > 255 ? 255 : (temperature < 0 ? 0 : temperature));
  tx_frame.data[1] = (uint8_t)(humidity    > 255 ? 255 : (humidity    < 0 ? 0 : humidity));
  tx_frame.data[2] = (uint8_t)(gasConcentration > 255 ? 255 : (gasConcentration < 0 ? 0 : gasConcentration));

  // Send to CAN bus
  if (mcp2515.sendMessage(&tx_frame) == MCP2515::ERROR_OK) {
    Serial.printf("[SENT] CAN ID: 0x%03X | T:%.1f°C H:%.1f%% G:%.1fppm\n",
                  CAN_ID, temperature, humidity, gasConcentration);
  } else {
    Serial.println("[ERROR] Failed to send over CAN bus.");
  }

  // Update OLED display
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.println("DHT11 + MQ135 + CAN");
  display.println("-------------------");

  // Temperature
  display.setCursor(0, 10);
  display.print("T: ");
  display.print(temperature, 1);
  display.print(" C");

  // Humidity
  display.setCursor(0, 20);
  display.print("H: ");
  display.print(humidity, 1);
  display.print(" %");

  // Gas
  display.setCursor(0, 30);
  display.print("G: ");
  display.print(gasConcentration, 1);
  display.print(" ppm");

  display.setCursor(0, 40);
  display.println("CAN: 0x123 @250k");

  display.display();
}
