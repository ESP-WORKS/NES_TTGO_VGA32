/* NES v0.1.8: SMS v0.1.43 selective hot-path placement.
 * Keep the large 6502 dispatch core in flash to avoid IRAM exhaustion.
 * Host builds ignore NES_HOT. Define NES_IRAM_HOT=0 for A/B tests.
 */
#ifndef NES_HOT_H
#define NES_HOT_H
#if defined(ESP_PLATFORM) && defined(NES_IRAM_HOT) && NES_IRAM_HOT
#include "esp_attr.h"
#define NES_HOT IRAM_ATTR
#else
#define NES_HOT
#endif
#endif
