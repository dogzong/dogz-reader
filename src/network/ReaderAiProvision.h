#pragma once
#include <Arduino.h>
namespace readerai {
void provisionUsb(const String& json);
void provisionWifiUsb(const String& json);
void testUsbConnection();
}  // namespace readerai
