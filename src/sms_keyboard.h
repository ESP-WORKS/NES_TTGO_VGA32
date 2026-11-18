// sms_keyboard.h
#ifndef SMS_KEYBOARD_H
#define SMS_KEYBOARD_H

#include <Arduino.h>
#include <fabgl.h>

class SMSKeyboard {
public:
    static void init();
    static uint8_t read();
    // Eventos de tecla, disparados somente na transicao solta -> pressionada.
    static bool takeFpsToggle();
    static bool takeFrameSkipToggle();
    static bool takeFractionalBgToggle(); // F3: A/B sem reiniciar o jogo
    static bool takeFullscreenToggle();   // F4: tela cheia SMS ON/OFF
    static bool takeSpriteLimitToggle();  // F5: limite de 8 sprites/linha ON/OFF
    static bool takeMainMenu();
    static bool takeLoadGames();
    static bool takeEscape();
    
    // SMS Button bitmask values (Botón 1 y Botón 2)
    enum SMSButton : uint8_t {
        SMS_UP       = 0x01,
        SMS_DOWN     = 0x02,
        SMS_LEFT     = 0x04,
        SMS_RIGHT    = 0x08,
        SMS_BTN1     = 0x10,  // Button 1 (Z)
        SMS_BTN2     = 0x20,  // Button 2 (X)
        SMS_START    = 0x40,  // ENTER
        SMS_PAUSE    = 0x80   // LEFT CTRL (NES SELECT)
    };
    
private:
    static fabgl::PS2Controller ps2;
    static fabgl::Keyboard keyboard;
    static uint8_t currentState;
    static bool fpsTogglePending;
    static bool frameSkipTogglePending;
    static bool fractionalBgTogglePending;
    static bool fullscreenTogglePending;
    static bool spriteLimitTogglePending;
    static bool mainMenuPending;
    static bool loadGamesPending;
    static bool escapePending;
    static void updateState();
};

#endif