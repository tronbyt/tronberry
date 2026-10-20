// Tronbyt xscreensaver boot screen!
#include "startup.h"
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

std::string GetStartupSoundPath() {
  const std::string shm_path = "/dev/shm/tronberry_startup.wav";
  const std::string tmp_path = "/tmp/tronberry_startup.wav";
  std::string target = (access("/dev/shm", W_OK) == 0) ? shm_path : tmp_path;

  struct stat st;
  if (stat(target.c_str(), &st) != 0 || static_cast<size_t>(st.st_size) != STARTUP_WAV_LEN) {
    std::ofstream out(target, std::ios::binary);
    if (!out.is_open()) {
      return "";
    }
    out.write(reinterpret_cast<const char *>(STARTUP_WAV), STARTUP_WAV_LEN);
  }
  return target;
}
