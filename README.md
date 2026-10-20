# Tronberry

A daemon to fetch images from a Tronbyt server and display on a matrix LED display connected to a Raspberry Pi.

It has only been tested with a Raspberry Pi Zero 2W, but should work with all devices supported by https://github.com/hzeller/rpi-rgb-led-matrix.

## Installation

Run the installation script and follow the instructions. The script will ask whether you want to apply optimizations to prevent LED matrix flickering. Note that this will disable on-board audio (USB audio remains fully supported) and Bluetooth and requires a reboot.

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/tronbyt/tronberry/HEAD/install.sh)"
```

## Configuration

The installer asks for additional command line flags which can be used to match the connected LED matrix display. The documentation at https://github.com/hzeller/rpi-rgb-led-matrix documents the available flags.

If you missed this during the initial installation, you can either run the installer again or add the flags to the `ExecStart` parameter in `/etc/systemd/system/tronberry.service`.

## Upgrading

To upgrade to a new version, just run the install script again:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/tronbyt/tronberry/HEAD/install.sh)"
```

## Building instructions

If you want to build the Tronbyt client yourself on the device:

```sh
# Install dependencies
sudo apt-get update
sudo apt-get -y install git libwebp-dev libssl-dev zlib1g-dev

# Clone repository
git clone --recurse-submodules https://github.com/tronbyt/tronberry.git

# Build
cd tronberry
make
```

Compilation on the device is slow and might run out of memory, so you may consider cross-compiling the binary on
a more powerful machine using `docker build -f Dockerfile.cross --output . .`.

## Running

For normal operations, the service should be run as a systemd service using `sudo systemctl enable`.

For testing, you can also run it manually (as root):

```sh
# The Tronbyt URL looks like http(s)://…/next or ws(s)://…/ws
sudo ./tronberry ${TRONBYT_URL}
```

### Panel & Color Configuration

For a list of all available options, run `./tronberry --help`. Key configuration flags include:

- **Original Tidbyt Display Panels:** If repurposing an original Tidbyt display panel directly connected to a Raspberry Pi, add `--led-panel-type=FM6126A` to initialize the panel controller. *(Note: Standalone ESP32 Tidbyt Gen 1 devices use Tronbyt Server's built-in "Swap Colors" firmware setting instead).*
- **DIY Matrix Builds & Swapped Colors:** Generic HUB75 matrix panels used in DIY builds often have different RGB pin orderings (for example, Green and Blue channel lines reversed, causing blue to show as green). Add `--led-rgb-sequence=RBG` (or `--led-swap-green-blue`) to correct this. Other sequences like `BGR`, `GRB`, `GBR`, etc. are also supported.
- **Slowdown GPIO (Anti-Flicker / Glitching):** Faster Raspberry Pi models (Zero 2W, Pi 3, Pi 4, Pi 5) can outpace GPIO switching speeds on certain panels. If you observe horizontal line glitching or flickering, adjust `--led-slowdown-gpio=4` (default is `2`).

## Sound Notifications & Audio Configuration

Tronberry supports sound notifications and chimes played through a connected speaker.

### Audio Output & Matrix Anti-Flicker Compatibility
Because HUB75 LED matrices require precise hardware timing that conflicts with the Raspberry Pi's on-board sound system (`snd_bcm2835`), the anti-flicker optimizations disable on-board 3.5mm/HDMI audio. 

**USB audio cards and USB DAC speakers (`snd-usb-audio`) do not conflict with the GPIO matrix timers and are fully supported.** Simply plug in any standard USB audio dongle or USB speaker.

### Permissions & Audio Access
Tronberry retains root privileges by default (`drop_privileges = 0`) so that background audio processes (`mpv`) can directly access `/dev/snd/*` devices. If you explicitly want to drop privileges to user `daemon`, pass `--led-drop-privs` (note that this may block audio access unless `daemon` is added to the `audio` group).

### Systemd Service Configuration
In `/etc/systemd/system/tronberry.service`, `sound.target` is included in `After=` so Tronberry waits for USB audio devices to enumerate before starting and playing the boot chime.

```ini
[Unit]
Description=Tronberry LED Matrix Service
After=network-online.target sound.target

[Service]
ExecStart=/home/pi/tronberry/tronberry ws://my-server:8000/<device-id>/ws --led-no-drop-privs --led-rows=64 --led-cols=128
```

### Hardware Volume (ALSA Mixer)
Many USB audio devices start with their hardware mixer volume low (e.g. 30%) or muted. `install.sh` automatically un-mutes and sets detected cards to 85%. To adjust or verify manually:
```sh
# List audio devices
aplay -l

# Set hardware volume to 85% and unmute
sudo amixer -c 0 sset PCM 85% unmute 2>/dev/null || sudo amixer -c 0 sset Master 85% unmute

# Persist settings across reboots
sudo alsactl store
```

### Volume Control & Flags
- **Initial Volume:** Set with `--volume=<0-100>` (e.g., `--volume=75`) or the `TRONBERRY_VOLUME` environment variable (default: `100`).
- **Live Volume Updates:** When connected via WebSocket, the Tronbyt server can adjust the volume dynamically at runtime (`{"volume": <0-100>}`), or through the `Tronbyt-Volume` HTTP header.
- **Startup Sound:** Tronberry plays a boot chime on launch. You can disable this with `--no-startup-sound` (or `--no-sound`), or by setting `TRONBERRY_NO_STARTUP_SOUND=1`.
- **Audio Backend:** Tronberry uses `mpv` with ALSA (`--ao=alsa`, installed automatically by `install.sh`) or `ffplay` for non-blocking playback in a background worker thread.

### Pairing with Tronbyt Server
In your Tronbyt Server web dashboard:
1. **Device Type:** Configure your device as a speaker-capable device type: **Raspberry Pi**, **Raspberry Pi Wide**, or **Raspberry Pi Square** (do *not* use Tidbyt Gen 1, which has no audio capability on the server).
2. **Endpoint URL:** Use `ws://<server>:<port>/<device-id>/ws` for WebSocket mode (recommended: supports instant image pushes, dynamic volume adjustments, immediate animations, real-time sounds, and instant OTA triggers), or `http://<server>:<port>/<device-id>/next` for HTTP polling.
3. **Volume & Sound Testing:** In the device settings, you can adjust the **Speaker Volume** slider (0–100%) and use the **Play on Device** test button to verify audio output.
4. **App Notifications:** Installed apps with notification sounds configured will chime automatically on display rotation (`every_display`) or content changes (`on_change`).

## Over-The-Air (OTA) Firmware Updates

Tronberry supports seamless managed firmware updates through the Tronbyt Server web UI:
- **Server UI Management:** In Tronbyt Server under **Device Settings -> Maintenance**, select the desired firmware release from the dropdown or upload a custom `tronberry` binary.
- **Automated Replacement:** When triggered, Tronberry receives the download URL via WebSocket (`{"ota_url": "..."}`) or HTTP response header (`Tronbyt-OTA-URL: ...`). It fetches the binary, verifies the ELF binary magic (`\x7fELF`), writes to `<binary>.new`, and atomically replaces the running binary on disk via POSIX `rename()`.
- **Zero-Downtime Restart:** Once replaced, Tronberry cleanly releases display and audio hardware and exits with code 0. The `systemd` service (`Restart=always`) automatically restarts Tronberry with the new version in under 1 second without needing a full system reboot.
