#!/bin/bash
set -e

# Parse optional username argument
TARGET_USER=${1:-$(whoami)}
HOME_DIR=$(eval echo "~$TARGET_USER")
INSTALL_DIR="$HOME_DIR/tronberry"

echo "Using user: $TARGET_USER"
echo "Install path: $INSTALL_DIR"

echo "Installing required packages..."
sudo apt update
sudo apt install -y ca-certificates curl jq libwebp7 libwebpdemux2

echo "Installing Tronberry..."
if [ ! -d "$INSTALL_DIR" ]; then
  mkdir -p "$INSTALL_DIR"
fi

if systemctl is-active --quiet tronberry; then
  echo "Stopping existing tronberry service..."
  sudo systemctl stop tronberry
fi

echo "Downloading latest release from GitHub..."
LATEST_URL=$(curl -s https://api.github.com/repos/tronbyt/tronberry/releases/latest | jq -r '.assets[] | select(.name == "tronberry") | .browser_download_url')
if [ -z "$LATEST_URL" ]; then
  echo "Error: Could not find the latest release URL."
  exit 1
fi
curl -fL --progress-bar -o "$INSTALL_DIR/tronberry" "$LATEST_URL"
chmod +x "$INSTALL_DIR/tronberry"

SERVICE_FILE=/etc/systemd/system/tronberry.service
SHOULD_CREATE_SERVICE=true
if [ -f "$SERVICE_FILE" ]; then
  read -r -p "tronberry.service already exists. Do you want to keep the existing file? [Y/n] " response
  if [[ ! "$response" =~ ^([nN][oO]?)$ ]]; then
    SHOULD_CREATE_SERVICE=false
    echo "Keeping existing tronberry.service."
  fi
fi

if [ "$SHOULD_CREATE_SERVICE" = true ]; then
  read -r -p "Enter Tronbyt server URL [http://my-server:8000/device-id/next or ws://my-server:8000/device-id/ws]: " TRONBYT_URL
  TRONBYT_URL=${TRONBYT_URL:-http://192.168.68.42:8000/d8e59932/next}

  read -r -p "Optionally enter additional command line flags for tronberry (e.g., --led-rows=128 --led-cols=64): " ADDITIONAL_FLAGS

  echo "Creating tronberry.service..."
  sudo bash -c "cat > $SERVICE_FILE" <<EOL
[Unit]
Description=Tronberry LED Matrix Service
After=network-online.target

[Service]
ExecStart=$INSTALL_DIR/tronberry $TRONBYT_URL $ADDITIONAL_FLAGS
WorkingDirectory=$INSTALL_DIR
StandardOutput=inherit
StandardError=inherit
Restart=always

[Install]
WantedBy=multi-user.target
EOL
fi

echo "Enabling and starting the service..."
sudo systemctl daemon-reload
sudo systemctl enable tronberry
sudo systemctl restart tronberry

echo "Tronberry is installed!"
echo "Use 'sudo systemctl status tronberry' to check status."

# Prompt for Wi-Fi SSID to disable autoconnect-retries
read -rp "Enter your Wi-Fi SSID (case-sensitive) to enable auto-reconnect on disconnect: " WIFI_SSID
if [ -n "$WIFI_SSID" ]; then
  echo "Applying autoconnect-retries=0 for Wi-Fi network: $WIFI_SSID"
  sudo nmcli connection modify "$WIFI_SSID" connection.autoconnect-retries 0
else
  echo "No SSID entered. Skipping Wi-Fi autoconnect tweak."
fi

echo "///////////////////////////////////////////////////////////////////////////////"
echo "###############################################################################"
echo "#.............................................................................#"
echo "#.............................................................................#"
echo "#████████╗██████╗..██████╗.███╗...██╗██████╗.███████╗██████╗.██████╗.██╗...██╗#"
echo "#╚══██╔══╝██╔══██╗██╔═══██╗████╗..██║██╔══██╗██╔════╝██╔══██╗██╔══██╗╚██╗ ██╔╝#"
echo "#...██║...██████╔╝██║...██║██╔██╗.██║██████╔╝█████╗..██████╔╝██████╔╝.╚████╔╝.#"
echo "#...██║...██╔══██╗██║...██║██║╚██╗██║██╔══██╗██╔══╝..██╔══██╗██╔══██╗..╚██╔╝..#"
echo "#...██║...██║..██║╚██████╔╝██║.╚████║██████╔╝███████╗██║..██║██║..██║...██║...#"
echo "#...╚═╝...╚═╝  ╚═╝ ╚═════╝.╚═╝..╚═══╝╚═════╝.╚══════╝╚═╝..╚═╝╚═╝..╚═╝...╚═╝...#"
echo "#.............................................................................#"
echo "#.............................................................................#"
echo "#............................INSTALL COMPLETE.................................#"
echo "#.............................................................................#"
echo "###############################################################################"
