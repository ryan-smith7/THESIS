## Set up the Zephyr environment in the `~/csse4011` folder:

```bash
source $HOME/zephyr_install/env/bin/activate
source zephyr/zephyr-env.sh
```

# SensorNode — Build Commands

## Clean build

```bash
cd SensorNode
# seeeduino_xiao
west build -b esp32_devkitc/esp32/procpu ~/csse4011/repo/prac2/SensorNode --pristine
```

## Flash

```bash
west flash --esp-baud-rate 115200 
```

## Monitor serial output

```bash
ls /dev/tty*
screen /dev/tty.... 115200
```