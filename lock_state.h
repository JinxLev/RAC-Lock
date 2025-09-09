#pragma once
#include <stdint.h>


enum class Mode : uint8_t { LOCKED, UNLOCKED, UNLOCKING, LOCKING, ERROR_MODE };
const char* modeName(Mode m);