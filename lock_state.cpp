#include "lock_state.h"
const char* modeName(Mode m){
switch (m) {
case Mode::LOCKED: return "LOCKED";
case Mode::UNLOCKED: return "UNLOCKED";
case Mode::UNLOCKING: return "UNLOCKING";
case Mode::LOCKING: return "LOCKING";
case Mode::ERROR_MODE: return "ERROR";
}
return "ERROR";
}