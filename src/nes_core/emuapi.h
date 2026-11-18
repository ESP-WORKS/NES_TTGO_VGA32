#pragma once
/* C bridge - Nofrendo originally used a TFT emulator API. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *emu_Malloc(int size);
void *emu_LineBuffer(int line);
void emu_DrawLine(unsigned char* pixels, int width, int height, int line);
void emu_printf(const char* msg);
int emu_FileOpen(const char* path);
int emu_FileRead(void* dst, int count);
void emu_FileClose(void);
#ifdef __cplusplus
}
#endif
