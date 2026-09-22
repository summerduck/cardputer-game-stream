// Wi-Fi book upload: the Cardputer opens its own Wi-Fi network with a small upload page.
#pragma once
#include <Arduino.h>

namespace upload {

constexpr const char *SSID = "Cardputer-Books";

void start();
void stop();
void loop();
// Name of the last finished upload (empty if none since the last call).
String takeLastUploaded();
int clientCount();

}  // namespace upload
