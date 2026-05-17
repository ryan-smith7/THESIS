# Raspberry Pi Grafana Kiosk Setup

Dedicated always-on display for the Mycorrhizal IoT Sensor Dashboard. The Pi runs Chromium in fullscreen kiosk mode pointing at the public Grafana dashboard — no login required.

---

## Hardware

| Component | Detail |
|---|---|
| Board | Raspberry Pi 3 Model B v1.2 |
| OS | Raspberry Pi OS (32-bit) |
| Storage | MicroSD card (16GB recommended) |
| Display | HDMI monitor |
| Power | Micro USB 5V 2.5A |

---

## Architecture

```
Grafana Cloud (Azure SQL backend)
        ↓
  Public Dashboard URL (no login)
        ↓
  Raspberry Pi — Chromium kiosk
        ↓
  HDMI → Lab monitor (always on)
```

The Pi has no role in the data pipeline — it is a display device only. All data flows through the existing ESP32 → Oracle VM → Azure IoT Hub → Azure Function → SQL → Grafana stack.

---

## Step 1 — Flash the SD Card

1. Download [Raspberry Pi Imager](https://www.raspberrypi.com/software/) on your Mac
2. Select:
   - **Device:** Raspberry Pi 3
   - **OS:** Raspberry Pi OS (32-bit)
   - **Storage:** your SD card
3. Click the **settings gear** and configure:

| Setting | Value |
|---|---|
| Hostname | `grafana-kiosk` |
| Username | `ryan_smit7` |
| Password | *(your password)* |
| WiFi SSID | *(lab WiFi name)* |
| WiFi Password | *(lab WiFi password)* |
| Keyboard layout | `gb` |
| Timezone | `Australia/Brisbane` |
| Enable SSH | Yes — password authentication |

4. Hit **Write** and wait ~10 minutes

---

## Step 2 — First Boot

Insert SD card into Pi, connect HDMI and power. Wait 60–90 seconds, then SSH in from your Mac:

```bash
ssh ryan_smit7@grafana-kiosk.local
```

---

## Step 3 — Update System

```bash
sudo apt update && sudo apt upgrade -y
```

---

## Step 4 — Install Chromium

```bash
sudo apt install chromium -y
```

---

## Step 5 — Disable Screen Blanking

```bash
sudo nano /etc/lightdm/lightdm.conf
```

Add at the bottom of the file:

```
[Seat:*]
xserver-command=X -s 0 -dpms
```

Save with `Ctrl+X → Y → Enter`.

---

## Step 6 — Create Kiosk Autostart

```bash
mkdir -p ~/.config/autostart
nano ~/.config/autostart/kiosk.desktop
```

Paste the following:

```ini
[Desktop Entry]
Type=Application
Name=Kiosk
Exec=chromium --noerrdialogs --kiosk --disable-infobars --force-device-scale-factor=0.8 --check-for-update-interval=31536000 "https://ryanasmith76.grafana.net/public-dashboards/739cf1a6c45a4b92a0fdc657e196729a?theme=light"
```

Save with `Ctrl+X → Y → Enter`.

> `--force-device-scale-factor=0.8` sets browser zoom to 80% — adjust as needed to fit all panels on screen without scrolling.

---

## Step 7 — Enable VNC (Remote Desktop)

```bash
sudo raspi-config
```

Navigate to **3 Interface Options → VNC → Enable**.

To connect remotely from your Mac, install [RealVNC Viewer](https://www.realvnc.com/en/connect/download/viewer/macos/) and connect to `grafana-kiosk.local`.

> VNC only works on the same WiFi network. Use SSH for remote management from home.

---

## Step 8 — Reboot

```bash
sudo reboot
```

After ~60 seconds Chromium will launch fullscreen on the Grafana dashboard automatically.

---

## Changing WiFi Network

```bash
sudo raspi-config
```

Navigate to **1 System Options → S1 Wireless LAN** and enter the new SSID and password.

---

## Grafana Public Dashboard

| Setting | Value |
|---|---|
| URL | `https://ryanasmith76.grafana.net/public-dashboards/739cf1a6c45a4b92a0fdc657e196729a` |
| Theme | `?theme=light` appended to URL |
| Access | Public — no login required |
| Refresh | Auto |

To update the dashboard URL on the Pi:

```bash
nano ~/.config/autostart/kiosk.desktop
# Edit the URL in the Exec line
sudo reboot
```

---

## Troubleshooting

**Chromium not launching on boot:**
```bash
ps aux | grep chromium
cat ~/.config/autostart/kiosk.desktop
```
Make sure the `Exec` line uses `chromium` not `chromium-browser`.

**No signal on monitor:**
- Press the JOG button on the back of the Samsung monitor to switch input source to HDMI
- Make sure HDMI was connected before powering the Pi

**SSH connection refused:**
```bash
ssh ryan_smit7@10.194.1.165   # use IP directly if .local doesn't resolve
```

**Wrong WiFi after moving to lab:**
```bash
sudo raspi-config  # System Options → Wireless LAN
```
