#pragma once

#include "freertos/FreeRTOS.h"

// The serial connection to the computer: UART0 through the CYD's USB bridge, or the
// ESP32-S3's native USB.
void link_init();
// Reads up to size bytes; returns 0 if nothing arrives within wait ticks. Sets lost when
// received bytes were dropped, so a partial record is not mistaken for a whole one.
int link_read(char* bytes, int size, TickType_t wait, bool& lost);
void link_write(const char* bytes, int count);
