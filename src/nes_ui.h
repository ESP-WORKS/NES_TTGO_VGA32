// sms_ui.h
#ifndef NES_UI_H
#define NES_UI_H

#include <SD.h>
#include <vector>
#include <string>
#include "sms_vga_adapter.h"
#include "sms_keyboard.h"

// ==================== COLOR DEFINITIONS ====================
#define UI_COLOR_BLACK      0x0000
#define UI_COLOR_WHITE      0xFFFF
#define UI_COLOR_RED        0xF800
#define UI_COLOR_GREEN      0x07E0
#define UI_COLOR_BLUE       0x001F
#define UI_COLOR_YELLOW     0xFFE0
#define UI_COLOR_CYAN       0x07FF
#define UI_COLOR_PURPLE     0x801F
#define UI_COLOR_GRAY       0x8410

// UI Color Scheme
#define BG_COLOR                UI_COLOR_BLACK
#define BAR_COLOR               UI_COLOR_BLUE
#define BAR_TEXT_COLOR          UI_COLOR_WHITE
#define TEXT_COLOR              UI_COLOR_WHITE
#define SELECTED_TEXT_COLOR     UI_COLOR_BLACK
#define SELECTED_BG_COLOR       UI_COLOR_YELLOW
#define BOX_BORDER_COLOR        UI_COLOR_CYAN

// ==================== UI CLASS ====================
class NES_UI
{
public:
    NES_UI(SMSVGADisplay* screen);
    ~NES_UI();
    
    enum MainMenuAction { RESUME_GAME, LOAD_GAMES, TOGGLE_FPS,
                          TOGGLE_SKIP, RESET_CONSOLE, RESET_ESP32,
                          TOGGLE_FULLSCREEN };
    // Navegador para uso na inicializacao ou durante um jogo.
    // Cancelar com ESC somente quando allowCancel=true.
    std::string selectGame(bool allowCancel = false);
    MainMenuAction mainMenu(bool fpsEnabled, bool autoSkip,
                            uint8_t fullscreenMode = 1, bool spriteLimit = true);
    void getGameFiles();
    void drawFileList();
    void drawBars();
    void showLoadingMessage(const char*);
    
private:
    void drawText(const char* text, const int x, const int y);
    
    SMSVGADisplay* screen = nullptr;
    int selected = 0;
    int scroll_offset = 0;
    int max_items = 0;
    static constexpr int ITEM_HEIGHT = 13;
    struct BrowserEntry {
        std::string name;
        bool directory;
    };
    std::vector<BrowserEntry> files;
    std::string directory;
    void browseDirectory(const std::string& path);
    void drawBrowser(bool allowCancel);
    void drawMenuRow(int row, const char* label, bool active);
    static std::string safeText(const std::string& text);
    std::string clipBrowserText(const std::string& text, int pixels,
                                bool preserveEnd = false) const;
    static std::string joinPath(const std::string& dir, const std::string& leaf);
    static std::string parentPath(const std::string& dir);
};

#endif