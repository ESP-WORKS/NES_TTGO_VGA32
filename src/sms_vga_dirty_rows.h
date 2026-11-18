#ifndef SMS_VGA_DIRTY_ROWS_H
#define SMS_VGA_DIRTY_ROWS_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>

// Copiar somente faixas alteradas da PSRAM para o framebuffer DMA.
// Funcao independente do ESP32 para poder validar a semantica em host.
static inline size_t sms_copy_dirty_rows(uint8_t* dest, const uint8_t* src,
                                         uint8_t* dirty, int height, int pitch) {
    size_t bytes = 0;
    for (int y = 0; y < height;) {
        if (!dirty[y]) { ++y; continue; }
        const int first = y;
        while (y < height && dirty[y]) ++y;
        const size_t offset = (size_t)first * pitch;
        const size_t len = (size_t)(y - first) * pitch;
        memcpy(dest + offset, src + offset, len);
        memset(dirty + first, 0, (size_t)(y - first));
        bytes += len;
    }
    return bytes;
}
#endif
