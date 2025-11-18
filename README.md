# Tronberry

A daemon to fetch images from a Tronbyt server and display on a matrix LED display connected to a Raspberry Pi.

It has only been tested with a Raspberry Pi Zero 2W, but should work with all devices supported by https://github.com/hzeller/rpi-rgb-led-matrix.

## Installation

In order to avoid flickering, follow these steps (taken from https://github.com/hzeller/rpi-rgb-led-matrix?tab=readme-ov-file#troubleshooting):

- Set `dtparam=audio=off` in `/boot/firmware/config.txt` (disables audio)
- Set `dtoverlay=disable-bt` in `/boot/firmware/config.txt` (disables bluetooth)
- Add `isolcpus=3` at the end of `/boot/firmware/cmdline.txt`
- Run
    ```
    cat <<EOF | sudo tee /etc/modprobe.d/blacklist-rgb-matrix.conf
    blacklist snd_bcm2835
    EOF
    sudo update-initramfs -u
    ```
- Reboot
- Run the installation script and follow the instructions
    ```
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

```sh
# The Tronbyt URL looks like http(s)://…/next or ws(s)://…/ws
sudo ./tronberry ${TRONBYT_URL}
```

If you use `tronberry` with the original Tidbyt display, add the `--led-panel-type=FM6126A` flag. For a list of available options, run `./tronberry --help`, there are many knobs to tweak.

To start `tronberry` at startup, create `/etc/systemd/system/tronberry.service` with the following content:

```ini
[Unit]
Description=Tronberry
After=network-online.target

[Service]
ExecStart=<ABSOLUTE_PATH_TO_TRONBERRY> <TRONBYT_URL>
Restart=always

[Install]
WantedBy=multi-user.target
```

Then run `sudo systemctl enable tronberry` to enable the new service.

To start it manually, run `sudo systemctl start tronberry`.

You may want to configure your Pi to automatically reconnect to the WiFi in case the
connection is interrupted, so that it'll display fresh images as soon as the WiFi
becomes available again:

```
sudo nmcli connection modify "{YOUR_WIFI_SSID}" connection.autoconnect-retries 0
```
