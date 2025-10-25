#include <webp/decode.h>
#include <webp/demux.h>

#include <chrono>
#include <iostream>
#include <queue>
#include <string>
#include <thread>

#include "httplib.h"
#include "ixwebsocket/IXWebSocket.h"
#include "json.hpp"
#include "led-matrix.h"
#include "startup.h"

using namespace rgb_matrix;

static std::atomic<bool> running(true);
static std::atomic<int> brightness(INITIAL_BRIGHTNESS);
static std::condition_variable queue_not_full;
static std::condition_variable queue_not_empty;

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
                         int dwell_secs) {
  DrawFrame(canvas, image_data, width, height);

  canvas = matrix->SwapOnVSync(canvas);
  if (!running) {
    return;
  }
  if (dwell_secs > 0) {
    std::this_thread::sleep_for(std::chrono::seconds(dwell_secs));
  }
}

static void DisplayAnimation(
    RGBMatrix *matrix, FrameCanvas *&canvas, WebPAnimDecoder *anim_decoder,
    int width, int height, int dwell_secs,
    const std::function<bool()> &stop_animation_callback) {
  auto start_time = std::chrono::steady_clock::now();
  uint8_t *frame_data;
  int timestamp;
  int prev_timestamp = 0;

  while (!dwell_secs || (std::chrono::steady_clock::now() - start_time <
                         std::chrono::seconds(dwell_secs))) {
    if (stop_animation_callback()) {
      break;
    }

    if (!WebPAnimDecoderGetNext(anim_decoder, &frame_data, &timestamp)) {
      WebPAnimDecoderReset(anim_decoder);
      prev_timestamp = 0;
      continue;
    }

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
  static std::atomic<bool> next_immediate(false);
  static std::atomic<int> ws_message_counter(0);

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
    bool immediate = false;
  };
  std::queue<ResponseData> response_queue;
  const size_t max_queue_size = 1;

  // Display the startup image
  ResponseData startup_response = {
      std::string(reinterpret_cast<const char *>(STARTUP_WEBP),
                  STARTUP_WEBP_LEN),
      INITIAL_BRIGHTNESS, use_websocket ? 0 : INITIAL_DWELL_SECS};
  response_queue.push(std::move(startup_response));

  std::thread fetch_thread;
  ix::WebSocket ws_client;

  auto add_to_queue = [&](ResponseData response) {
    std::unique_lock<std::mutex> lock(queue_mutex);
    if (response.immediate && !response_queue.empty()) {
      response_queue.pop();
    }
    queue_not_full.wait(lock, [&]() {
      return response_queue.size() < max_queue_size || !running;
    });
    if (!running) {
      return;
    }
    response_queue.push(std::move(response));
    if (use_websocket) {
      ws_message_counter++;
      nlohmann::json queued_msg;
      queued_msg["queued"] = ws_message_counter.load();
      ws_client.send(queued_msg.dump());
    }
    queue_not_empty.notify_one();
  };

  if (use_websocket) {
    ws_client.setUrl(url);
    ws_client.enableAutomaticReconnection();
    ws_client.setOnMessageCallback(
        [&](const ix::WebSocketMessagePtr &msg) {
          if (!running) {
            return;
          }
          if (msg->type == ix::WebSocketMessageType::Message) {
            if (msg->binary) {
              ResponseData response = {msg->str, brightness.load(),
                                       next_dwell_secs.load(),
                                       next_immediate.load()};
              add_to_queue(std::move(response));
              next_immediate.store(false);
            } else {
              auto json_message =
                  nlohmann::json::parse(msg->str, nullptr, false);
              if (json_message.is_discarded()) {
                std::cerr << "JSON parsing error: Invalid JSON format"
                          << std::endl;
                return;
              }

              if (json_message.contains("brightness") &&
                  json_message["brightness"].is_number_integer()) {
                int new_brightness = json_message["brightness"].get<int>();
                if (new_brightness < 0 || new_brightness > 100) {
                  std::cerr << "Invalid brightness value: " << new_brightness
                            << std::endl;
                  return;
                }
                brightness.store(new_brightness);
              } else if (json_message.contains("dwell_secs") &&
                         json_message["dwell_secs"].is_number_integer()) {
                next_dwell_secs.store(
                    json_message["dwell_secs"].get<int>());
              } else if (json_message.contains("immediate") &&
                         json_message["immediate"].is_boolean()) {
                next_immediate.store(
                    json_message["immediate"].get<bool>());
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
      response_queue.pop();
    }
    queue_not_full.notify_one();

    if (use_websocket) {
      nlohmann::json displaying_msg;
      displaying_msg["displaying"] = ws_message_counter.load();
      ws_client.send(displaying_msg.dump());
    }

    static int previous_brightness = -1;
    if (response.brightness != -1 &&
        response.brightness != previous_brightness) {
      std::cout << "Setting brightness to " << response.brightness << std::endl;
      matrix->SetBrightness(response.brightness);
      previous_brightness = response.brightness;
    }

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

    if (anim_decoder) {
      WebPAnimInfo anim_info;
      WebPAnimDecoderGetInfo(anim_decoder, &anim_info);

      if (anim_info.frame_count > 1) {
        auto stop_animation_callback = [&]() {
          std::unique_lock<std::mutex> lock(queue_mutex);
          return !response_queue.empty() || !running;
        };

        DisplayAnimation(matrix, offscreen_canvas, anim_decoder,
                         anim_info.canvas_width, anim_info.canvas_height,
                         response.dwell_secs, stop_animation_callback);
      } else {
        uint8_t *frame_data;
        int timestamp;
        WebPAnimDecoderGetNext(anim_decoder, &frame_data, &timestamp);
        DisplayImage(matrix, offscreen_canvas, frame_data,
                     anim_info.canvas_width, anim_info.canvas_height,
                     response.dwell_secs);
      }

      WebPAnimDecoderDelete(anim_decoder);
    } else {
      int width, height;
      uint8_t *image_data =
          WebPDecodeRGBA(webp_data.bytes, webp_data.size, &width, &height);
      if (image_data) {
        DisplayImage(matrix, offscreen_canvas, image_data, width, height,
                     response.dwell_secs);
        WebPFree(image_data);
      } else {
        std::cerr << "Failed to decode WebP image" << std::endl;
      }
    }
  }

  std::cout << "Shutting down..." << std::endl;
  if (use_websocket) {
    ws_client.stop();
  } else {
    fetch_thread.join();
  }

  delete matrix;
  return 0;
}