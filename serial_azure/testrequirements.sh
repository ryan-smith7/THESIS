#!/bin/bash

# =============================================================================
# Azure IoT Hub - Python Gateway Setup & Test Script
# =============================================================================

set -e  # Exit on any error

# Terminal colours
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Colour

print_step() { echo -e "\n${BLUE}==>${NC} $1"; }
print_ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
print_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
print_err()  { echo -e "${RED}[ERROR]${NC} $1"; }

# =============================================================================
# Step 1 — Check Python is installed
# =============================================================================
print_step "Checking Python installation..."

if ! command -v python3 &> /dev/null; then
    print_err "Python3 not found. Please install Python 3.7+ first."
    exit 1
fi

PYTHON_VERSION=$(python3 --version)
print_ok "Found $PYTHON_VERSION"

# =============================================================================
# Step 2 — Create and activate virtual environment
# =============================================================================
print_step "Setting up Python virtual environment..."

if [ ! -d "venv" ]; then
    python3 -m venv venv
    print_ok "Virtual environment created"
else
    print_warn "Virtual environment already exists, skipping creation"
fi

source venv/bin/activate
print_ok "Virtual environment activated"

# =============================================================================
# Step 3 — Install dependencies
# =============================================================================
print_step "Installing Python dependencies..."

if [ ! -f "requirements.txt" ]; then
    print_err "requirements.txt not found. Are you in the right directory?"
    exit 1
fi

pip install --upgrade pip -q
pip install -r requirements.txt -q
print_ok "Dependencies installed"

# =============================================================================
# Step 4 — Check for .env file
# =============================================================================
print_step "Checking for .env file..."

if [ ! -f ".env" ]; then
    print_warn ".env file not found — creating a template for you to fill in"

    cat > .env << 'EOF'
# Device connection string — from IoT Hub > Devices > your-device > Primary Connection String
IOTHUB_DEVICE_CONNECTION_STRING=HostName=yourhub.azure-devices.net;DeviceId=esp32-device-01;SharedAccessKey=REPLACE_ME

# Hub connection string — from IoT Hub > Shared Access Policies > iothubowner > Primary Connection String
IOTHUB_CONNECTION_STRING=HostName=yourhub.azure-devices.net;SharedAccessKeyName=iothubowner;SharedAccessKey=REPLACE_ME

# Optional — only needed if testing local HTTP server
API_KEY=your_api_key_here
LOCAL_SERVER_URL=http://localhost:8000/api/telemetry/gateway/
EOF

    echo ""
    print_warn "Please fill in your connection strings in the .env file, then re-run this script."
    echo -e "  Edit with: ${YELLOW}nano .env${NC}"
    exit 0
fi

print_ok ".env file found"

# =============================================================================
# Step 5 — Validate .env has real values (not placeholders)
# =============================================================================
print_step "Validating .env values..."

if grep -q "REPLACE_ME" .env; then
    print_err ".env still contains placeholder values. Please fill in your Azure connection strings."
    echo "  Edit with: nano .env"
    exit 1
fi

if grep -q "IOTHUB_DEVICE_CONNECTION_STRING" .env && grep -q "IOTHUB_CONNECTION_STRING" .env; then
    print_ok ".env looks good"
else
    print_err ".env is missing required keys. Check the template."
    exit 1
fi

# =============================================================================
# Step 6 — Test device-to-cloud (ESP32 → Azure IoT Hub)
# =============================================================================
print_step "Sending test device-to-cloud message (--az-test)..."
echo "  This simulates what the ESP32 will send to Azure IoT Hub."
echo ""

if python3 run.py --az-test; then
    print_ok "Device-to-cloud message sent successfully"
else
    print_err "Device-to-cloud test failed. Check your IOTHUB_DEVICE_CONNECTION_STRING."
    exit 1
fi

# =============================================================================
# Step 7 — Test cloud-to-device (Azure IoT Hub → ESP32)
# =============================================================================
print_step "Sending test cloud-to-device message (--az-mqtt)..."
echo "  This simulates Azure sending a command down to the ESP32."
echo ""

if python3 run.py --az-mqtt; then
    print_ok "Cloud-to-device message sent successfully"
else
    print_err "Cloud-to-device test failed. Check your IOTHUB_CONNECTION_STRING."
    exit 1
fi

# =============================================================================
# Done
# =============================================================================
echo ""
echo -e "${GREEN}=============================================${NC}"
echo -e "${GREEN}  All tests passed! IoT Hub is working.${NC}"
echo -e "${GREEN}=============================================${NC}"
echo ""
echo "Next steps:"
echo "  1. Open Azure IoT Explorer and connect with your IOTHUB_CONNECTION_STRING"
echo "  2. Select esp32-device-01 > Telemetry > Start"
echo "  3. Re-run this script and watch messages arrive in real time"
echo "  4. Once confirmed, we move on to the ESP32 Zephyr MQTT firmware"
echo ""