# Tronberry Project (`GEMINI.md`)

This document provides a comprehensive overview of the Tronberry project, its structure, and development conventions to guide future interactions.

## Project Overview

Tronberry is a C++ daemon designed to run on a Raspberry Pi. Its primary function is to fetch and display WebP images and animations on an RGB LED matrix panel. The application is built to be robust, with support for fetching content via both HTTP/HTTPS and WebSocket protocols.

The project is architected with a clear separation between data fetching and rendering. A producer-consumer model is used, where a dedicated thread fetches image data and places it into a queue. A separate main thread consumes from this queue to decode and display the images on the LED matrix. This ensures that the display remains active and responsive even during network latency.

### Key Technologies & Libraries

-   **Language:** C++23
-   **Display Driver:** `rpi-rgb-led-matrix` (included as a git submodule). This is the core library for controlling the LED panel on the Raspberry Pi.
-   **WebSocket Communication:** `websocket.h`. A minimal, header-only WebSocket client built on POSIX sockets and OpenSSL, with auto-reconnect.
-   **HTTP Client:** `httpclient.h`. A minimal, header-only HTTP/HTTPS client built on POSIX sockets and OpenSSL.
-   **JSON Parsing:** `json.h`. A minimal, header-only JSON parser and serializer.
-   **Image Decoding:** `libwebp`. Used for decoding WebP images and animations.
-   **Build System:** `make`.

## Building and Running

### Dependencies

The project requires the following libraries to be installed on the target system (Raspberry Pi):
-   `libwebp-dev`
-   `libssl-dev`
-   `zlib1g-dev`

### Local Build

To build the project directly on a Raspberry Pi:

1.  **Clone the repository with submodules:**
    ```sh
    git clone --recurse-submodules https://github.com/tronbyt/tronberry.git
    cd tronberry
    ```
2.  **Compile the code:**
    ```sh
    make
    ```

The project can't be built on non-Linux platforms. Don't attempt to build or test the project on macOS, for example.

### Cross-Compilation

For faster development, the project can be cross-compiled for `aarch64` (Raspberry Pi) using Docker on a more powerful machine.

```sh
docker build -f Dockerfile.cross --output . .
```
This command will produce a statically linked `tronberry` binary in the current directory.

### Running the Application

The application requires a single command-line argument: the URL to fetch images from.

```sh
# The URL can be http(s)://... or ws(s)://...
sudo ./tronberry <URL>
```

The application requires `sudo` to access the Raspberry Pi's GPIO hardware. Additional flags for configuring the LED matrix (e.g., `--led-panel-type`) can be passed. Run `./tronberry --help` for a full list of options.

## Development Conventions

### Code Style

-   The project uses `clang-format` to enforce a consistent code style. The configuration is defined in the `.clang-format` file.

### Architecture

-   **Multithreading:** The application is multithreaded. A `fetch_thread` handles HTTP requests, while the main thread manages the display loop and WebSocket client.
-   **Producer-Consumer Queue:** A `std::queue` (`response_queue`) is used to pass image data from the fetching thread to the display thread, decoupling network operations from rendering.
-   **WebSocket Handling:** The `IXWebSocket` library is used for asynchronous WebSocket communication. It operates on its own thread and uses callbacks (`setOnMessageCallback`) to handle incoming messages.
-   **Remote Control:** When using WebSockets, the server can send JSON messages to:
    -   Adjust the display brightness.
    -   Set the "dwell time" for the next image.
    -   Request that the next image be displayed "immediately," which interrupts the currently playing animation.
-   **Bidirectional Communication:** The client sends `queued` and `displaying` status messages back to the server over the WebSocket to confirm receipt and rendering of images.

### Continuous Integration

-   The GitHub Actions workflow in `.github/workflows/build.yml` automatically builds the project using Docker-based cross-compilation on an `ubuntu-latest` runner for every push and pull request to the `main` branch.
-   When a new tag is pushed, the version from the tag is passed to the build and compiled into the binary. The workflow then creates a GitHub Release and attaches the compiled, stripped binary as an artifact.
