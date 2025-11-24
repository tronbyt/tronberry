#!/bin/bash
set -e

# Function to apply flicker fixes
apply_flicker_fixes() {
  echo "Checking for Raspberry Pi optimizations to prevent LED matrix flickering..."

  CONFIG_TXT="/boot/firmware/config.txt"
  CMDLINE_TXT="/boot/firmware/cmdline.txt"
  BLACKLIST_FILE="/etc/modprobe.d/blacklist-rgb-matrix.conf"

  # Check if we are on a Pi by checking for config.txt
  if [ ! -f "$CONFIG_TXT" ]; then
    echo "Warning: Could not find $CONFIG_TXT. Skipping flicker fixes."
    echo "This is expected if you are not running on a Raspberry Pi."
    return
  fi

  MODIFICATIONS_NEEDED=false
  if ! grep -q "^dtparam=audio=off$" "$CONFIG_TXT" 2>/dev/null || \
     ! grep -q "^dtoverlay=disable-bt$" "$CONFIG_TXT" 2>/dev/null || \
     ! grep -q "\bisolcpus=3\b" "$CMDLINE_TXT" 2>/dev/null || \
     ! grep -q "^blacklist snd_bcm2835$" "$BLACKLIST_FILE" 2>/dev/null; then
    MODIFICATIONS_NEEDED=true
  fi

  if [ "$MODIFICATIONS_NEEDED" = false ]; then
    echo "Optimizations to prevent flickering are already applied."
    return
  fi

  read -r -p "Apply Raspberry Pi optimizations to prevent LED matrix flickering? This will disable audio and Bluetooth, modify system files, and requires a reboot. [Y/n] " response
  if [[ "$response" =~ ^([nN][oO]?)$ ]]; then
    echo "Skipping optimizations."
    return
  fi

  echo "Applying optimizations..."
  NEEDS_REBOOT=false

  if ! grep -q "^dtparam=audio=off$" "$CONFIG_TXT" 2>/dev/null; then
    echo "Disabling audio by adding 'dtparam=audio=off' to $CONFIG_TXT"
    echo -e "\ndtparam=audio=off" | sudo tee -a "$CONFIG_TXT" > /dev/null
    NEEDS_REBOOT=true
  fi

  if ! grep -q "^dtoverlay=disable-bt$" "$CONFIG_TXT" 2>/dev/null; then
    echo "Disabling Bluetooth by adding 'dtoverlay=disable-bt' to $CONFIG_TXT"
    echo -e "\ndtoverlay=disable-bt" | sudo tee -a "$CONFIG_TXT" > /dev/null
    NEEDS_REBOOT=true
  fi

  if ! grep -q "\bisolcpus=3\b" "$CMDLINE_TXT" 2>/dev/null; then
    echo "Isolating CPU core 3 by adding 'isolcpus=3' to $CMDLINE_TXT"
    CMDLINE_CONTENT=$(sudo cat "$CMDLINE_TXT")
    echo "$CMDLINE_CONTENT isolcpus=3" | sudo tee "$CMDLINE_TXT" > /dev/null
    NEEDS_REBOOT=true
  fi

  if ! grep -q "^blacklist snd_bcm2835$" "$BLACKLIST_FILE" 2>/dev/null; then
      echo "Blacklisting snd_bcm2835 module..."
      echo "blacklist snd_bcm2835" | sudo tee "$BLACKLIST_FILE" > /dev/null
      echo "Updating initramfs..."
      sudo update-initramfs -u
      NEEDS_REBOOT=true
  fi

  if [ "$NEEDS_REBOOT" = true ]; then
    # This variable will be used outside the function
    export FLICKER_FIX_APPLIED=true
  fi
}

# Apply optimizations
apply_flicker_fixes


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
Nice=-10

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

# Prompt for reboot if needed
if [ "$FLICKER_FIX_APPLIED" = true ]; then
  read -r -p "The system needs to be rebooted for the anti-flickering settings to take effect. Reboot now? [Y/n] " response
  if [[ ! "$response" =~ ^([nN][oO]?)$ ]]; then
    echo "Rebooting..."
    sudo reboot
  fi
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
