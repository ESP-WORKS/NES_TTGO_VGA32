// config.h
#ifndef CONFIG_H
#define CONFIG_H

// ========== SERVICOS NAO UTILIZADOS ==========
// Nao redefinir CONFIG_* vindos de sdkconfig.h do Arduino ESP32.
// O NES nao inicializa WiFi/Bluetooth; alterar estes CONFIG_*
// neste header do aplicativo nao desativa servicos ja compilados no SDK.
// ========== VIDEO OUTPUT ==========
#define USE_VGA

// VGA pins 
#define PIN_RED_LOW     21
#define PIN_RED_HIGH    22
#define PIN_GREEN_LOW   18
#define PIN_GREEN_HIGH  19
#define PIN_BLUE_LOW    4
#define PIN_BLUE_HIGH   5
#define PIN_HSYNC       23
#define PIN_VSYNC       15

// VGA pin configuration macro
#define VGA_PINS { PIN_RED_LOW, PIN_RED_HIGH, PIN_GREEN_LOW, PIN_GREEN_HIGH, PIN_BLUE_LOW, PIN_BLUE_HIGH, PIN_HSYNC, PIN_VSYNC }

// v0.1.22: testes fisicos de Phantasy Star comprovaram regressao com
// LUT compacta. LUT de pares (2 KiB) volta a ser o padrao: duas consultas
// para quatro pixels, com RGB222, XOR2 e sync VGA identicos a v0.1.20.
// 0 = conversor rapido de pares (validado no hardware); 1 = caminho
// compacto experimental da v0.1.21, mantido SOMENTE para testes A/B.
#ifndef SMS_VGA_COMPACT_LUT
#define SMS_VGA_COMPACT_LUT 0
#endif

// v0.1.8: pack four palette-indexed pixels per internal-RAM 32-bit store.
// Set to 0 for A/B FPS comparison with the unmodified v0.1.7 converter.
#ifndef SMS_VGA_CONVERT_WORD32
#define SMS_VGA_CONVERT_WORD32 1
#endif

// v0.1.9: skip blending for fully transparent sprite rows without
// skipping sprite counting, overflow or collision checks. Set to 0 for
// A/B comparisons if an individual SMS/GG title behaves unexpectedly.
#ifndef SMS_SPRITE_EMPTY_ROW_SKIP
#define SMS_SPRITE_EMPTY_ROW_SKIP 1
#endif

// v0.1.20: only in tile-aligned SMS background rows, use proven 4-byte
// aligned word loads/stores instead of dynamic per-word alignment checks.
// The normal renderer remains available (0) for A/B comparisons.
#ifndef SMS_BG_ALIGNED_WORD_FAST
#define SMS_BG_ALIGNED_WORD_FAST 1
#endif

// v0.1.28: F3 alterna ON/OFF em tempo real sem recompilar ou reiniciar.
// O flag abaixo PRECISA permanecer 1 para incluir as duas rotinas no
// mesmo firmware. A tecla F3 altera apenas o byte runtime; o comando
// [SMS AB] e o campo [SMS PERF] BG_FRAC identificam cada janela A/B.
// Configuracao inicial: ON apos boot, persistente nas trocas de ROM.
// v0.1.27: SMS background com scroll horizontal fracionario (R8 mod 8).
// Compõe cada grupo de 8 pixels com dois tiles e grava em 2 DWORDs
// ALINHADOS; preserva H-lock/vertical row, os 32 tiles visiveis e o
// comportamento original de GG (nao entra neste caminho).
// 0 restaura exatamente a composicao fracionaria da v0.1.26.
#ifndef SMS_BG_FRACTIONAL_WORD_FAST
#define SMS_BG_FRACTIONAL_WORD_FAST 1
#endif

// v0.1.20: specialize normal-size, fully-visible 8px sprite rows.
// Clipped/zoomed sprites retain the exact v0.1.19 path.
#ifndef SMS_SPRITE_FULL_ROW_FAST
#define SMS_SPRITE_FULL_ROW_FAST 1
#endif

// v0.1.26: composicao de sprites normais inteiros (8 px, sem zoom
// ou clipping) em 2/3 palavras de 32 bits com prioridade e colisao
// por byte. 0 restaura imediatamente a via da v0.1.25 para A/B.
// Nenhuma mudanca na alocacao RAM, SRAM, ROM0/ROM1, VGA ou audio.
#ifndef SMS_SPRITE_WORD_FAST
#define SMS_SPRITE_WORD_FAST 1
#endif

// v0.1.10: emulacao apresenta as linhas somente quando o quadro termina.
// Marcar a faixa VGA suja uma vez por quadro em lugar de 192 chamadas.
// A opcao 0 restaura a marcacao de cada linha (comparacao A/B).
#ifndef SMS_VGA_BATCH_DIRTY_ROWS
#define SMS_VGA_BATCH_DIRTY_ROWS 1
#endif

// v0.1.12: REVERSAO DA REGRESSAO v0.1.11 (3 jogos testados).
// O hash+memcmp lia PSRAM e elevava CONVERT ~0.9-1.1 ms por quadro.
// Default 0: restaura EXATAMENTE o caminho de copia da v0.1.10.
// Set 1 somente para testes historicos A/B (NAO RECOMENDADO).
#ifndef SMS_VGA_SKIP_UNCHANGED_ROWS
#define SMS_VGA_SKIP_UNCHANGED_ROWS 0
#endif

// v0.1.15: REGRESSAO CONFIRMADA em v0.1.14 (Alex/R-Type/Phantasy).
// Default 0: caminho de 4 pixels validado na v0.1.13.
// A alternativa de 8 pixels permanece disponivel somente para testes A/B;
// no hardware PACK subiu cerca de 0.22 a 0.24 ms por quadro com 1.
#ifndef SMS_VGA_PACK_UNROLL2
#define SMS_VGA_PACK_UNROLL2 0
#endif

// v0.1.14 ETAPA 21B: alternativa de escrita PSRAM em palavras alinhadas.
// Default 0: mantem memcpy da v0.1.13, pois a copia direta e comprovada.
// Ativar 1 SOMENTE num segundo teste isolado (depois do pack A/B).
// Necessita destino 4-byte aligned; demais casos usam memcpy original.
#ifndef SMS_VGA_PSRAM_WRITE32
#define SMS_VGA_PSRAM_WRITE32 0
#endif

// v0.1.15: DESATIVADO para nao perturbar o hot path do Z80.
// 1 coleta opcodes de 1/64 dos quadros, a custo adicional e possivel
// perturbacao de cache/flash. Deixar 0 em benchmarks de FPS.
#ifndef SMS_Z80_OPCODE_PROFILE
#define SMS_Z80_OPCODE_PROFILE 0
#endif

// v0.1.13: evitar repintar o mesmo FPS/EPS/FRAME SKIP em cada
// iteracao: dois stages PSRAM mantem revisoes independentes da tarja.
// 0 restaura o comportamento OSD original para A/B no hardware.
#ifndef SMS_OSD_CACHE_STAGES
#define SMS_OSD_CACHE_STAGES 1
#endif

// v0.1.13: a linha [SMS AUTO] registra custos de quadros renderizados,
// omitidos, espera de sincronizacao e taxa REAL de descarte, sem
// alterar a politica do AUTO. O perfil de Z80 usa amostragem 1/64.
#ifndef SMS_AUTO_DIAGNOSTICS
#define SMS_AUTO_DIAGNOSTICS 1
#endif

// v0.1.17 (SOMENTE FONTES): mapper + amostragem esparsa do PC.
// Nao adiciona nenhuma condicao por leitura de opcode/operando Z80.
// Contadores precisos apenas para mapeamentos e recargas (memcpy).
// Histograma de PC: 1 quadro/64, 262 amostras, NAO leituras reais.
// 0 elimina toda a coleta, para A/B de FPS no hardware.
#ifndef SMS_ROM_CACHE_DIAGNOSTICS
#define SMS_ROM_CACHE_DIAGNOSTICS 1
#endif

// v0.1.24: o espelho descartavel de escritas nos enderecos ROM
// (sms.dummy, 8KiB) nao guarda RAM real nem VRAM. Para cartuchos em
// PSRAM, mover o buffer FRIO para PSRAM ANTES dos caches de ROM.
// Libera ate 8KiB internos para a metade LOW de ROM0 sem diminuir a
// reserva de 64KiB e sem tocar ROM1, VGA ou audio.
// 0 = posicionamento de memoria identico a v0.1.23 para teste A/B.
#ifndef SMS_ROM_DUMMY_IN_PSRAM
#define SMS_ROM_DUMMY_IN_PSRAM 1
#endif

// v0.1.23: na falta de um bloco DRAM contiguo 16KiB para ROM0,
// tentar duas metades independentes de 8KiB, mantendo ROM1 e a reserva.
// O mapeamento usa os ponteiros cpu_readmap[0..1] existentes: nenhum
// branch adicional nas leituras/opcodes Z80. 0 restaura v0.1.22.
#ifndef SMS_ROM_SLOT0_SPLIT_8K
#define SMS_ROM_SLOT0_SPLIT_8K 1
#endif

// v0.1.25 - EXPERIENCIA REVERSIVEL: so a metade LOW da ROM0 pode
// utilizar reserva interna de 56KiB (57344), desde que haja tambem
// folga DMA antes/depois. ROM1 e a reserva normal continuam com 64KiB.
// Ao falhar quaisquer guardas, mantem ROM0 na PSRAM sem efeito colateral.
// 0 = politica integral de cache identica a v0.1.24 (reserva 64KiB).
#ifndef SMS_ROM0_8K_EXPERIMENT
#define SMS_ROM0_8K_EXPERIMENT 1
#endif
#ifndef SMS_ROM0_8K_EXPERIMENT_RESERVE
#define SMS_ROM0_8K_EXPERIMENT_RESERVE 57344u
#endif
// Guardas conservadoras de DMA: nao trocar margem de memoria essencial
// apenas para ganhar cache de ROM. Os limites sao verificados NO ESP32.
#ifndef SMS_ROM0_DMA_FREE_GUARD
#define SMS_ROM0_DMA_FREE_GUARD 32768u
#endif
#ifndef SMS_ROM0_DMA_LARGEST_GUARD
#define SMS_ROM0_DMA_LARGEST_GUARD 16384u
#endif

// v0.1.18: PSRAM para 32KiB SRAM cartucho; libera DRAM para ROM1
// (Phantasy Star executa a maior parte da cena pesada em $4000-$7FFF).
// A/B: 0 mantém SRAM na DRAM, se ainda nao movida.
#ifndef SMS_ROM_SRAM_IN_PSRAM
#define SMS_ROM_SRAM_IN_PSRAM 1
#endif
// Priorizar slot1 antes de slot0 quando so houver um bloco interno;
// cache de vetores RST/IRQ de 1KiB continua independente.
#ifndef SMS_ROM_PRIORITY_SLOT1
#define SMS_ROM_PRIORITY_SLOT1 1
#endif
// Revalidar alocacao mantendo 64KiB de reserva interna.
#ifndef SMS_ROM_CACHE_INTERNAL_RESERVE
#define SMS_ROM_CACHE_INTERNAL_RESERVE 65536u
#endif

// v0.1.18: após underrun, iniciar com um quadro PCM em vez de dois.
// Inicializacao e volta de menus continuam com dois quadros.
#ifndef SMS_AUDIO_RECOVERY_FRAMES
#define SMS_AUDIO_RECOVERY_FRAMES 1
#endif
// Diagnosticos de perda de audio e maior custo/gap por janela de 5s.
#ifndef SMS_AUDIO_STALL_DIAGNOSTICS
#define SMS_AUDIO_STALL_DIAGNOSTICS 1
#endif

// Evitar que ~1.5KiB de Serial.printf a cada 5s bloqueiem o Z80
// e esgotem a fila PCM. Saida UART em ate 40B/quadro (nao ISR).
// 0 restaura o modo de diagnosticos bloqueante para A/B.
#ifndef SMS_NONBLOCKING_SERIAL_DIAGNOSTICS
#define SMS_NONBLOCKING_SERIAL_DIAGNOSTICS 1
#endif

// v0.1.17: alvo de 64KiB (2 janelas fixas de 16KiB + 2 bancos
// adaptativos da janela 2). Alocar somente enquanto restarem 64KiB
// internos e houver bloco contiguo. Fallback automatico a 48/32/16/0KiB.
// 0 = mesma politica v0.1.16 (32KiB maximo), comparacao A/B.
#ifndef SMS_ROM_CACHE_ADAPTIVE_64K
#define SMS_ROM_CACHE_ADAPTIVE_64K 1
#endif
// Promover banco 2 para DRAM somente quando a mesma pagina for
// remapeada varias vezes. Previne copiar 16KiB para bancos passageiros.
#ifndef SMS_ROM_CACHE_PROMOTE_HITS
#define SMS_ROM_CACHE_PROMOTE_HITS 3
#endif
// Otimizacao Z80/mapper: modulo por mascara para ROMs com numero de
// bancos potencia de dois; restante mantem modulo para compatibilidade.
#ifndef SMS_Z80_MAPPER_FAST_MOD
#define SMS_Z80_MAPPER_FAST_MOD 1
#endif

// v0.1.19: escrita direta na RAM comum do Z80 (C000-FFFB).
// Evita chamada externa a cpu_writemem16 no hot path de LD/PUSH/LDIR,
// mas preserva o mapper FFFC-FFFF, SRAM de cartucho e gravacoes em ROM.
// 0 = rotina identica a v0.1.18, para comparar Phantasy Star A/B.
#ifndef SMS_Z80_FAST_WORK_RAM_WRITE
#define SMS_Z80_FAST_WORK_RAM_WRITE 1
#endif

// v0.1.16: geometry VGA e constante apos vga_init(). Evita dezenas
// de consultas externas por quadro (320x240 / SMS 256x192 / GG 160x192).
// 0 = caminho original, comparacao A/B com o mesmo framebuffer.
#ifndef SMS_VGA_CACHE_GEOMETRY
#define SMS_VGA_CACHE_GEOMETRY 1
#endif

// v0.1.16: caminho especializado do background SMS quando o H-scroll
// esta em zero e nao ha bloqueio horizontal das ultimas 8 colunas.
// Renderiza os mesmos tiles/atributos; 0 = renderer da v0.1.15.
#ifndef SMS_BG_ZERO_SCROLL_FAST
#define SMS_BG_ZERO_SCROLL_FAST 1
#endif

// ========== v0.1.34: DESEMPENHO (60 FPS sem frame skip) ==========
// Cada item pode ser desligado (0) para comparar A/B no hardware.

// Caminho quente na IRAM (Z80 base, VDP, sms.c, renderizador, scanline).
// 0 = codigo na flash, identico a v0.1.33. Verificar no build o uso da
// IRAM (o link falha com "iram0_0_seg overflowed" se nao couber).
#ifndef SMS_IRAM_HOT
#define SMS_IRAM_HOT 1
#endif
// Handlers de prefixos Z80 na IRAM (opcodes base e z80_execute sempre
// vao junto com SMS_IRAM_HOT). ED = LDIR/OTIR/OUTI; DD/FD = IX/IY; CB =
// BIT/SET/RES/rotacoes. Valores: 0 = flash, 1 = subconjunto mais usado
// (lista em tools/gen_z80_hot_map.py), 2 = tabela inteira.
#ifndef SMS_IRAM_Z80_ED
#define SMS_IRAM_Z80_ED 1
#endif
#ifndef SMS_IRAM_Z80_DDFD
#define SMS_IRAM_Z80_DDFD 1
#endif
#ifndef SMS_IRAM_Z80_CB
#define SMS_IRAM_Z80_CB 1
#endif
// Despacho do Z80 por tabela de salto. O Arduino-ESP32 compila com
// -fno-jump-tables: o switch de 256 opcodes vira ~8 comparacoes.
#ifndef SMS_Z80_JUMP_TABLE
#define SMS_Z80_JUMP_TABLE 1
#endif

// Escrita das linhas VGA na PSRAM feita pelo core 0. Os traces mostraram
// ~3,7 ms/quadro so nesse memcpy (cada linha de 256 B = 8 linhas de cache
// PSRAM, com stall do CPU). O core 1 empacota a linha num slot de um anel
// em RAM interna e segue emulando; uma tarefa no core 0 copia para o stage.
// O anel (SLOTS x 264 B) e a tarefa existem so durante a emulacao e sao
// alocados DEPOIS dos caches de ROM (nao alteram a politica de memoria).
// Se a alocacao falhar, usa o caminho anterior (memcpy no core 1).
// 0 = comportamento identico a v0.1.33.
#ifndef SMS_VGA_LINE_RING
#define SMS_VGA_LINE_RING 0
#endif
#ifndef SMS_VGA_LINE_RING_SLOTS
#define SMS_VGA_LINE_RING_SLOTS 16   // potencia de 2
#endif
#ifndef SMS_VGA_LINE_RING_BATCH
#define SMS_VGA_LINE_RING_BATCH 8    // linhas por notificacao ao core 0
#endif

// ISR do DAC (15,7 kHz) registrada no core 0, que fica quase ocioso.
// No core 1 ela custava ~3% do tempo do emulador (~0,5 ms/quadro).
// O anel de audio e SPSC com acquire/release: seguro entre cores.
// 0 = ISR no core do setup() (core 1), como na v0.1.33.
#ifndef SMS_AUDIO_ISR_CORE0
#define SMS_AUDIO_ISR_CORE0 1
#endif

// ========== TELA CHEIA SMS POR HARDWARE (v0.1.37/38) ==========
// Os 256x192 do jogo preenchem o monitor 4:3 sem escalar por software: o
// clock de pixel e reduzido (a linha ativa de 256 amostras ocupa quase o mesmo
// tempo das 320 antigas) e o DMA repete linhas (cada linha do jogo aparece 2-3x).
// Custo de CPU e memoria = 1:1. O tempo total de linha e identico (31,5 kHz):
// o monitor nao muda de modo. F4 alterna tela cheia <-> 1:1. Game Gear: 1:1.
// SMS_FULLSCREEN=0 remove o recurso (identico a v0.1.34).
#ifndef SMS_FULLSCREEN
#define SMS_FULLSCREEN 1
#endif
// Estado inicial apos o boot: 0 = 1:1 com bordas (padrao), 1 = tela cheia. F4 e o menu F1 alternam.
#ifndef SMS_FULLSCREEN_DEFAULT
#define SMS_FULLSCREEN_DEFAULT 0
#endif
// Geometria da imagem esticada, para o overscan do monitor nao cortar as bordas.
// Horizontal: amostras por linha, multiplo de 4 entre 312 e 400 (320 = imagem
// ocupa 100% da largura; 344 = ~93%; 400 = igual ao 1:1). Vertical: linhas de
// video usadas das 480 (448 = 93%, padrao 2-3-2: 7 linhas de video para 3 do jogo).
#ifndef SMS_HW_H_SAMPLES
#define SMS_HW_H_SAMPLES 344
#endif
#ifndef SMS_HW_V_LINES
#define SMS_HW_V_LINES 448
#endif
#if (SMS_HW_H_SAMPLES % 4) != 0 || SMS_HW_H_SAMPLES < 312 || SMS_HW_H_SAMPLES > 400
#error "SMS_HW_H_SAMPLES deve ser multiplo de 4 entre 312 e 400"
#endif
#if SMS_HW_V_LINES < 384 || SMS_HW_V_LINES > 480 || (SMS_HW_V_LINES % 2) != 0
#error "SMS_HW_V_LINES deve ser par e estar entre 384 e 480"
#endif
// v0.1.42: suavizacao vertical. As linhas de video repetidas do meio (2-3-2)
// viram linhas intermediarias (media das vizinhas do jogo), reduzindo a "grade"
// de linhas grossas. Usa 2 linhas de 256 B a cada 3 do jogo (32 KB de RAM
// interna, so enquanto a tela cheia esta ligada e se sobrar memoria: deixa
// SMS_HW_VBLEND_FREE_GUARD bytes livres; senao volta ao modo sem mistura).
// Custo: ~0,3 ms por quadro no core 0 (copia do quadro para o DMA).
#ifndef SMS_HW_VBLEND
#define SMS_HW_VBLEND 1
#endif
#ifndef SMS_HW_VBLEND_FREE_GUARD
#define SMS_HW_VBLEND_FREE_GUARD 24576u
#endif
// Ajuste fino de posicao (opcional): H_SHIFT em amostras (positivo = imagem
// para a ESQUERDA), V_SHIFT em linhas de video (positivo = imagem para BAIXO).
#ifndef SMS_HW_H_SHIFT
#define SMS_HW_H_SHIFT 0
#endif
#ifndef SMS_HW_V_SHIFT
#define SMS_HW_V_SHIFT 0
#endif
// Posicao vertical (linha da tela 320x240) do indicador FPS/EPS no modo 1:1, na
// borda superior (24 linhas acima do jogo). A linha 1 fica no overscan de alguns
// monitores; 12 aparece na borda sem tocar no jogo. Em tela cheia o indicador
// continua na linha 1, sobre a imagem.
#ifndef SMS_OSD_Y_1TO1
#define SMS_OSD_Y_1TO1 12
#endif
// Limite de 8 sprites por linha. Padrao: 0 = VDP real (limite ligado). 1 = sem
// limite (elimina o pisca-pisca de cenas com muitos sprites). F5 e o menu F1
// alternam em tempo real.
#ifndef SMS_SPRITE_NO_LIMIT
#define SMS_SPRITE_NO_LIMIT 0
#endif
// v0.1.39: trava a grade de quadros do emulador numa fase fixa depois do vsync
// do VGA (ver src/sms_vsync_lock.h). Remove quadros repetidos/atrasados em
// cenas de custo variavel. SMS_VSYNC_PHASE_US = atraso do inicio do quadro apos
// o fim da varredura ativa; o quadro precisa terminar antes de 16667 us.
#ifndef SMS_VSYNC_LOCK
#define SMS_VSYNC_LOCK 1
#endif
#ifndef SMS_VSYNC_PHASE_US
#define SMS_VSYNC_PHASE_US 2000
#endif

// ========== CONTROLLER INPUT ==========
#define USE_PS2_KEYBOARD

// PS/2 pins for TTGO VGA32
#define PS2_CLK_PIN     33
#define PS2_DAT_PIN     32

// ========== I2S AUDIO OUTPUT ==========
// Usar frecuencia nativa del SMS (NTSC = 15720 Hz, PAL = 15600 Hz)
#define SAMPLE_RATE     15720   // NTSC SMS native rate
#define AUDIO_BUFFER_SIZE 512   

// ========== SD CARD ==========
#define SD_CS_PIN       13
#define SD_MOSI_PIN     12
#define SD_MISO_PIN     2
#define SD_SCLK_PIN     14
#define SD_FREQ         20000000

// ========== FRAMESKIP ==========
// Controles em tempo real: F12 = FPS/EPS/SKIP; Shift+F12 = 0%/AUTO.
// F3 = BG fracionario ON/OFF, medicao A/B sem reiniciar jogo/ESP32.
// AUTO SMS: custo real de quadro desenhado/omitido, janela 250 ms,
// menor descarte necessario, limite 65%: src/sms_frameskip_adaptive.h.
// Audio: PCM em fila entre PSG e ISR do DAC, 2 quadros de prebuffer.

// Diagnostico a cada 5s: tempos CPU Z80/VDP e VGA em microssegundos.
// Permite medir a distancia ate os 60 EPS e localizar novos gargalos.
#define SMS_PERF_DIAGNOSTICS 1
// Detaile Z80/VDP/BG/SPR/VGA em apenas 1/64 quadros; 0 para desligar.
#define SMS_DETAIL_PROFILE 0

// ========== DEBUG ==========
//#define DEBUG 1

#endif