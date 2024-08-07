# ESP-BOX Example

This example shows how to use the `espp::EspBox` hardware abstraction component
to automatically detect and initialize components on both the ESP32-S3-BOX and
the ESP32-S3-BOX-3.

It initializes the touch, display, and audio subsystems. It reads the touchpad
state and each time you touch the scren it 1) uses LVGL to draw a circle where
you touch, and 2) play a click sound (wav file bundled with the firmware). If
you press the home button on the display, it will clear the circles.

https://github.com/esp-cpp/espp/assets/213467/d5379983-9bc2-4d56-a9fc-9e37f54af15e

![image](https://github.com/esp-cpp/espp/assets/213467/bfb45218-0d1f-4a07-ba30-c0c74a1657b9)
![image](https://github.com/esp-cpp/espp/assets/213467/c7216cfd-330d-4610-baf2-30001c98ff42)

## How to use example

### Hardware Required

This example is designed to run on the ESP32-S3-BOX or ESP32-S3-BOX-3.

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view
serial output:

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

BOX3:
![CleanShot 2024-07-01 at 08 47 31](https://github.com/esp-cpp/espp/assets/213467/27cdec8e-6db0-4e3d-8fd6-91052ce2ad92)

BOX:
![CleanShot 2024-07-01 at 09 56 23](https://github.com/esp-cpp/espp/assets/213467/2f758ff5-a82e-4620-896e-99223010f013)
