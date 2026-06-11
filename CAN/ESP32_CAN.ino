#include <SPI.h>
#include <mcp2515.h>

#define CAN_CS_PIN      5
#define CAN_ID          0x123
#define CAN_CLOCK       MCP_8MHZ // CHANGE TO 8MHZ IF YOUR MODULE HAS AN 8MHZ CRYSTAL
#define CAN_SPEED       CAN_250KBPS

MCP2515 mcp2515(CAN_CS_PIN);

void setup() {
    Serial.begin(115200);
    mcp2515.reset();
    mcp2515.setBitrate(CAN_SPEED, CAN_CLOCK);
    mcp2515.setNormalMode();
    
    printf("\n--- CAN Serial Forwarder Ready ---\n");
    printf("Type format: temp,hum,gas (e.g., 25,60,15) and press Enter\n");
}

void loop() {
    // Check if data is available from Serial Monitor
    if (Serial.available() > 0) {
        // Read the incoming string until newline
        String input = Serial.readStringUntil('\n');
        
        // Parse the comma-separated values
        int t = input.substring(0, input.indexOf(',')).toInt();
        int h = input.substring(input.indexOf(',') + 1, input.lastIndexOf(',')).toInt();
        int g = input.substring(input.lastIndexOf(',') + 1).toInt();

        // Create the CAN frame
        struct can_frame tx_frame;
        tx_frame.can_id  = CAN_ID;
        tx_frame.can_dlc = 3;
        tx_frame.data[0] = (uint8_t)t;
        tx_frame.data[1] = (uint8_t)h;
        tx_frame.data[2] = (uint8_t)g;

        // Send to CAN bus
        if (mcp2515.sendMessage(&tx_frame) == MCP2515::ERROR_OK) {
            printf("[SENT] CAN ID: 0x%03X | T:%d H:%d G:%d\n", CAN_ID, t, h, g);
        } else {
            printf("[ERROR] Failed to send over CAN bus.\n");
        }
    }
}
