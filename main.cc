#include <webp/decode.h>
#include <webp/demux.h>

#include <sys/reboot.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

#include "httpclient.h"
#include "websocket.h"
#include "json.h"
#include "led-matrix.h"
#include "startup.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

#define FIRMWARE_TYPE "Tronberry"
#define PROTOCOL_VERSION 1

using rgb_matrix::CreateMatrixFromOptions;
using rgb_matrix::FrameCanvas;
using rgb_matrix::PrintMatrixFlags;
using rgb_matrix::RGBMatrix;
using rgb_matrix::RuntimeOptions;

struct AppState {
  std::atomic<bool> running{true};
  std::atomic<int> brightness{INITIAL_BRIGHTNESS};
  std::atomic<bool> redraw_frame{false};
  bool verbose = false;
  std::condition_variable queue_not_full;
  std::condition_variable queue_not_empty;
};

static AppState *g_app_state = nullptr;

static std::string get_hostname() {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) == 0) {
    buf[sizeof(buf) - 1] = '\0';
    return buf;
  }
  return "unknown";
}

static std::string get_mac_address() {
  for (const char *iface_name : {"eth0", "wlan0"}) {
    std::ifstream iface(std::format("/sys/class/net/{}/address", iface_name));
    if (iface.is_open()) {
      std::string mac;
      if (std::getline(iface, mac)) {
        // Trim whitespace from end
        size_t endpos = mac.find_last_not_of(" \t\n\r");
        if (std::string::npos != endpos) {
          mac = mac.substr(0, endpos + 1);
        }
        return mac;
      }
    }
  }
  return "xx:xx:xx:xx:xx:xx";  // fallback
}

static void Log(const AppState &state, const std::string &message) {
  if (!state.verbose) {
    return;
  }
  auto now = std::chrono::system_clock::now();
  std::time_t now_c = std::chrono::system_clock::to_time_t(now);
  char timebuf[64];
  std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S",
                std::localtime(&now_c));
  std::cout << "[" << timebuf << "] " << message << std::endl;
}

static void InterruptHandler(int) {
  // Only the atomic store is strictly signal-safe. The notify_all calls on
  // condition variables are technically undefined behavior in a signal handler,
  // but are needed to unblock waiting threads for shutdown. In practice this
  // works reliably on Linux/glibc.
  if (g_app_state) {
    g_app_state->running = false;
    g_app_state->queue_not_empty.notify_all();
    g_app_state->queue_not_full.notify_all();
  }
}

static void DrawFrame(FrameCanvas *canvas, const uint8_t *frame_data, int width,
                      int height) {
  int max_x = std::min(width, canvas->width());
  int max_y = std::min(height, canvas->height());
  for (int y = 0; y < max_y; ++y) {
    for (int x = 0; x < max_x; ++x) {
      int index = (y * width + x) * 4;  // RGBA
      canvas->SetPixel(x, y, frame_data[index], frame_data[index + 1],
                       frame_data[index + 2]);
    }
  }
}

// Shared helper: waits while brightness is zero, then redraws the frame.
// Returns true if the display should stop (shutdown or callback).
static bool WaitWhileDark(
    AppState &state, RGBMatrix *matrix, FrameCanvas *&canvas,
    const uint8_t *frame_data, int width, int height,
    const std::chrono::steady_clock::time_point &start_time, int dwell_secs,
    const std::function<bool()> &stop_callback) {
  canvas->Clear();
  matrix->SwapOnVSync(canvas);
  while (std::chrono::steady_clock::now() - start_time <
             std::chrono::seconds(dwell_secs) &&
         state.brightness.load() == 0 && state.running && !stop_callback()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!state.running || stop_callback()) {
    return true;
  }
  matrix->SetBrightness(state.brightness.load());
  DrawFrame(canvas, frame_data, width, height);
  canvas = matrix->SwapOnVSync(canvas);
  return false;
}

// Shared helper: checks for brightness change and redraws if needed.
// Returns true if a redraw was performed.
static bool HandleBrightnessRedraw(AppState &state, RGBMatrix *matrix,
                                   FrameCanvas *&canvas,
                                   const uint8_t *frame_data, int width,
                                   int height) {
  if (state.redraw_frame.exchange(false)) {
    Log(state, "Redrawing frame for brightness change");
    matrix->SetBrightness(state.brightness.load());
    DrawFrame(canvas, frame_data, width, height);
    canvas = matrix->SwapOnVSync(canvas);
    return true;
  }
  return false;
}

static void DisplayImage(AppState &state, RGBMatrix *matrix,
                         FrameCanvas *&canvas, const uint8_t *image_data,
                         int width, int height, int dwell_secs,
                         const std::function<bool()> &stop_display_callback) {
  DrawFrame(canvas, image_data, width, height);

  canvas = matrix->SwapOnVSync(canvas);
  Log(state,
      "Showing still image for " + std::to_string(dwell_secs) + " seconds");
  if (!state.running) {
    return;
  }
  if (dwell_secs > 0) {
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time <
           std::chrono::seconds(dwell_secs)) {
      if (stop_display_callback()) {
        break;
      }

      if (state.brightness.load() == 0) {
        if (WaitWhileDark(state, matrix, canvas, image_data, width, height,
                          start_time, dwell_secs, stop_display_callback)) {
          break;
        }
        continue;
      }

      HandleBrightnessRedraw(state, matrix, canvas, image_data, width, height);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

static void DisplayAnimation(
    AppState &state, RGBMatrix *matrix, FrameCanvas *&canvas,
    WebPAnimDecoder *anim_decoder, int width, int height, int dwell_secs,
    const std::function<bool()> &stop_animation_callback) {
  auto start_time = std::chrono::steady_clock::now();
  uint8_t *frame_data;
  uint8_t *last_frame_data = nullptr;
  int timestamp;
  int prev_timestamp = 0;
  Log(state,
      "Showing animation for " + std::to_string(dwell_secs) + " seconds");

  while (!dwell_secs || (std::chrono::steady_clock::now() - start_time <
                         std::chrono::seconds(dwell_secs))) {
    if (stop_animation_callback()) {
      break;
    }

    if (state.brightness.load() == 0) {
      if (last_frame_data) {
        if (WaitWhileDark(state, matrix, canvas, last_frame_data, width, height,
                          start_time, dwell_secs, stop_animation_callback)) {
          break;
        }
      } else {
        canvas->Clear();
        matrix->SwapOnVSync(canvas);
        while (std::chrono::steady_clock::now() - start_time <
                   std::chrono::seconds(dwell_secs) &&
               state.brightness.load() == 0 && state.running &&
               !stop_animation_callback()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
      continue;
    }

    if (last_frame_data) {
      if (HandleBrightnessRedraw(state, matrix, canvas, last_frame_data, width,
                                 height)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
    }

    if (!WebPAnimDecoderGetNext(anim_decoder, &frame_data, &timestamp)) {
      WebPAnimDecoderReset(anim_decoder);
      prev_timestamp = 0;
      last_frame_data = nullptr;
      continue;
    }
    last_frame_data = frame_data;

    DrawFrame(canvas, frame_data, width, height);

    canvas = matrix->SwapOnVSync(canvas);

    if (!state.running) {
      break;
    }

    int delay_ms = timestamp - prev_timestamp;
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    prev_timestamp = timestamp;

    if (!state.running) {
      break;
    }
  }
}

static int usage(const char *progname, const char *msg = NULL) {
  if (msg) {
    std::cerr << msg << std::endl;
  }
  std::cerr << "Fetch images over HTTP and display on RGB-Matrix" << std::endl;
  std::cerr << "usage: " << progname << " <URL>" << std::endl;

  std::cerr << "\nGeneral LED matrix options:" << std::endl;
  PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  AppState state;
  g_app_state = &state;

  RGBMatrix::Options matrix_options;
  matrix_options.rows = 32;
  matrix_options.cols = 64;
  matrix_options.chain_length = 1;
  matrix_options.parallel = 1;
  matrix_options.brightness = INITIAL_BRIGHTNESS;
  matrix_options.hardware_mapping = "regular";

  RuntimeOptions runtime_options;
  runtime_options.gpio_slowdown = 2;
  runtime_options.drop_privileges = true;

  std::vector<char *> new_argv(argv, argv + argc);
  for (auto it = new_argv.begin() + 1; it != new_argv.end();) {
    if (strcmp(*it, "--verbose") == 0) {
      state.verbose = true;
      it = new_argv.erase(it);
    } else {
      ++it;
    }
  }
  argc = new_argv.size();
  argv = new_argv.data();

  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                         &runtime_options)) {
    return usage(argv[0]);
  }

  if (argc != 2) {
    usage(argv[0], "Invalid number of arguments");
    return 1;
  }

  bool use_websocket = false;
  std::string url = argv[1];
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    std::cerr
        << "Invalid URL: Missing scheme (http://, https://, ws://, or wss://)"
        << std::endl;
    return 1;
  }

  std::string scheme = url.substr(0, scheme_end);
  if (scheme == "ws" || scheme == "wss") {
    use_websocket = true;
  } else if (scheme != "http" && scheme != "https") {
    std::cerr << "Invalid URL: Unsupported scheme (" << scheme << ")"
              << std::endl;
    return 1;
  }

  static std::atomic<int> next_dwell_secs(0);
  static std::atomic<int> ws_message_counter(0);
  static std::atomic<int> immediate_requests(0);

  std::unique_ptr<RGBMatrix> matrix(
      CreateMatrixFromOptions(matrix_options, runtime_options));
  if (!matrix) {
    std::cerr << "Failed to initialize RGB matrix" << std::endl;
    return 1;
  }

  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  std::mutex queue_mutex;
  struct ResponseData {
    std::string data;
    int brightness;
    int dwell_secs;
    int counter = 0;
  };
  std::deque<ResponseData> response_queue;
  const size_t max_queue_size = use_websocket ? 3 : 1;
  std::atomic<bool> startup_animation_playing(true);

  // Display the startup image
  ResponseData startup_response = {
      std::string(reinterpret_cast<const char *>(STARTUP_WEBP),
                  STARTUP_WEBP_LEN),
      INITIAL_BRIGHTNESS, use_websocket ? 0 : INITIAL_DWELL_SECS, 0};
  response_queue.push_back(std::move(startup_response));

  std::thread fetch_thread;
  std::optional<ws::Client> ws_client;

  auto add_to_queue = [&](ResponseData response) {
    std::unique_lock<std::mutex> lock(queue_mutex);
    state.queue_not_full.wait(lock, [&]() {
      return response_queue.size() < max_queue_size || !state.running;
    });
    if (!state.running) {
      return;
    }
    if (use_websocket) {
      ws_message_counter++;
      response.counter = ws_message_counter.load();
      json::Value queued_msg;
      queued_msg["queued"] = response.counter;
      Log(state, "Queued message: " + queued_msg.dump());
      ws_client->send(queued_msg.dump());
    }
    response_queue.push_back(std::move(response));
    state.queue_not_empty.notify_one();
  };

  if (use_websocket) {
    ws_client.emplace();
    ws_client->setUrl(url);
    ws_client->enableAutomaticReconnection();
    ws_client->setOnMessageCallback(
        [&](const ws::Message &msg) {
          if (!state.running) {
            return;
          }
          if (msg.type == ws::MessageType::Open) {
            Log(state, "WebSocket connection established");
            json::Value client_info;
            client_info["firmware_version"] = FIRMWARE_VERSION;
            client_info["firmware_type"] = FIRMWARE_TYPE;
            client_info["protocol_version"] = PROTOCOL_VERSION;
            client_info["mac"] = get_mac_address();
            client_info["hostname"] = get_hostname();
            client_info["image_url"] = url;
            json::Value client_info_msg;
            client_info_msg["client_info"] = std::move(client_info);
            Log(state, "Sending client info: " + client_info_msg.dump());
            ws_client->send(client_info_msg.dump());
          } else if (msg.type == ws::MessageType::Message) {
            if (msg.binary) {
              Log(state, "Received image of size " +
                             std::to_string(msg.data.size()) + " bytes");
              ResponseData response = {
                  msg.data, -1, next_dwell_secs.load(), 0};
              add_to_queue(std::move(response));
            } else {
              auto json_message = json::Value::parse(msg.data);
              if (json_message.is_discarded()) {
                std::cerr << "JSON parsing error: Invalid JSON format"
                          << std::endl;
                return;
              }
              Log(state, "Received JSON message: " + msg.data);

              if (json_message.contains("brightness") &&
                  json_message["brightness"].is_number_integer()) {
                int new_brightness = json_message["brightness"].get<int>();
                if (new_brightness < 0 || new_brightness > 100) {
                  std::cerr << "Invalid brightness value: " << new_brightness
                            << std::endl;
                  return;
                }
                if (new_brightness != state.brightness.load()) {
                  Log(state, "Setting brightness to " +
                                 std::to_string(new_brightness));
                  state.brightness.store(new_brightness);
                  if (new_brightness > 0) {
                    state.redraw_frame.store(true);
                  }
                }
              } else if (json_message.contains("dwell_secs") &&
                         json_message["dwell_secs"].is_number_integer()) {
                next_dwell_secs.store(
                    json_message["dwell_secs"].get<int>());
              } else if (json_message.contains("immediate") &&
                         json_message["immediate"].is_boolean()) {
                if (json_message["immediate"].get<bool>()) {
                  Log(state, "Received immediate display request");
                  {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    while (response_queue.size() > 1) {
                      response_queue.pop_front();
                    }
                  }
                  state.queue_not_full.notify_all();
                  immediate_requests++;
                }
              } else if (json_message.contains("reboot") &&
                         json_message["reboot"].is_boolean()) {
                if (json_message["reboot"].get<bool>()) {
                  Log(state, "Reboot requested via WebSocket");
                  std::cerr << "Rebooting..." << std::endl;
                  sync();
                  reboot(RB_AUTOBOOT);
                }
              } else if (json_message.contains("status") &&
                         json_message["status"].is_string() &&
                         json_message.contains("message") &&
                         json_message["message"].is_string()) {
                std::cerr << json_message["status"].get<std::string>() << ": "
                          << json_message["message"].get<std::string>()
                          << std::endl;
              } else {
                std::cerr << "Invalid JSON message format: " << msg.data
                          << std::endl;
              }
            }
          } else if (msg.type == ws::MessageType::Error) {
            std::cerr << "WebSocket error: " << msg.error_reason
                      << std::endl;
          } else if (msg.type == ws::MessageType::Close) {
            std::cerr << "WebSocket closed: " << msg.close_reason
                      << std::endl;
          }
        });
    ws_client->start();
  } else {
    // Find the start of the path after the scheme
    auto path_start = url.find('/', scheme_end + 3);
    auto base_url = url.substr(
        0, path_start != std::string::npos ? path_start : url.length());
    auto client = std::make_shared<http::Client>(base_url);
    if (!client->is_valid()) {
      std::cerr << "Invalid URL: Unable to create client" << std::endl;
      return 1;
    }
    client->set_default_headers(
        {{"User-Agent", "Tronberry/1.0"},
         {"Accept", "image/webp, image/*;q=0.8, */*;q=0.5"},
         {"X-Firmware-Version", FIRMWARE_VERSION}});
    client->set_connection_timeout(30);
    client->set_read_timeout(30);
    auto path = path_start != std::string::npos
                    ? url.substr(path_start)
                    : "/";  // Extract path or default to "/"

    fetch_thread = std::thread([&, client, path]() mutable {
      int retry_count = 0;
      while (state.running) {
        auto res = client->Get(path.c_str());
        if (!res || res->status != 200) {
          std::cerr << "Failed to fetch image from URL: " << url << std::endl;
          // Exponential backoff capped at 60 seconds
          int wait_time =
              std::min(1 << std::min(retry_count, 6), 60);
          wait_time = std::max(wait_time, 1);
          // Sleep in small increments so we can respond to shutdown
          auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(wait_time);
          while (state.running &&
                 std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
          }
          retry_count++;
          continue;
        }
        retry_count = 0;  // Reset retry_count on success

        ResponseData response;
        response.data = std::move(res->body);
        auto brightness_str = res->get_header_value("Tronbyt-Brightness", "0");
        auto dwell_secs_str = res->get_header_value("Tronbyt-Dwell-Secs", "0");

        char *end_ptr = nullptr;
        response.brightness =
            std::strtol(brightness_str.c_str(), &end_ptr, 10);
        if (*end_ptr != '\0' || response.brightness < 0 ||
            response.brightness > 100) {
          std::cerr << "Invalid brightness header value: " << brightness_str
                    << std::endl;
          response.brightness = 0;
        }

        end_ptr = nullptr;
        response.dwell_secs =
            std::strtol(dwell_secs_str.c_str(), &end_ptr, 10);
        if (*end_ptr != '\0') {
          std::cerr << "Invalid dwell_secs header value: " << dwell_secs_str
                    << std::endl;
          response.dwell_secs = 0;
        }

        add_to_queue(std::move(response));
      }
    });
  }

  while (state.running) {
    ResponseData response;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      state.queue_not_empty.wait(
          lock, [&]() { return !response_queue.empty() || !state.running; });
      if (!state.running) {
        break;
      }

      response = std::move(response_queue.front());
      response_queue.pop_front();
    }
    state.queue_not_full.notify_one();

    if (use_websocket && response.counter > 0) {
      json::Value displaying_msg;
      displaying_msg["displaying"] = response.counter;
      Log(state, "Displaying message: " + displaying_msg.dump());
      ws_client->send(displaying_msg.dump());
    }

    if (response.brightness != -1 &&
        response.brightness != state.brightness.load()) {
      Log(state,
          "Setting brightness to " + std::to_string(response.brightness));
      state.brightness.store(response.brightness);
      if (state.brightness.load() > 0) {
        state.redraw_frame.store(true);
      }
    }

    matrix->SetBrightness(state.brightness.load());

    if (response.data.empty()) {
      continue;
    }

    WebPData webp_data = {
        reinterpret_cast<const uint8_t *>(response.data.data()),
        response.data.size()};
    WebPAnimDecoderOptions anim_options;
    WebPAnimDecoderOptionsInit(&anim_options);

    auto anim_deleter = [](WebPAnimDecoder *d) {
      if (d) WebPAnimDecoderDelete(d);
    };
    std::unique_ptr<WebPAnimDecoder, decltype(anim_deleter)> anim_decoder(
        WebPAnimDecoderNew(&webp_data, &anim_options), anim_deleter);

    const int immediate_requests_at_start = immediate_requests.load();

    auto stop_display_callback = [&]() {
      if (immediate_requests.load() > immediate_requests_at_start) {
        return true;
      }
      if (startup_animation_playing.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        return !response_queue.empty() || !state.running;
      }
      return !state.running;
    };

    if (anim_decoder) {
      WebPAnimInfo anim_info;
      WebPAnimDecoderGetInfo(anim_decoder.get(), &anim_info);
      int dwell_secs = response.dwell_secs;
      if (anim_info.frame_count > 1) {
        DisplayAnimation(state, matrix.get(), offscreen_canvas, anim_decoder.get(),
                         anim_info.canvas_width, anim_info.canvas_height,
                         dwell_secs, stop_display_callback);
      } else {
        uint8_t *frame_data;
        int timestamp;
        if (dwell_secs == 0) {
          dwell_secs = INITIAL_DWELL_SECS;
        }
        if (!WebPAnimDecoderGetNext(anim_decoder.get(), &frame_data,
                                    &timestamp)) {
          std::cerr << "Failed to decode first animation frame" << std::endl;
        } else {
          DisplayImage(state, matrix.get(), offscreen_canvas, frame_data,
                       anim_info.canvas_width, anim_info.canvas_height,
                       dwell_secs, stop_display_callback);
        }
      }
    } else {
      int width, height;
      uint8_t *image_data =
          WebPDecodeRGBA(webp_data.bytes, webp_data.size, &width, &height);
      if (image_data) {
        int dwell_secs = response.dwell_secs;
        if (dwell_secs == 0) {
          dwell_secs = INITIAL_DWELL_SECS;
        }
        DisplayImage(state, matrix.get(), offscreen_canvas, image_data, width, height,
                     dwell_secs, stop_display_callback);
        WebPFree(image_data);
      } else {
        std::cerr << "Failed to decode WebP image" << std::endl;
      }
    }
    startup_animation_playing.store(false);
  }

  Log(state, "Shutting down...");
  if (ws_client) {
    ws_client->stop();
  } else {
    fetch_thread.join();
  }

  g_app_state = nullptr;
  return 0;
}