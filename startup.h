#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#define INITIAL_BRIGHTNESS 30
#define INITIAL_DWELL_SECS 10

extern const uint8_t STARTUP_WEBP[];
extern const size_t STARTUP_WEBP_LEN;

extern const uint8_t STARTUP_WAV[];
extern const size_t STARTUP_WAV_LEN;

// Returns path to temporary startup sound file (/dev/shm or /tmp)
std::string GetStartupSoundPath();
