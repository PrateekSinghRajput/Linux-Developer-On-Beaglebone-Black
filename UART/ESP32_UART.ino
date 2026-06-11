#include <Arduino.h>

/* --- UART CONFIGURATION --- */
// Using Hardware Serial 2 on ESP32
#define RXD2 16
#define TXD2 17
#define BAUD_RATE 9600

void setup() {
  // Initialize USB Serial for PC debugging (Input)
  Serial.begin(115200);

  // Initialize UART Serial for BeagleBone Black (Output)
  Serial2.begin(BAUD_RATE, SERIAL_8N1, RXD2, TXD2);

  Serial.println("ESP32 Sensor Node Booted.");
  Serial.println("Type your data below and press Enter to send to the dashboard.");
  Serial.println("Example format: T:45 H:60 G:20");
}

void loop() {
  // Check if you have typed anything in the PC Serial Monitor
  if (Serial.available() > 0) {
    // Read the incoming text until you press Enter (\n)
    String input_data = Serial.readStringUntil('\n');

    // Clean up any hidden carriage return characters (common on Windows)
    input_data.trim();

    // Make sure we don't send empty blank lines
    if (input_data.length() > 0) {
      
      // 1. Send the exact string you typed over UART2 to the screen
      Serial2.println(input_data);

      // 2. Print a confirmation back to your PC terminal
      Serial.print("Sent to BBB: ");
      Serial.println(input_data);
    }
  }
}
