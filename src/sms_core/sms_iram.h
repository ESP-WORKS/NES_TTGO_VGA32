/* v0.1.34: colocacao do caminho quente do emulador na IRAM.
 *
 * O ESP32 executa codigo da flash por um cache de 32 KB compartilhado com
 * constantes da flash e com TODOS os acessos a PSRAM (ROM, stage VGA).
 * Os traces fisicos da v0.1.31 mostraram +57% em BG/PACK so por mudanca
 * de layout, assinatura de conflito de cache. Codigo em IRAM nao usa o
 * cache de instrucoes: custo constante e independente de layout.
 *
 * SMS_HOT: IRAM (se SMS_IRAM_HOT=1 e plataforma ESP-IDF); vazio no host,
 * nos testes locais e com SMS_IRAM_HOT=0 (A/B exato com a v0.1.33).
 */
#ifndef SMS_IRAM_H
#define SMS_IRAM_H
#include "../../config.h"
/* __has_include: o host (testes locais) pode definir ESP_PLATFORM sem ter
 * o ESP-IDF; nesse caso SMS_HOT fica vazio. */
#if SMS_IRAM_HOT && defined(ESP_PLATFORM) && defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#define SMS_HOT IRAM_ATTR
#endif
#endif
#ifndef SMS_HOT
#define SMS_HOT
#endif
#endif
