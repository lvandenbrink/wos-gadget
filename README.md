# WOS-Gadget

Forked from: http://github.com/kittengineering/Omgevingsmonitor_Software


| Constrols | | | |
|---------|:--------:|-----:|-----:|
| LED     |   Status | dB   | VOC  |
| Buttons |    RESET | BOOT | USER |


Press BOOT + USER button during start up -> Process PC Config

Press USER button during start up -> Start Wifi config AP

The dB LED has the following meaning, more or less in the order of the rainbow:
dBA >= 90 white
dBA >= 80 && dBA < 90 red
dBA >= 70 && dBA < 80 yellow
dBA >= 60 && dBA < 70 green
dBA >= 50 && dBA < 60 light blue
dBA >= 40 && dBA < 50 blue
dBA >= 35 && dBA < 40 purple
dBA < 35 LED off, equals to noise level of the microphone

## Getting started

Reference: https://wiki.deomgevingsmonitor.nl/index.php/Programmeeromgeving

Install [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html#get-softwarehttps://www.st.com/en/development-tools/stm32cubeprog.html#get-software)

Follow the wizard
```
./stm32cubeprg-lin-v2-20-0/SetupSTM32CubeProgrammer-2.20.0.linux 
```

## State diagram

### Main State
![Main State](Main_Thread_State_Diagram.png)

### ESP upkeep State
![ESP upkeep State](ESP_STATE_Diagram.png)

### Measurement State
![Measurement State](Measurement_State_Diagram.png)

### Diagrams
Requirements to view PlantUML diagrams:
- Java
- graphviz
```
sudo apt install graphviz
```
- VSCode extention: [PlantUML](https://marketplace.visualstudio.com/items?itemName=jebbs.plantuml)


## Compiling
Compile the program with either STM32CubeIDE or command line.

### 1. STM32CubeIDE

    1. Open STM32CubeIDE
    2. Import the project:
        - File → Import → General → Existing Projects into Workspace
        - Browse to wos-gadget
        - Select the project and click Finish
    3. Build the project:
        - Right-click on project → Build Project
        - Or use Ctrl+B
        - Or click the hammer icon in the toolbar
    
    4. Manage Embedded Software Packages
        - Install `STM32CubeL0` version `V1.12.2` which install package in `STM32Cube/Repository/STM32Cube_FW_L0_V1.12.2`

### 2. Commnand line
```
# from project root 
cmake --preset debug
cmake --build --preset debug
```
## Deploying

Start the STM32CubeProgrammer select `build/MSJGadget*.elf` 
Press middle (Reset) and right (Boot) button. Let go of the right button first. 
Upload the firmware with 'Download'.  
Press 2x reset 

## Connecting

```
gtkterm -p /dev/ttyACM0 -s 115200
```

# Troubleshoot

## compiling in Linux
`fatal error: String.h: No such file or directory`

Change '#include <String.h>' to '#include <string.h>
Change '#include "EEprom.h"' to '#include <EEProm.h>
Change '#include "GPIO.h"' to '#include "gpio.h"'

### Home Assistant config
Configure Home Assistant to listen to the WOS gadget.
```yaml
# ── Sensors ───────────────────────────────────────────────────
  sensor:
    # WOS Gadget
    - name: "WOS Temperature"
      unique_id: wos_temperature
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.temperature }}"
      unit_of_measurement: "°C"
      device_class: temperature
      state_class: measurement
      device: &wos_device
        identifiers: ["wos_omgevingsmonitor"]
        name: "WOS Omgevingsmonitor"
        model: "WOS Gadget"
        manufacturer: "Custom"
    - name: "WOS Humidity"
      unique_id: wos_humidity
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.humidity }}"
      unit_of_measurement: "%"
      device_class: humidity
      state_class: measurement
      device: *wos_device
    - name: "WOS Sound Level"
      unique_id: wos_sound
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.sound }}"
      unit_of_measurement: "dB(A)"
      state_class: measurement
      icon: mdi:volume-high
      device: *wos_device
    - name: "WOS Battery Voltage"
      unique_id: wos_battery
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.battery }}"
      unit_of_measurement: "V"
      device_class: voltage
      state_class: measurement
      device: *wos_device
    - name: "WOS Solar Voltage"
      unique_id: wos_solar
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.solar }}"
      unit_of_measurement: "V"
      device_class: voltage
      state_class: measurement
      icon: mdi:solar-power
      device: *wos_device
    - name: "WOS VOC Index"
      unique_id: wos_voc
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.voc }}"
      unit_of_measurement: "VOCi"
      state_class: measurement
      icon: mdi:air-filter
      device: *wos_device
    - name: "WOS PM2.5"
      unique_id: wos_pm25
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json['PM2.5'] | default(none) }}"
      unit_of_measurement: "µg/m³"
      device_class: pm25
      state_class: measurement
      device: *wos_device
    - name: "WOS PM10"
      unique_id: wos_pm10
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json['PM10'] | default(none) }}"
      unit_of_measurement: "µg/m³"
      device_class: pm10
      state_class: measurement
      device: *wos_device
    - name: "WOS NOx Index"
      unique_id: wos_nox
      state_topic: "sensor/climate/wos"
      value_template: "{{ value_json.NOx | default(none) }}"
      unit_of_measurement: "ppb"
      state_class: measurement
      icon: mdi:molecule
      device: *wos_device
```