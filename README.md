# BeagleBone Black Hardware Configuration Notes

This repository contains configuration notes and reference code for interfacing the **BeagleBone Black (BBB)** with **CAN Bus** and **UART** peripheral devices.

## 1. CAN Bus Configuration 

The BeagleBone Black provides access to CAN controllers via the P9 expansion header. 

# Initialize CAN 
config-pin P9.24 can
config-pin P9.26 can

# Initialize UART2 
config-pin P9.21 uart
config-pin P9.22 uart

# Bring up CAN network
ip link set can1 type can bitrate 250000
ip link set can1 up

echo "Hardware initialized: CAN1 on P9.24/26, UART2 on P9.21/22"

### Pin Mapping
| Function | BeagleBone Pin | Pin Description |
| :--- | :--- | :--- |
| **CAN1 TX** | P9.24 | UART1_TXD (Configured as CAN1_TX) |
| **CAN1 RX** | P9.26 | UART1_RXD (Configured as CAN1_RX) |

### Setup Commands
To initialize the CAN interface, run the following commands on the BeagleBone:

sudo ip link set can1 type can bitrate 250000

# 3. Bring the interface online
sudo ip link set can1 up
