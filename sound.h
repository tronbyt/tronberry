#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <sys/types.h>

class SoundPlayer {
 public:
  using LogCallback = std::function<void(const std::string &)>;

  explicit SoundPlayer(LogCallback log_cb = nullptr);
  ~SoundPlayer();

  SoundPlayer(const SoundPlayer &) = delete;
  SoundPlayer &operator=(const SoundPlayer &) = delete;

  // Queue a sound for non-blocking playback at specified volume (0-100)
  void Play(const std::string &sound_url, int volume = 100);

  // Stop playback immediately, terminate active subprocess, and drain queue
  void Stop();

  // Set or update logging callback
  void SetLogger(LogCallback log_cb);

 private:
  void WorkerLoop();

  struct SoundRequest {
    std::string url;
    int volume{100};
  };

  std::queue<SoundRequest> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> stopped_{false};
  std::atomic<pid_t> current_pid_{-1};
  LogCallback log_cb_;
  std::thread worker_;
};

// Global player lifecycle and playback helpers
void InitSoundPlayer(SoundPlayer::LogCallback log_cb = nullptr);
void PlaySound(const std::string &sound_url, int volume = 100);
void StopSoundPlayer();
void ShutdownSoundPlayer();
SoundPlayer *GetSoundPlayer();
