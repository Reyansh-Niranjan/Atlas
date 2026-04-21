#pragma once

#include <Arduino.h>

namespace LiveMicStt {

bool begin(String &outError);
bool transcribeWindow(String &outText, String &outError);

} // namespace LiveMicStt
