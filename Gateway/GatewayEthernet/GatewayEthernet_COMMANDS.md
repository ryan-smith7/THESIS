# GatewayEthernet — Build Commands

## Set up the Zephyr environment in the `~/csse4011` folder:

```bash
source $HOME/zephyr_install/env/bin/activate
source zephyr/zephyr-env.sh
```

## Clean build

## Build & Run Instructions for Each Task  

```bash
cd GatewayEthernet
west build -b esp32_poe/esp32/procpu \
  /Users/ryan/csse4011/repo/prac2/Gateway/GatewayEthernet \
  --pristine \
  -- -DBOARD_ROOT=/Users/ryan/csse4011 \
     -DDTC_OVERLAY_FILE=/Users/ryan/csse4011/repo/prac2/Gateway/GatewayEthernet/boards/esp32_poe_procpu.overlay
```

## Flash

```bash
west flash --esp-baud-rate 115200
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
grep "ETH_GATEWAY" build/zephyr/include/generated/zephyr/autoconf.h
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
