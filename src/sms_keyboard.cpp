// sms_keyboard.cpp
#include "sms_keyboard.h"
#include "nes_bootrc.h"
#include "../config.h"

fabgl::PS2Controller SMSKeyboard::ps2;
fabgl::Keyboard SMSKeyboard::keyboard;
uint8_t SMSKeyboard::currentState = 0;
bool SMSKeyboard::fpsTogglePending = false;
bool SMSKeyboard::frameSkipTogglePending = false;
bool SMSKeyboard::fractionalBgTogglePending = false;
bool SMSKeyboard::fullscreenTogglePending = false;
bool SMSKeyboard::spriteLimitTogglePending = false;
bool SMSKeyboard::mainMenuPending = false;
bool SMSKeyboard::loadGamesPending = false;
bool SMSKeyboard::escapePending = false;

void SMSKeyboard::init() {
    #ifdef DEBUG
        Serial.println("SMS Keyboard: Initializing PS/2...");
    #endif
    
    ps2.begin((gpio_num_t)g_nes_bootrc.kbdclk, (gpio_num_t)g_nes_bootrc.kbddat);
    keyboard.begin(true, false, 0);
    
    #ifdef DEBUG
        Serial.println("SMS Keyboard: Ready");
        Serial.println("  Arrow Keys - D-Pad");
        Serial.println("  Z - A, X - B, LEFT CTRL - Select, ENTER - Start");
        Serial.println("  X - Button 2");
        Serial.println("  LEFT CTRL - NES Select");
        Serial.println("  ESC - Open game browser (no ESP32 reset)");
    #endif
}

void SMSKeyboard::updateState() {
    currentState = 0;
    
    // D-Pad
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_UP))    currentState |= SMS_UP;
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_DOWN))  currentState |= SMS_DOWN;
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_LEFT))  currentState |= SMS_LEFT;
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_RIGHT)) currentState |= SMS_RIGHT;
    
    // Button 1 (Z)
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_Z) || 
        keyboard.isVKDown(fabgl::VirtualKey::VK_z)) currentState |= SMS_BTN1;
    
    // Button 2 (X)
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_X) || 
        keyboard.isVKDown(fabgl::VirtualKey::VK_x)) currentState |= SMS_BTN2;
    
    // Start (ENTER)
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_RETURN)) currentState |= SMS_START;
    
    // NES SELECT (CTRL ESQUERDO)
    if (keyboard.isVKDown(fabgl::VirtualKey::VK_LCTRL)) currentState |= SMS_PAUSE;
    
    // F12 e Shift+F12: debounce no flanco de pressionamento.
    // Shift nao deve gerar evento de F12 simples.
    static bool lastF12 = false;
    const bool f12 = keyboard.isVKDown(fabgl::VirtualKey::VK_F12);
    if (f12 && !lastF12) {
        const bool shift = keyboard.isVKDown(fabgl::VirtualKey::VK_LSHIFT) ||
                           keyboard.isVKDown(fabgl::VirtualKey::VK_RSHIFT);
        if (shift) frameSkipTogglePending = true;
        else fpsTogglePending = true;
    }
    lastF12 = f12;

    // F3 livre: alternancia A/B sem parar o jogo e sem repeticao enquanto
    // pressionada (o filtro no flanco descarta autorepeat de PS/2).
    static bool lastF3 = false;
    const bool f3 = keyboard.isVKDown(fabgl::VirtualKey::VK_F3);
    if (f3 && !lastF3) fractionalBgTogglePending = true;
    lastF3 = f3;

    // F4: tela cheia SMS ON/OFF (flanco; ignora autorepeat).
    static bool lastF4 = false;
    const bool f4 = keyboard.isVKDown(fabgl::VirtualKey::VK_F4);
    if (f4 && !lastF4) fullscreenTogglePending = true;
    lastF4 = f4;
    // F5: limite de sprites por linha (flanco).
    static bool lastF5 = false;
    const bool f5 = keyboard.isVKDown(fabgl::VirtualKey::VK_F5);
    if (f5 && !lastF5) spriteLimitTogglePending = true;
    lastF5 = f5;

    // Os menus consomem eventos de um unico pressionamento; nao alteram
    // diretamente a memoria do console nem reiniciam o ESP32.
    static bool lastF1 = false, lastF2 = false, lastEsc = false;
    const bool f1 = keyboard.isVKDown(fabgl::VirtualKey::VK_F1);
    const bool f2 = keyboard.isVKDown(fabgl::VirtualKey::VK_F2);
    const bool esc = keyboard.isVKDown(fabgl::VirtualKey::VK_ESCAPE);
    if (f1 && !lastF1) mainMenuPending = true;
    static bool lastMagic=false;
    const bool magic=g_nes_bootrc.magicb>=0 && digitalRead(g_nes_bootrc.magicb)==LOW;
    if(magic && !lastMagic) mainMenuPending=true;
    lastMagic=magic;
    if (f2 && !lastF2) loadGamesPending = true;
    if (esc && !lastEsc) escapePending = true;
    lastF1 = f1;
    lastF2 = f2;
    lastEsc = esc;
}

uint8_t SMSKeyboard::read() {
    updateState();
    return currentState;
}

bool SMSKeyboard::takeFpsToggle() {
    const bool result = fpsTogglePending;
    fpsTogglePending = false;
    return result;
}

bool SMSKeyboard::takeFrameSkipToggle() {
    const bool result = frameSkipTogglePending;
    frameSkipTogglePending = false;
    return result;
}

#define SMS_TAKE_EVENT(name, field) \
    bool SMSKeyboard::name() { const bool value = field; field = false; return value; }
SMS_TAKE_EVENT(takeMainMenu, mainMenuPending)
SMS_TAKE_EVENT(takeLoadGames, loadGamesPending)
SMS_TAKE_EVENT(takeEscape, escapePending)
SMS_TAKE_EVENT(takeFractionalBgToggle, fractionalBgTogglePending)
SMS_TAKE_EVENT(takeFullscreenToggle, fullscreenTogglePending)
SMS_TAKE_EVENT(takeSpriteLimitToggle, spriteLimitTogglePending)
#undef SMS_TAKE_EVENT
