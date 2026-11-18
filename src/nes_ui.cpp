// sms_ui.cpp
#include "nes_ui.h"
#include <algorithm>
#include <ctype.h>
#include "sms_sd_case.h"

NES_UI::NES_UI(SMSVGADisplay* screen)
{    
    this->screen = screen;
    selected = 0;
    scroll_offset = 0;
}

NES_UI::~NES_UI()
{
}

void NES_UI::drawBars() 
{
    int screenWidth  = screen->getWidth();
    int screenHeight = screen->getHeight();
    
    // Top bar
    screen->fillRect(0, 0, screenWidth, 24, BAR_COLOR);
    screen->fillRect(0, 22, screenWidth, 2, UI_COLOR_CYAN);
    
    screen->setTextColor(BAR_TEXT_COLOR, BAR_COLOR);
    screen->drawBrowserSerif("NES EMULATOR FOR TTGO VGA32", 55, 8);
    
    // Bottom bar reduced to 30 pixels
    screen->fillRect(0, screenHeight - 30, screenWidth, 30, BAR_COLOR);
    screen->fillRect(0, screenHeight - 32, screenWidth, 2, UI_COLOR_CYAN);
    
    // First line - Controls (higher up)
    int y1 = screenHeight - 22;
    screen->setTextColor(UI_COLOR_YELLOW, BAR_COLOR);
    screen->drawBrowserSerif("Z=A X=B CTRL=SELECT ENTER=START", 60, y1);
    
    screen->setTextColor(UI_COLOR_CYAN, BAR_COLOR);
    screen->setCursor(180, y1);
    screen->print("\x18  \x19  \x1A  \x1B");
    
    // Second line - System buttons (directly below)
    int y2 = screenHeight - 12;
    screen->setTextColor(UI_COLOR_GREEN, BAR_COLOR);
    screen->drawBrowserSerif("F1=MENU   F2=JOGOS", 30, y2);
}

void NES_UI::showLoadingMessage(const char* message)
{
    int screenWidth  = screen->getWidth();
    int screenHeight = screen->getHeight();
    
    // Caixa de aviso centralizada. O fundo preto cobre so a propria caixa (mais
    // 4 px de margem): uma faixa de largura total apagava as bordas laterais
    // do navegador de jogos e do menu por tras do aviso.
    screen->fillRect(screenWidth/2 - 104, screenHeight/2 - 16, 208, 32, UI_COLOR_BLACK);
    screen->drawRect(screenWidth/2 - 100, screenHeight/2 - 12, 200, 24, UI_COLOR_CYAN);
    screen->setTextColor(UI_COLOR_YELLOW, UI_COLOR_BLACK);
    screen->setCursor((screenWidth - screen->textWidth(message)) / 2, screenHeight/2 - 4);
    screen->print(message);
    screen->updateDisplay();
}

// O browser usa nomes base para evitar duplicar caminhos: a SD library pode
// retornar file.name() absoluto ou apenas o ultimo componente.
namespace {
static std::string basenameOf(const char* path) {
    std::string name = path ? path : "";
    size_t sep = name.find_last_of("/\\");
    return sep == std::string::npos ? name : name.substr(sep + 1);
}
static bool isRom(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == ".nes";
}
static int compareCI(const std::string& a, const std::string& b) {
    size_t i = 0;
    for (; i < a.size() && i < b.size(); ++i) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a.compare(b);
}
}

std::string NES_UI::safeText(const std::string& text) {
    // A fonte VGA eh ASCII: evita indices negativos com UTF-8.
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) out += (ch >= 32 && ch < 127) ? (char)ch : '?';
    return out;
}

// Nomes sao cortados pela largura REAL da fonte TK-2000 (nao por
// contagem fixa de 7 pixels). Em caminhos longos, conserva a pasta final.
std::string NES_UI::clipBrowserText(const std::string& text, int pixels,
                                     bool preserveEnd) const {
    if (pixels <= 0) return "";
    if (screen->browserSerifWidth(text.c_str()) <= pixels) return text;
    const std::string dots = "...";
    if (screen->browserSerifWidth(dots.c_str()) > pixels) return "";
    std::string clipped;
    if (preserveEnd) {
        for (size_t i = text.size(); i > 0; --i) {
            std::string candidate = dots + text.substr(i - 1);
            if (screen->browserSerifWidth(candidate.c_str()) > pixels) break;
            clipped = candidate;
        }
    } else {
        for (size_t i = 1; i <= text.size(); ++i) {
            std::string candidate = text.substr(0, i) + dots;
            if (screen->browserSerifWidth(candidate.c_str()) > pixels) break;
            clipped = candidate;
        }
    }
    return clipped.empty() ? dots : clipped;
}

std::string NES_UI::joinPath(const std::string& dir, const std::string& leaf) {
    return (dir == "/" ? "/" : dir + "/") + leaf;
}

std::string NES_UI::parentPath(const std::string& dir) {
    if (dir == "/" || dir.empty()) return "/";
    size_t end = dir.find_last_of('/');
    return end == 0 || end == std::string::npos ? "/" : dir.substr(0, end);
}

void NES_UI::browseDirectory(const std::string& path) {
    directory = path;
    selected = 0;
    scroll_offset = 0;
    getGameFiles();
}

void NES_UI::getGameFiles() {
    files.clear();
    if (directory != "/") files.push_back({"..", true});
    File root = SD.open(directory.c_str());
    if (!root || !root.isDirectory()) { if(root) root.close(); return; }
    while (true) {
        File file = root.openNextFile();
        if (!file) break;
        const bool dir = file.isDirectory();
        const std::string name = basenameOf(file.name());
        if (!name.empty() && name != "." && name != ".." &&
            (dir || isRom(name))) files.push_back({name, dir});
        file.close();
    }
    root.close();
    // Entrada .. sempre em primeiro lugar. Diretorios antes das ROMs.
    const size_t first = (directory == "/" ? 0 : 1);
    std::sort(files.begin() + first, files.end(),
              [](const BrowserEntry& a, const BrowserEntry& b) {
                  return a.directory == b.directory
                         ? compareCI(a.name,b.name) < 0
                         : a.directory;
              });
}

void NES_UI::drawBrowser(bool allowCancel) {
    const int width = screen->getWidth();
    const int height = screen->getHeight();
    screen->fillScreen(BG_COLOR);
    drawBars();
    screen->fillRect(0, height - 14, width, 12, BAR_COLOR);
    screen->setTextColor(UI_COLOR_GREEN, BAR_COLOR);
    screen->drawBrowserSerif(allowCancel ? "ENTER/Z=ABRIR  ESC=VOLTAR" :
                                       "ENTER/Z=ABRIR  SETAS=MOVER", 17, height - 13);
    // Indicador de diretorio, recortado pela esquerda para manter o final.
    std::string path = safeText(directory);
    const int dirLabelWidth = screen->browserSerifWidth("DIR:");
    const int pathX = 14 + dirLabelWidth + 6;
    path = clipBrowserText(path, width - pathX - 12, true);
    screen->setTextColor(UI_COLOR_CYAN, BG_COLOR);
    screen->drawBrowserSerif("DIR:", 12, 27);
    screen->drawBrowserSerif(path.c_str(), pathX, 27);
    const int listY = 42;
    const int listBottom = height - 36;
    max_items = (listBottom - listY - 4) / ITEM_HEIGHT;
    if (max_items < 1) max_items = 1;
    screen->drawRect(8, listY, width - 16, listBottom - listY, BOX_BORDER_COLOR);
    drawFileList();
}

void NES_UI::drawFileList() {
    const int width = screen->getWidth();
    const int height = screen->getHeight();
    const int y0 = 46;
    screen->fillRect(10, 44, width - 20, height - 83, BG_COLOR);
    if (files.empty()) {
        screen->setTextColor(UI_COLOR_YELLOW, BG_COLOR);
        screen->drawBrowserSerif("SEM ROMS NESTA PASTA", 18, 57);
    }
    for (int i=0; i<max_items; ++i) {
        int item = scroll_offset + i;
        if (item >= (int)files.size()) break;
        std::string name = (files[item].directory && files[item].name != ".." ? "[+] " : "")
                           + safeText(files[item].name);
        name = clipBrowserText(name, width - 43);
        const int y = y0 + i * ITEM_HEIGHT;
        const bool active = item == selected;
        if (active) screen->fillRect(12,y-2,width-24,ITEM_HEIGHT,SELECTED_BG_COLOR);
        screen->setTextColor(active ? SELECTED_TEXT_COLOR : TEXT_COLOR,
                             active ? SELECTED_BG_COLOR : BG_COLOR);
        screen->drawBrowserSerif(name.c_str(), 19, y + 1);
    }
    screen->updateDisplay();
}

std::string NES_UI::selectGame(bool allowCancel) {
    // Sempre comeca na pasta padrao; preserva compatibilidade de SD antigo
    // criando os diretorios ausentes no FAT32, ou volta a raiz em caso de erro.
    std::string actualDefault;
    const bool hasDefault = SMSSDCase::ensureDirectory("/NES/roms", actualDefault);
    browseDirectory(hasDefault ? actualDefault : "/");
    screen->endEmulation();
    drawBrowser(allowCancel);
    uint32_t lastMove = 0;
    bool selectHeld = true; // exige liberar ENTER/Z ao entrar
    while (true) {
        const uint8_t key = SMSKeyboard::read();
        SMSKeyboard::takeFpsToggle();
        SMSKeyboard::takeFrameSkipToggle();
        SMSKeyboard::takeMainMenu();
        SMSKeyboard::takeLoadGames();
        if (SMSKeyboard::takeEscape() && allowCancel) return "";
        const bool pressed = (key & (SMSKeyboard::SMS_BTN1 | SMSKeyboard::SMS_START)) != 0;
        if (!pressed) selectHeld = false;
        if (pressed && !selectHeld && selected < (int)files.size()) {
            selectHeld = true;
            const BrowserEntry entry = files[selected];
            if (entry.directory) {
                browseDirectory(entry.name == ".." ? parentPath(directory)
                                                    : joinPath(directory, entry.name));
                drawBrowser(allowCancel);
            } else {
                const std::string path = joinPath(directory, entry.name);
                showLoadingMessage("CARREGANDO JOGO...");
                return path;
            }
        }
        const uint32_t now = millis();
        if (!files.empty() && now - lastMove >= 145) {
            int old = selected;
            if (key & SMSKeyboard::SMS_UP) --selected;
            else if (key & SMSKeyboard::SMS_DOWN) ++selected;
            if (selected < 0) selected = (int)files.size() - 1;
            if (selected >= (int)files.size()) selected = 0;
            if (old != selected) {
                if (selected < scroll_offset) scroll_offset = selected;
                if (selected >= scroll_offset + max_items)
                    scroll_offset = selected - max_items + 1;
                drawFileList();
                lastMove = now;
            }
        }
        delay(5);
    }
}

void NES_UI::drawMenuRow(int row, const char* label, bool active) {
    const int width = screen->getWidth();
    const int y = 52 + row * 19;               // 8 linhas cabem na caixa
    screen->fillRect(15, y-3, width - 30, 17,
                     active ? SELECTED_BG_COLOR : BG_COLOR);
    screen->setTextColor(active ? SELECTED_TEXT_COLOR : TEXT_COLOR,
                         active ? SELECTED_BG_COLOR : BG_COLOR);
    screen->drawString(label, 25, y);
}

NES_UI::MainMenuAction NES_UI::mainMenu(bool fpsEnabled, bool autoSkip,
                                        uint8_t fullscreenMode, bool spriteLimit) {
    screen->endEmulation();
    screen->fillScreen(BG_COLOR);
    screen->fillRect(0, 0, screen->getWidth(), 25, BAR_COLOR);
    screen->setTextColor(UI_COLOR_WHITE, BAR_COLOR);
    screen->drawString("MENU PRINCIPAL", 85, 8);
    screen->drawRect(9, 34, screen->getWidth()-18, 159, BOX_BORDER_COLOR);
    screen->setTextColor(UI_COLOR_CYAN, BG_COLOR);
    screen->drawString("NINTENDO ENTERTAINMENT SYSTEM", 53, 39);
    static constexpr int N = 7;
    int current = 0;
    auto draw = [&]() {
        drawMenuRow(0, "CARREGAR JOGOS", current == 0);
        drawMenuRow(1, fpsEnabled ? "EXIBIR FPS/EPS: ON" : "EXIBIR FPS/EPS: OFF", current == 1);
        drawMenuRow(2, autoSkip ? "FRAME SKIP: AUTO" : "FRAME SKIP: 0%", current == 2);
        drawMenuRow(3, fullscreenMode ? "TELA CHEIA (F4): ON" : "TELA CHEIA (F4): OFF", current == 3);
        drawMenuRow(4, "COLD RESET", current == 4);
        drawMenuRow(5, "REINICIAR ESP32", current == 5);
        drawMenuRow(6, "RETORNAR AO JOGO", current == 6);
    };
    draw();
    screen->setTextColor(UI_COLOR_YELLOW, BG_COLOR);
    screen->drawString("SETAS: MOVER  ENTER/Z: OK", 17, 218);
    screen->updateDisplay();
    uint32_t lastMove = 0;
    bool selectHeld = true;
    while (true) {
        const uint8_t key = SMSKeyboard::read();
        SMSKeyboard::takeFpsToggle();
        SMSKeyboard::takeFrameSkipToggle();
        SMSKeyboard::takeFullscreenToggle();    // F4/F5 so valem no jogo;
        SMSKeyboard::takeSpriteLimitToggle();   // evitar toggle atrasado ao voltar
        if (SMSKeyboard::takeMainMenu() || SMSKeyboard::takeEscape()) return RESUME_GAME;
        if (SMSKeyboard::takeLoadGames()) return LOAD_GAMES;
        const bool pressed = (key & (SMSKeyboard::SMS_BTN1 | SMSKeyboard::SMS_START)) != 0;
        if (!pressed) selectHeld = false;
        if (pressed && !selectHeld) {
            switch (current) {
                case 0: return LOAD_GAMES;
                case 1: return TOGGLE_FPS;
                case 2: return TOGGLE_SKIP;
                case 3: return TOGGLE_FULLSCREEN;
                case 4: return RESET_CONSOLE;
                case 5: return RESET_ESP32;
                default: return RESUME_GAME;
            }
        }
        const uint32_t now = millis();
        if (now - lastMove >= 145) {
            int old = current;
            if (key & SMSKeyboard::SMS_UP) current = (current + N - 1) % N;
            else if (key & SMSKeyboard::SMS_DOWN) current = (current + 1) % N;
            if (old != current) { draw(); screen->updateDisplay(); lastMove = now; }
        }
        delay(5);
    }
}
