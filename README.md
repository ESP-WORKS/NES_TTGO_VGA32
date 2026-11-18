# NES TTGO VGA32 v1.03 by André Chaves: Smoothed Edges (F5) + CPU/IRQ Fixes

> v1.03: See `ALTERACOES_v1.03_BORDAS_E_CPU.md` (smoothed edges on all 4 sides without cropping, source-based IRQ handling, indirect addressing, and unofficial opcodes).

> v1.02: See `ALTERACOES_v1.02_CORRECOES_NUCLEO.md` (8 fixes to PPU, CPU, MMC3, and frame IRQ handling, with no performance overhead).

**Source:** The NES v1.00 ZIP file provided by the user. This v1.01 release only removes the artificial cropping previously used to hide overscan, while preserving the original project structure, menus, and all other features included in the source archive.

## v1.01 Changes

- `NES_VGA_OVERSCAN_SIDE=0`: Restores the **8 pixels on the left and 8 pixels on the right** that were previously replaced with black in centered mode or cropped by DMA in F4 mode.
- `NES_VGA_OVERSCAN_BOTTOM=0`: Restores the **8 bottom rows** that were previously replaced with blanking in F4 mode.
- F4 output returns to its original geometry of **256 active pixels / 344 samples per line** (default profile), with no DMA pointer offset and no changes to the NES image dimensions through cropping.
- The FPS/EPS OSD returns to the beginning of the game area in F4 mode (x=1), since horizontal cropping is no longer applied.
- All **240 NES scanlines** are displayed across the 480 VGA periods (2 VGA lines per NES scanline), with no artificial black bar at the bottom.
- The margins in **320×240 centered mode** (32 pixels to the left and right of the game area) are natural margins of this mode and **are not** overscan masks. They remain unchanged.

**Important:** Removing the cropping may reveal existing artifacts/tiles along the edges in some games. This change does not modify the PPU engine or fix any underlying scrolling artifacts; it simply stops hiding them.

## Build and Installation

1. Open this folder in **VS Code with PlatformIO**.
2. Run **PlatformIO: Clean** followed by **PlatformIO: Build**.
3. Flash the firmware using the previously approved **MultiLoader** procedure. The standard `Upload` command remains blocked to protect the *factory* partition.
4. Continue using ROMs and the microSD card as before; the ZIP file does not include ROMs or binaries.

## Hardware Testing

1. Start a game and verify that all 256 pixels are displayed, including both horizontal edges.
2. Toggle F4 on/off: all 240 rows should remain visible, including the bottom 8 rows.
3. Check that the FPS/EPS indicator displays all characters correctly and that the F1 menus still work.
4. Test horizontal/diagonal scrolling in Double Dragon II and Castlevania II, confirming that content in the bottom corners is visible again.
5. Verify FPS, EPS, audio, controls, ROM loading, savegames, and returning to MultiLoader.

**Validation:** Geometry and configuration checks were performed on the host system. Xtensa/PlatformIO compilation and physical hardware testing were not performed in this environment.

The document `AUDITORIA_GERAL_VIDEO_OVERSCAN_v1.00.md` is preserved as a historical record of the original version with overscan masking. See `ALTERACOES_v1.01_SEM_MASCARAS.md` for details of this change.
