# Componentes reutilizados e licenças

1. **Nofrendo**: arquivo `NES.zip` fornecido pelo usuário, `MCUME_esp32/espnofrendo/main/nofrendo`. Cabeçalhos de Nofrendo reconhecem Matthew Conte (1998–2000) e GNU Library General Public License v2. Seus avisos permanecem presentes nos arquivos de código. Verifique os termos originais antes da redistribuição de binários.
2. **Vídeo/VGA, navegador, fonte e teclado do SMS**: versão oficial fornecida pelo usuário `SMS_TTGO_VGA_v0_1_43_PLATFORMIO_TELA_CHEIA_HW.zip`, código de menus e vídeo adaptado para este projeto. Fonte original preservada no histórico do repositório do usuário.
3. **Tabela de partições e esquema de MultiLoader**: versão oficial `TTGO-VGA32-MC-1000-main-0.92.zip`, tabela `partitions.csv` mantida.
4. **FabGL 1.0.9**: obtida via PlatformIO e mantida como dependência separada.

Não distribuir ROMs protegidas por copyright sem autorização. Consulte as licenças específicas dos projetos originais antes da redistribuição pública.

5. **Mesen (referência técnica, não incluído no pacote):** `Mesen-master.zip` fornecido pelo usuário. Foram inspecionados `Core/MapperFactory.cpp`, `Core/BaseMapper.h`, `Core/BatteryManager.cpp` e headers específicos de mappers. O código Mesen é **GPL-3.0**. Na v0.1.13, somente os valores de referência RGB das 64 cores NTSC 2C02 de `Core/EmulationSettings.h` foram incorporados como dados em `src/nes_core/nes_ntsc_palette_2c02.h` (192 bytes), com atribuição. Não foi incorporado o mecanismo de emulação do Mesen. Consulte o texto GPL-3.0 no `Mesen-master.zip` original antes de redistribuir o conjunto.
6. **NESdev Wiki:** especificações públicas de chips de cartucho MMC1, MMC3, TxSROM e TQROM utilizadas como documentação complementar.
