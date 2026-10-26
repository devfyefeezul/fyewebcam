// USB mode: forwards a local TCP port to the phone with `adb forward`.
#pragma once

#include <string>

// Runs `adb forward tcp:<port> tcp:<port>`. On failure returns false and sets |error|.
bool AdbForward(int port, std::wstring* error);
