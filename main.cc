#include <webp/decode.h>
#include <webp/demux.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <format>
#include <iostream>
#include <queue>
#include <string>
#include <thread>

#include "httplib.h"
#include "ixwebsocket/IXWebSocket.h"
#include "json.hpp"
#include "led-matrix.h"
#include "startup.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

#define FIRMWARE_TYPE "Tronberry"
#define PROTOCOL_VERSION 1

using namespace rgb_matrix;

static std::atomic<bool> running(true);
static std::atomic<int> brightness(INITIAL_BRIGHTNESS);
static std::atomic<bool> redraw_frame(false);
static bool verbose = false;
static std::condition_variable queue_not_full;
static std::condition_variable queue_not_empty;

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

static void Log(const std::string &message) {
  if (!verbose) {
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
  running = false;
  queue_not_empty.notify_all();
  queue_not_full.notify_all();
}

static void DrawFrame(FrameCanvas *canvas, const uint8_t *frame_data, int width,
                      int height) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int index = (y * width + x) * 4;  // RGBA
      canvas->SetPixel(x, y, frame_data[index], frame_data[index + 1],
                       frame_data[index + 2]);
    }
  }
}

static void DisplayImage(RGBMatrix *matrix, FrameCanvas *&canvas,
                         const uint8_t *image_data, int width, int height,
                         int dwell_secs,
                         const std::function<bool()> &stop_display_callback) {
  DrawFrame(canvas, image_data, width, height);

  canvas = matrix->SwapOnVSync(canvas);
  Log("Showing still image for " + std::to_string(dwell_secs) + " seconds");
  if (!running) {
    return;
  }
  if (dwell_secs > 0) {
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time <
           std::chrono::seconds(dwell_secs)) {
      if (stop_display_callback()) {
        break;
      }

      if (brightness.load() == 0) {
        canvas->Clear();
        matrix->SwapOnVSync(canvas);
        while (std::chrono::steady_clock::now() - start_time <
                   std::chrono::seconds(dwell_secs) &&
               brightness.load() == 0 && running && !stop_display_callback()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Force a redraw with the new brightness.
        matrix->SetBrightness(brightness.load());
        DrawFrame(canvas, image_data, width, height);
        canvas = matrix->SwapOnVSync(canvas);
        continue;
      }

      if (redraw_frame.exchange(false)) {
        Log("Redrawing frame for brightness change");
        matrix->SetBrightness(brightness.load());
        DrawFrame(canvas, image_data, width, height);
        canvas = matrix->SwapOnVSync(canvas);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

static void DisplayAnimation(
    RGBMatrix *matrix, FrameCanvas *&canvas, WebPAnimDecoder *anim_decoder,
    int width, int height, int dwell_secs,
    const std::function<bool()> &stop_animation_callback) {
  auto start_time = std::chrono::steady_clock::now();
  uint8_t *frame_data;
  uint8_t *last_frame_data = nullptr;
  int timestamp;
  int prev_timestamp = 0;
  Log("Showing animation for " + std::to_string(dwell_secs) + " seconds");

  while (!dwell_secs || (std::chrono::steady_clock::now() - start_time <
                         std::chrono::seconds(dwell_secs))) {
    if (stop_animation_callback()) {
      break;
    }

    if (brightness.load() == 0) {
      canvas->Clear();
      matrix->SwapOnVSync(canvas);
      while (std::chrono::steady_clock::now() - start_time <
                 std::chrono::seconds(dwell_secs) &&
             brightness.load() == 0 && running && !stop_animation_callback()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (last_frame_data) {
        matrix->SetBrightness(brightness.load());
        DrawFrame(canvas, last_frame_data, width, height);
        canvas = matrix->SwapOnVSync(canvas);
      }
      continue;
    }

    if (redraw_frame.exchange(false)) {
      if (last_frame_data) {
        Log("Redrawing animation frame for brightness change");
        matrix->SetBrightness(brightness.load());
        DrawFrame(canvas, last_frame_data, width, height);
        canvas = matrix->SwapOnVSync(canvas);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
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

    if (!running) {
      break;
    }

    int delay_ms = timestamp - prev_timestamp;
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    prev_timestamp = timestamp;

    if (!running) {
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
      verbose = true;
      it = new_argv.erase(it);
    } else {
      ++it;
    }
  }
  argc = new_argv.size();
  argv = new_argv.data();

  if (!ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_options)) {
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

  RGBMatrix *matrix = CreateMatrixFromOptions(matrix_options, runtime_options);
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
  ix::WebSocket ws_client;

  auto add_to_queue = [&](ResponseData response) {
    std::unique_lock<std::mutex> lock(queue_mutex);
    queue_not_full.wait(lock, [&]() {
      return response_queue.size() < max_queue_size || !running;
    });
    if (!running) {
      return;
    }
    if (use_websocket) {
      ws_message_counter++;
      response.counter = ws_message_counter.load();
      nlohmann::json queued_msg;
      queued_msg["queued"] = response.counter;
      Log("Queued message: " + queued_msg.dump());
      ws_client.send(queued_msg.dump());
    }
    response_queue.push_back(std::move(response));
    queue_not_empty.notify_one();
  };

  if (use_websocket) {
    ws_client.setUrl(url);
    ws_client.enableAutomaticReconnection();
    ws_client.setOnMessageCallback(
        [&, matrix, offscreen_canvas](const ix::WebSocketMessagePtr &msg) {
          if (!running) {
            return;
          }
          if (msg->type == ix::WebSocketMessageType::Open) {
            Log("WebSocket connection established");
            const nlohmann::json client_info_msg = {
                {"client_info", {
                    {"firmware_version", FIRMWARE_VERSION},
                    {"firmware_type", FIRMWARE_TYPE},
                    {"protocol_version", PROTOCOL_VERSION},
                    {"mac", get_mac_address()}
                }}
            };
            Log("Sending client info: " + client_info_msg.dump());
            ws_client.send(client_info_msg.dump());
          } else if (msg->type == ix::WebSocketMessageType::Message) {
            if (msg->binary) {
              Log("Received image of size " +
                  std::to_string(msg->str.size()) + " bytes");
              ResponseData response = {msg->str, -1, next_dwell_secs.load(), 0};
              add_to_queue(std::move(response));
            } else {
              auto json_message =
                  nlohmann::json::parse(msg->str, nullptr, false);
              if (json_message.is_discarded()) {
                std::cerr << "JSON parsing error: Invalid JSON format"
                          << std::endl;
                return;
              }
              Log("Received JSON message: " + msg->str);

              if (json_message.contains("brightness") &&
                  json_message["brightness"].is_number_integer()) {
                int new_brightness = json_message["brightness"].get<int>();
                if (new_brightness < 0 || new_brightness > 100) {
                  std::cerr << "Invalid brightness value: " << new_brightness
                            << std::endl;
                  return;
                }
                if (new_brightness != brightness.load()) {
                  Log("Setting brightness to " +
                      std::to_string(new_brightness));
                  brightness.store(new_brightness);
                  if (new_brightness > 0) {
                    redraw_frame.store(true);
                  }
                }
              } else if (json_message.contains("dwell_secs") &&
                         json_message["dwell_secs"].is_number_integer()) {
                next_dwell_secs.store(
                    json_message["dwell_secs"].get<int>());
              } else if (json_message.contains("immediate") &&
                         json_message["immediate"].is_boolean()) {
                if (json_message["immediate"].get<bool>()) {
                  Log("Received immediate display request");
                  {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    while (response_queue.size() > 1) {
                      response_queue.pop_front();
                    }
                  }
                  immediate_requests++;
                }
              } else if (json_message.contains("status") &&
                         json_message["status"].is_string() &&
                         json_message.contains("message") &&
                         json_message["message"].is_string()) {
                std::cerr << json_message["status"].get<std::string>() << ": "
                          << json_message["message"].get<std::string>()
                          << std::endl;
              } else {
                std::cerr << "Invalid JSON message format: " << msg->str
                          << std::endl;
              }
            }
          } else if (msg->type == ix::WebSocketMessageType::Error) {
            std::cerr << "WebSocket error: " << msg->errorInfo.reason
                      << std::endl;
          } else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cerr << "WebSocket closed: " << msg->closeInfo.reason
                      << std::endl;
          }
        });
    ws_client.start();
  } else {
    // Find the start of the path after the scheme
    auto path_start = url.find('/', scheme_end + 3);
    auto base_url = url.substr(
        0, path_start != std::string::npos ? path_start : url.length());
    auto client = std::make_shared<httplib::Client>(base_url);
    if (!client->is_valid()) {
      std::cerr << "Invalid URL: Unable to create client" << std::endl;
      return 1;
    }
    client->set_default_headers(
        {{"User-Agent", "Tronberry/1.0"},
         {"Accept", "image/webp, image/*;q=0.8, */*;q=0.5"}});
    auto path = path_start != std::string::npos
                    ? url.substr(path_start)
                    : "/";  // Extract path or default to "/"

    fetch_thread = std::thread([&, client, path]() mutable {
      int retry_count = 0;
      while (running) {
        auto res = client->Get(path.c_str());
        if (!res || res->status != 200) {
          std::cerr << "Failed to fetch image from URL: " << url << std::endl;
          int wait_time = std::min(
              1 << retry_count,
              60);  // Exponential backoff with max wait time of 60 seconds
          wait_time =
              std::max(wait_time, 1);  // Ensure at least 1 second wait time
          std::this_thread::sleep_for(std::chrono::seconds(wait_time));
          retry_count++;
          continue;
        }
        retry_count = 0;  // Reset retry_count on success

        ResponseData response;
        response.data = std::move(res->body);
        auto brightness_str = res->get_header_value("Tronbyt-Brightness", "0");
        auto dwell_secs_str = res->get_header_value("Tronbyt-Dwell-Secs", "0");

        char *end_ptr = nullptr;
        response.brightness = std::strtol(brightness_str.c_str(), &end_ptr, 10);
        if (*end_ptr != '\0' || response.brightness < 0 ||
            response.brightness > 100) {
          std::cerr << "Invalid brightness header value: " << brightness_str
                    << std::endl;
          response.brightness = 0;
        }

        end_ptr = nullptr;
        response.dwell_secs = std::strtol(dwell_secs_str.c_str(), &end_ptr, 10);
        if (*end_ptr != '\0') {
          std::cerr << "Invalid dwell_secs header value: " << dwell_secs_str
                    << std::endl;
          response.dwell_secs = 0;
        }

        add_to_queue(std::move(response));
      }
    });
  }

  while (running) {
    ResponseData response;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_not_empty.wait(
          lock, [&]() { return !response_queue.empty() || !running; });
      if (!running) {
        break;
      }

      response = std::move(response_queue.front());
      response_queue.pop_front();
    }
    queue_not_full.notify_one();

    if (use_websocket && response.counter > 0) {
      nlohmann::json displaying_msg;
      displaying_msg["displaying"] = response.counter;
      Log("Displaying message: " + displaying_msg.dump());
      ws_client.send(displaying_msg.dump());
    }

    if (response.brightness != -1 &&
        response.brightness != brightness.load()) {
      Log("Setting brightness to " + std::to_string(response.brightness));
      brightness.store(response.brightness);
      if (brightness.load() > 0) {
        redraw_frame.store(true);
      }
    }

    matrix->SetBrightness(brightness.load());

    if (response.data.empty()) {
      continue;
    }

    WebPData webp_data = {
        reinterpret_cast<const uint8_t *>(response.data.data()),
        response.data.size()};
    WebPAnimDecoderOptions anim_options;
    WebPAnimDecoderOptionsInit(&anim_options);
    WebPAnimDecoder *anim_decoder =
        WebPAnimDecoderNew(&webp_data, &anim_options);

    const int immediate_requests_at_start = immediate_requests.load();

    auto stop_display_callback = [&]() {
      if (immediate_requests.load() > immediate_requests_at_start) {
        return true;
      }
      if (startup_animation_playing.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        return !response_queue.empty() || !running;
      }
      return !running;
    };

    if (anim_decoder) {
      WebPAnimInfo anim_info;
      WebPAnimDecoderGetInfo(anim_decoder, &anim_info);
      int dwell_secs = response.dwell_secs;
      if (anim_info.frame_count > 1) {
        DisplayAnimation(matrix, offscreen_canvas, anim_decoder,
                         anim_info.canvas_width, anim_info.canvas_height,
                         dwell_secs, stop_display_callback);
      } else {
        uint8_t *frame_data;
        int timestamp;
        if (dwell_secs == 0) {
          dwell_secs = INITIAL_DWELL_SECS;
        }
        WebPAnimDecoderGetNext(anim_decoder, &frame_data, &timestamp);
        DisplayImage(matrix, offscreen_canvas, frame_data,
                     anim_info.canvas_width, anim_info.canvas_height,
                     dwell_secs, stop_display_callback);
      }

      WebPAnimDecoderDelete(anim_decoder);
    } else {
      int width, height;
      uint8_t *image_data =
          WebPDecodeRGBA(webp_data.bytes, webp_data.size, &width, &height);
      if (image_data) {
        int dwell_secs = response.dwell_secs;
        if (dwell_secs == 0) {
          dwell_secs = INITIAL_DWELL_SECS;
        }
        DisplayImage(matrix, offscreen_canvas, image_data, width, height,
                     dwell_secs, stop_display_callback);
        WebPFree(image_data);
      } else {
        std::cerr << "Failed to decode WebP image" << std::endl;
      }
    }
    startup_animation_playing.store(false);
  }

  Log("Shutting down...");
  if (use_websocket) {
    ws_client.stop();
  } else {
    fetch_thread.join();
  }

  delete matrix;
  return 0;
}