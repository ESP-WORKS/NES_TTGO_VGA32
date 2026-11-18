# NES TTGO VGA32 v1.03: bordas suavizadas (F5) + correções de CPU/IRQ

> v1.03: ver `ALTERACOES_v1.03_BORDAS_E_CPU.md` (bordas suavizadas nas 4 bordas sem recorte, IRQ por fonte, endereçamento indireto e opcodes não oficiais).


> v1.02: ver `ALTERACOES_v1.02_CORRECOES_NUCLEO.md` (8 correções em PPU, CPU, MMC3 e IRQ de quadro, sem custo de desempenho).

**Origem:** o ZIP NES v1.00 enviado pelo usuário. Esta v1.01 remove apenas o recorte artificial usado para esconder o overscan, mantendo a estrutura do projeto, os menus e os demais recursos presentes no arquivo de origem.

## Alterações v1.01

- `NES_VGA_OVERSCAN_SIDE=0`: restaura os **8 pixels da esquerda e 8 pixels da direita** anteriormente substituídos por preto no modo centralizado ou recortados pelo DMA no F4.
- `NES_VGA_OVERSCAN_BOTTOM=0`: restaura as **8 linhas inferiores** anteriormente substituídas por blanking no F4.
- A saída do F4 volta à geometria de **256 pixels ativos / 344 amostras por linha** (perfil padrão), sem deslocamento do ponteiro DMA e sem alteração do tamanho da imagem NES por recorte.
- O OSD FPS/EPS volta ao início da área de jogo no F4 (x=1), porque o corte lateral não existe mais.
- As **240 linhas NES** são apresentadas nos 480 períodos VGA (2 linhas por linha NES), sem faixa preta artificial no rodapé.
- As margens do modo **320×240 centralizado** (32 pixels à esquerda e à direita do jogo) são margens naturais desse modo, e **não** são máscaras de overscan. Permanecem como antes.

**Importante:** a eliminação do recorte permite voltar a ver artefatos/tiles existentes nas bordas de alguns jogos. Não altera o motor PPU nem corrige eventuais artefatos de rolagem na origem; somente deixa de escondê-los.

## Compilação e instalação

1. Abrir esta pasta em **VS Code com PlatformIO**.
2. Executar **PlatformIO: Clean** e **PlatformIO: Build**.
3. Gravar pelo fluxo já aprovado do **MultiLoader**. O comando `Upload` padrão permanece bloqueado para preservar a partição *factory*.
4. Continuar usando as ROMs e o microSD como antes; o ZIP não inclui ROMs ou binários.

## Teste em hardware

1. Iniciar um jogo e verificar os 256 pixels completos, incluindo as extremidades laterais.
2. Alternar F4 ligado/desligado: todas as 240 linhas devem ficar visíveis, incluindo as 8 inferiores.
3. Conferir que o indicador FPS/EPS não perdeu letras e os menus F1 continuam funcionando.
4. Testar scroll horizontal/diagonal em Double Dragon II e Castlevania II e confirmar que o conteúdo do canto inferior voltou a aparecer.
5. Verificar FPS, EPS, áudio, teclas, carregamento de ROM, savegames e saída para o MultiLoader.

**Validação:** verificações de geometria e configuração feitas em host. Compilação Xtensa/PlatformIO e teste físico não foram realizados neste ambiente.

O documento `AUDITORIA_GERAL_VIDEO_OVERSCAN_v1.00.md` é preservado como histórico da versão original com máscara. Veja `ALTERACOES_v1.01_SEM_MASCARAS.md` para o detalhamento desta alteração.
