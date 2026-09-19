# ESP32 Smart Lockbox

Firmware for an IoT package lockbox powered by an ESP32. This system secures package deliveries using a mobile app for admin access, QR code web links for one-time guest access, and a PIR motion sensor for automated locking. 

## Key Features
* **Headless Provisioning:** WiFi credentials and WebSocket server URLs are provisioned via BLE on first boot using a custom mobile app—no hardcoded secrets in the firmware.
* **Auto-Locking Mechanism:** Uses a PIR motion sensor to detect when a package is placed inside, automatically triggering the relay lock after a 15-second delay.
* **Smart Grace Periods:** Motion sensor is temporarily ignored for 45 seconds after an admin unlock to prevent accidental lockouts while retrieving packages.
* **Offline Safe Mode:** If the WebSocket connection drops for more than 8 seconds, the box automatically unlocks to prevent trapped items, saving its state to restore upon reconnection.
* **Real-time Events:** Communicates via WebSockets to send instant `box_locked` events and status heartbeats to the central server.

## Hardware Requirements
* ESP32 Development Board
* Electronic Lock & Relay Module (Pin 26)
* PIR Motion Sensor (Pin 27)
* Status LED (Pin 2)

## Dependencies
This project requires the following libraries:
* `WiFi.h` (Standard ESP32)
* `ArduinoJson` (v6.x)
* `WebSocketsClient` (by Markus Sattler)
* `NimBLEDevice` (h2zero NimBLE-Arduino)

## Setup & Flashing
1. Clone this repository.
2. Open the project in the Arduino IDE or PlatformIO.
3. Install the required libraries via the Library Manager.
4. Flash to your ESP32.
5. On first boot, the status LED will blink, indicating the device has generated its unique ID and is broadcasting its BLE setup network.
6. Connect via the companion app to pass WiFi credentials and the WebSocket server URL.
