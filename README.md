# ISOTP BLE Bridge

The goal of this project is to build a native Macchina A0 (or ESP32 clone) firmware which can bridge BLE to ISOTP. The A0 has become increasingly more difficult to find in stock as of 2022, an alternative is to purchase the individual dev boards and assemble your own "clone".

Building and installing:
1) This project is built using the ESP32 native toolchain ESP-IDF (based on FreeRTOS) and can be compiled using `idf.py build`

2) Download and flash precompiled firmware: <br>
https://github.com/Switchleg1/esp32-isotp-ble-bridge/releases/tag/v1.02

# Supported software

1) SimosTools, android based ecu flashing and logging software: <br>
https://play.google.com/store/apps/details?id=com.app.simostools<br>

2) VW_Flash, python based ecu flashing and logging software: <br>
https://github.com/bri3d/VW_Flash<br>

3) Some other J2534 software

# Supported hardware

1) Genuine Macchina A0 <br>
https://www.macchina.cc/catalog/a0-boards/a0-under-dash <br>

2) AMAleg - DIY Macchina A0 clone <br>
https://github.com/Switchleg1/AMAleg
