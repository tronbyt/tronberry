# Tronberry

A daemon to fetch images from a Tronbyt server and display on a matrix LED display connected to a Raspberry Pi.

It has only been tested with a Raspberry Pi Zero 2W, but should work with all devices supported by https://github.com/hzeller/rpi-rgb-led-matrix.

## Installation

Run the installation script and follow the instructions. The script will ask whether you want to apply optimizations to prevent LED matrix flickering. Note that this will disable audio and Bluetooth and requires a reboot.

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

If you use `tronberry` with the original Tidbyt display, add the `--led-panel-type=FM6126A` flag. For a list of available options, run `./tronberry --help`, there are many knobs to tweak.
