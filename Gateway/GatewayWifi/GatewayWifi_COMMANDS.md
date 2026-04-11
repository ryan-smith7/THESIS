## Set up the Zephyr environment in the `~/csse4011` folder:

```bash
source $HOME/zephyr_install/env/bin/activate
source zephyr/zephyr-env.sh
```

# GatewayWifi — Build Commands

## Clean build

```bash
cd GatewayWifi
west build -b esp32_devkitc/esp32/procpu ~/csse4011/repo/prac2/Gateway/GatewayWifi --pristine
```

## Flash

```bash
west flash
```

## Monitor serial output

```bash
ls /dev/tty*
screen ----- 115200
```

## Clean build directory manually

```bash
rm -rf build
```

## Check autoconf.h for a symbol

```bash
grep "ESP_SPIRAM" build/zephyr/include/generated/zephyr/autoconf.h
```

## RAM usage report (requires successful link)

```bash
west build -t ram_report
```

## ROM usage report

```bash
west build -t rom_report
```

## Open interactive Kconfig menu

```bash
west build -t menuconfig
```
