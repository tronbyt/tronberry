#!/bin/bash
set -e

# Parse optional username argument
TARGET_USER=${1:-pi}
HOME_DIR=$(eval echo "~$TARGET_USER")
INSTALL_DIR="$HOME_DIR/tronberry"

echo "Using user: $TARGET_USER"
echo "Install path: $INSTALL_DIR"

echo "Installing required packages..."
sudo apt update
sudo apt install -y libwebp7 libwebpdemux2 jq

echo "Installing Tronberry..."
if [ ! -d "$INSTALL_DIR" ]; then
  mkdir -p "$INSTALL_DIR"
fi
echo "Downloading latest release from GitHub..."
LATEST_URL=$(curl -s https://api.github.com/repos/tronbyt/tronberry/releases/latest | jq -r '.assets[] | select(.name == "tronberry") | .browser_download_url')
if [ -z "$LATEST_URL" ]; then
  echo "Error: Could not find the latest release URL."
  exit 1
fi
curl -fL --progress-bar -o "$INSTALL_DIR/tronberry" "$LATEST_URL"
chmod +x "$INSTALL_DIR/tronberry"

echo "Configure Tronbyt server URL..."

read -r -p "Enter Tronbyt server URL [http://my-server:8000/device-id/next or ws://my-server:8000/device-id/ws]: " TRONBYT_URL
TRONBYT_URL=${TRONBYT_URL:-http://192.168.68.42:8000/d8e59932/next}

echo "Creating tronberry.service..."
SERVICE_FILE=/etc/systemd/system/tronberry.service

sudo bash -c "cat > $SERVICE_FILE" <<EOL
[Unit]
Description=Tronberry LED Matrix Service
After=network-online.target

[Service]
ExecStart=$INSTALL_DIR/tronberry $TRONBYT_URL
WorkingDirectory=$INSTALL_DIR
StandardOutput=inherit
StandardError=inherit
Restart=always

[Install]
WantedBy=multi-user.target
EOL

echo "Enabling and starting the service..."
sudo systemctl daemon-reload
sudo systemctl enable tronberry
sudo systemctl restart tronberry

echo "Tronberry is installed and running as $TARGET_USER!"
echo "Use 'sudo systemctl status tronberry' to check status."

# Prompt for Wi-Fi SSID to disable autoconnect-retries
read -rp "Enter your Wi-Fi SSID (case-sensitive) to enable auto-reconnect on disconnect: " WIFI_SSID
if [ -n "$WIFI_SSID" ]; then
  echo "Applying autoconnect-retries=0 for Wi-Fi network: $WIFI_SSID"
  sudo nmcli connection modify "$WIFI_SSID" connection.autoconnect-retries 0
else
  echo "No SSID entered. Skipping Wi-Fi autoconnect tweak."
fi

echo " ////////////////////////////////////////////////////////////////////////////////"
echo "###############################################################################.,"
echo "#.............................................................................#.,"
echo "#.............................................................................#.,"
echo "#████████╗██████╗..██████╗.███╗...██╗██████╗.███████╗██████╗.██████╗.██╗...██╗#.,"
echo "#╚══██╔══╝██╔══██╗██╔═══██╗████╗..██║██╔══██╗██╔════╝██╔══██╗██╔══██╗╚██╗ ██╔╝#.,"
echo "#...██║...██████╔╝██║...██║██╔██╗.██║██████╔╝█████╗..██████╔╝██████╔╝.╚████╔╝.#.,"
echo "#...██║...██╔══██╗██║...██║██║╚██╗██║██╔══██╗██╔══╝..██╔══██╗██╔══██╗..╚██╔╝..#.,"
echo "#...██║...██║..██║╚██████╔╝██║.╚████║██████╔╝███████╗██║..██║██║..██║...██║...#.,"
echo "#...╚═╝...╚═╝  ╚═╝ ╚═════╝.╚═╝..╚═══╝╚═════╝.╚══════╝╚═╝..╚═╝╚═╝..╚═╝...╚═╝...#.,"
echo "#.............................................................................#.,"
echo "#.............................................................................#.,"
echo "#............................INSTALL COMPLETE.................................#.,"
echo "#.............................................................................#.,"
echo "###############################################################################. "
