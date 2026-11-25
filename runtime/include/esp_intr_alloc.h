#pragma once

// The flags ESP-IDF code passes when it asks for an interrupt. Nothing here allocates one:
// the host has no interrupt controller, so the flags are accepted and ignored.
#define ESP_INTR_FLAG_LEVEL1 (1 << 1)
#define ESP_INTR_FLAG_LEVEL2 (1 << 2)
#define ESP_INTR_FLAG_LEVEL3 (1 << 3)
#define ESP_INTR_FLAG_LEVEL4 (1 << 4)
#define ESP_INTR_FLAG_LEVEL5 (1 << 5)
#define ESP_INTR_FLAG_LEVEL6 (1 << 6)
#define ESP_INTR_FLAG_NMI (1 << 7)
#define ESP_INTR_FLAG_SHARED (1 << 8)
#define ESP_INTR_FLAG_EDGE (1 << 9)
#define ESP_INTR_FLAG_IRAM (1 << 10)
#define ESP_INTR_FLAG_INTRDISABLED (1 << 11)
#define ESP_INTR_FLAG_LOWMED (ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_LEVEL3)
#define ESP_INTR_FLAG_HIGH \
  (ESP_INTR_FLAG_LEVEL4 | ESP_INTR_FLAG_LEVEL5 | ESP_INTR_FLAG_LEVEL6 | ESP_INTR_FLAG_NMI)
#define ESP_INTR_FLAG_DEFAULT 0
