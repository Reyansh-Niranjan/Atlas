#pragma once

#include <Arduino.h>

namespace LiveMicStt {

[[nodiscard]]
bool begin(String &outError);
[[nodiscard]]
bool transcribeWindow(String &outText, String &outError);

} // namespace LiveMicStt
