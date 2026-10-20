#include "sound.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>

static std::unique_ptr<SoundPlayer> g_sound_player;

SoundPlayer::SoundPlayer(LogCallback log_cb)
    : log_cb_(std::move(log_cb)),
      worker_([this]() { WorkerLoop(); }) {}

SoundPlayer::~SoundPlayer() {
  Stop();
}

void SoundPlayer::SetLogger(LogCallback log_cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_cb_ = std::move(log_cb);
}

void SoundPlayer::Play(const std::string &sound_url, int volume) {
  if (sound_url.empty() || stopped_) {
    return;
  }
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_cb_) {
      log_cb_("Queueing sound: " + sound_url + " (vol " + std::to_string(volume) + "%)");
    }
    if (queue_.size() >= 2) {
      queue_.pop();
    }
    queue_.push({sound_url, volume});
  }
  cv_.notify_one();
}

void SoundPlayer::Stop() {
  bool expected = false;
  if (!stopped_.compare_exchange_strong(expected, true)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) queue_.pop();
  }
  cv_.notify_all();
  pid_t pid = current_pid_.load();
  if (pid > 0) {
    kill(pid, SIGTERM);
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void SoundPlayer::WorkerLoop() {
  while (!stopped_) {
    SoundRequest req;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
      if (stopped_) {
        break;
      }
      req = std::move(queue_.front());
      queue_.pop();
    }

    pid_t pid = fork();
    if (pid < 0) {
      std::cerr << "Failed to fork for sound playback" << std::endl;
      continue;
    }
    if (pid == 0) {
      // Child process: redirect stdio to /dev/null
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
          close(devnull);
        }
      }
      std::string mpv_vol = "--volume=" + std::to_string(req.volume);
      std::string ffplay_vol = std::to_string(req.volume);
      execlp("mpv", "mpv", "--ao=alsa", "--no-video", "--really-quiet",
             mpv_vol.c_str(), req.url.c_str(), static_cast<char *>(nullptr));
      execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
             "-volume", ffplay_vol.c_str(), req.url.c_str(),
             static_cast<char *>(nullptr));
      execlp("aplay", "aplay", "-q", req.url.c_str(),
             static_cast<char *>(nullptr));
      _exit(127);
    }

    current_pid_ = pid;
    int status = 0;
    waitpid(pid, &status, 0);
    current_pid_ = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
      std::cerr << "Sound player not found (install mpv or ffplay)" << std::endl;
    }
  }
}

void InitSoundPlayer(SoundPlayer::LogCallback log_cb) {
  if (!g_sound_player) {
    g_sound_player = std::make_unique<SoundPlayer>(std::move(log_cb));
  } else if (log_cb) {
    g_sound_player->SetLogger(std::move(log_cb));
  }
}

void PlaySound(const std::string &sound_url, int volume) {
  if (g_sound_player) {
    g_sound_player->Play(sound_url, volume);
  }
}

void StopSoundPlayer() {
  if (g_sound_player) {
    g_sound_player->Stop();
  }
}

void ShutdownSoundPlayer() {
  if (g_sound_player) {
    g_sound_player->Stop();
    g_sound_player.reset();
  }
}

SoundPlayer *GetSoundPlayer() {
  return g_sound_player.get();
}
