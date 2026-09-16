#pragma once
#include <cstdint>

void requestDisplayInit();
void requestDisplayRecord(const char *ip);
bool requestDisplayHealthy();
uint32_t requestDisplayUpdates();
