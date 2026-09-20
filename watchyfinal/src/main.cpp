#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <RTClib.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include <lvgl.h>
#include "ui.h" 
#include <esp_sleep.h> 
#include <driver/gpio.h>
#include <math.h>
#include "WatchLink.h"
#include "WatchSecrets.h" 

// --- PINS & ADDRESSES ---
const int I2C_SDA = 13; 
const int I2C_SCL = 14; 
const int PIN_CHG = 21; 
const int BTN_MAIN = 18; 

// --- APP PINS ---
const int LASER_EN = 6; 
bool laserState = false; 

// --- PWM BACKLIGHT SETTINGS ---
const int PWM_CHANNEL = 0;
const int PWM_FREQ = 5000;
const int PWM_RES = 8; // 8-bit resolution (0-255)
const int MAX_BRIGHTNESS = 255;
const int DIM_BRIGHTNESS = 15; // Tweak this between 1 and 30 for the perfect dimness!

// --- BUTTON PINS ---
const int BTN1 = 48; 
const int BTN2 = 47; 

int btn1State = LOW;
unsigned long btn1DebounceTime = 0;

int btn2State = LOW;
unsigned long btn2DebounceTime = 0;

bool isMenuOpen = false;

// --- POWER MANAGEMENT VARIABLES ---
const unsigned long IDLE_TIMEOUT_MS = 300000;         // 5 minutes (Screen Dims)
const unsigned long DEEP_SLEEP_TIMEOUT_MS = 1800000;  // 30 minutes (Full Shutdown)
const unsigned long DEEP_SLEEP_HOLD_TIME_MS = 1500;   // 1.5 Seconds hold time
unsigned long lastActivityTime = 0;
bool isScreenOff = false;
lv_obj_t * screenBeforeSleep = NULL; 

// --- MAIN BUTTON STATE MACHINE ---
bool mainBtnIsPressed = false;
unsigned long mainBtnPressTime = 0;
unsigned long mainBtnReleaseTime = 0;
bool deepSleepTriggered = false;

// --- RTC (DEEP SLEEP) MEMORY ---
// 0 = Main, 1 = Gyro, 2 = Laser
RTC_DATA_ATTR int savedScreenID = 0; 
RTC_DATA_ATTR uint32_t savedCompileTime = 0;

// --- HARDWARE OBJECTS ---
RTC_DS3231 rtc; 
SFE_MAX1704X lipo;
TFT_eSPI tft = TFT_eSPI();

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
    lv_disp_flush_ready(disp_drv);
}

unsigned long lastTimeUpdate = 0;

// --- SCREEN DIM HELPER (CPU remains awake; this is NOT ESP light sleep) ---
void turnOffScreen() {
    Serial.println("Dimming Screen (network remains active)...");
    
    // Smoothly fade the backlight down to the dim setting
    for (int i = MAX_BRIGHTNESS; i >= DIM_BRIGHTNESS; i--) {
        ledcWrite(PWM_CHANNEL, i);
        delay(3);
    }
    
    isScreenOff = true;
}

// --- RESTORE BACKLIGHT HELPER ---
void turnOnScreen() {
    Serial.println("Waking Screen...");
    
    // Smoothly fade the backlight back up to max
    for (int i = DIM_BRIGHTNESS; i <= MAX_BRIGHTNESS; i++) {
        ledcWrite(PWM_CHANNEL, i);
        delay(2);
    }
    
    isScreenOff = false;
    lastActivityTime = millis();
    lastTimeUpdate = millis();
}

// --- DEEP SLEEP HELPER ---
void enterDeepSleep() {
    Serial.println("Preparing for Deep Sleep...");
    watchLinkPrepareSleep(); // Stops Wi-Fi/TCP; no notifications until button wake.

    lv_obj_t * currentScreen = lv_disp_get_scr_act(NULL);
    if (currentScreen == ui_MainScreen) savedScreenID = 0;
    else if (currentScreen == ui_GyroScreen) savedScreenID = 1;
    else if (currentScreen == ui_LaserScreen) savedScreenID = 2;

    digitalWrite(LASER_EN, LOW);
    laserState = false;
    if (ui_LaserSwitch != NULL) {
        lv_obj_clear_state(ui_LaserSwitch, LV_STATE_CHECKED); 
    }

    // Smoothly fade the backlight completely to ZERO
    int currentBrightness = isScreenOff ? DIM_BRIGHTNESS : MAX_BRIGHTNESS;
    for (int i = currentBrightness; i >= 0; i--) {
        ledcWrite(PWM_CHANNEL, i);
        delay(3);
    }

    // Release PWM and safely lock the pin LOW to stop battery leaks
    ledcDetachPin(TFT_BL);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW); 
    gpio_hold_en((gpio_num_t)TFT_BL); 
    gpio_deep_sleep_hold_en();
    
    tft.writecommand(0x10); // Put TFT driver chip to sleep

    // ONLY the Main Button can wake from Deep Sleep
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_18, 1); 
    
    Serial.println("Goodnight!");
    esp_deep_sleep_start();
}

void setup() {
    gpio_hold_dis((gpio_num_t)TFT_BL);
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
    ledcAttachPin(TFT_BL, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0); // Keep screen black during boot sequence

    Serial.begin(115200);
    Serial.println("--- Booting WatchyUI ---");
    
    pinMode(PIN_CHG, INPUT_PULLUP);
    pinMode(BTN_MAIN, INPUT); 
    
    pinMode(BTN1, INPUT_PULLDOWN); 
    pinMode(BTN2, INPUT_PULLDOWN);

    pinMode(LASER_EN, OUTPUT);
    digitalWrite(LASER_EN, LOW); 

    Wire.begin(I2C_SDA, I2C_SCL);
    rtc.begin();

    // --- SMART RTC SYNC ---
    DateTime compiledTime = DateTime(F(__DATE__), F(__TIME__));
    if (savedCompileTime != compiledTime.unixtime()) {
        Serial.println("New code upload detected! Syncing RTC to PC time...");
        rtc.adjust(compiledTime);
        savedCompileTime = compiledTime.unixtime(); 
    }

    // Init MAX17048 Fuel Gauge
    if (!lipo.begin()) Serial.println("MAX17048 not detected!");
    else Serial.println("MAX17048 Initialized");

    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    lv_init();
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf[240 * 28]; 
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 240 * 28);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240;
    disp_drv.ver_res = 280;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init(); 
    
    isMenuOpen = false;
    isScreenOff = false;
    
    if (savedScreenID == 1 && ui_GyroScreen != NULL) {
        lv_disp_load_scr(ui_GyroScreen);
    } 
    else if (savedScreenID == 2 && ui_LaserScreen != NULL) {
        lv_disp_load_scr(ui_LaserScreen);
    } 
    else if (ui_MainScreen != NULL) {
        lv_disp_load_scr(ui_MainScreen);
        savedScreenID = 0; 
    }

    // Fade the screen on for a premium boot effect!
    for (int i = 0; i <= MAX_BRIGHTNESS; i++) {
        ledcWrite(PWM_CHANNEL, i);
        delay(2);
    }

    while(digitalRead(BTN_MAIN) == HIGH) { delay(10); }
    lastActivityTime = millis(); 
    watchLinkBegin(); // Network task starts AFTER the display and UI are ready.
    Serial.println("--- Setup Complete ---");
}

void loop() {
    // All LVGL updates still happen only on this Arduino loop thread.
    if (watchLinkPoll()) {
        if (isScreenOff) turnOnScreen();
        lastActivityTime = millis();
    }
    lv_timer_handler(); 
    delay(5); 

    lv_obj_t * currentScreen = lv_disp_get_scr_act(NULL);

    // ==========================================
    // MAIN BUTTON: LIGHT SLEEP / DEEP SLEEP
    // ==========================================
    if (digitalRead(BTN_MAIN) == HIGH) {
        if (!mainBtnIsPressed) {
            if (millis() - mainBtnReleaseTime > 50) {
                mainBtnIsPressed = true;
                mainBtnPressTime = millis();
                deepSleepTriggered = false;
            }
        }
        
        if (mainBtnIsPressed) {
            unsigned long holdTime = millis() - mainBtnPressTime;
            
            if (!isScreenOff) {
                int arcVal = map(constrain(holdTime, 0, DEEP_SLEEP_HOLD_TIME_MS), 0, DEEP_SLEEP_HOLD_TIME_MS, 0, 100);
                
                if (currentScreen == ui_MainScreen && ui_SleepArcMain != NULL) lv_arc_set_value(ui_SleepArcMain, arcVal);
                else if (currentScreen == ui_GyroScreen && ui_SleepArcGyro != NULL) lv_arc_set_value(ui_SleepArcGyro, arcVal);
                else if (currentScreen == ui_LaserScreen && ui_SleepArcLaser != NULL) lv_arc_set_value(ui_SleepArcLaser, arcVal);
                
                lv_timer_handler(); 
                lv_refr_now(NULL);
            }

            if (holdTime >= DEEP_SLEEP_HOLD_TIME_MS) {
                deepSleepTriggered = true;
            }
        }
    } 
    else { 
        if (mainBtnIsPressed) {
            delay(30); 
            if (digitalRead(BTN_MAIN) == LOW) {
                mainBtnIsPressed = false;
                mainBtnReleaseTime = millis();
                
                if (deepSleepTriggered) {
                    enterDeepSleep();
                } 
                else {
                    if (!isScreenOff) {
                        if (ui_SleepArcMain != NULL) lv_arc_set_value(ui_SleepArcMain, 0);
                        if (ui_SleepArcGyro != NULL) lv_arc_set_value(ui_SleepArcGyro, 0);
                        if (ui_SleepArcLaser != NULL) lv_arc_set_value(ui_SleepArcLaser, 0);
                        lv_timer_handler();
                        lv_refr_now(NULL);
                    }
                    
                    if (isScreenOff) turnOnScreen();
                    else turnOffScreen(); 
                }
            }
        }
    }
    
    // ==========================================
    // AUTO TIMEOUT CHECKS
    // ==========================================
    // For receive testing this is false in WatchSecrets.h. Manual long-press stays enabled.
    if (WATCH_ALLOW_AUTO_DEEP_SLEEP && millis() - lastActivityTime >= DEEP_SLEEP_TIMEOUT_MS) enterDeepSleep();
    if (!isScreenOff && millis() - lastActivityTime >= IDLE_TIMEOUT_MS) turnOffScreen();

    // ==========================================
    // BUTTON WAKE & APP LOGIC
    // ==========================================
    if (isScreenOff) {
        // --- ASLEEP: ONLY LISTEN FOR WAKE COMMANDS ---
        if (digitalRead(BTN1) == HIGH || digitalRead(BTN2) == HIGH) {
            turnOnScreen();
            while(digitalRead(BTN1) == HIGH || digitalRead(BTN2) == HIGH) { delay(10); }
            btn1DebounceTime = btn2DebounceTime = millis(); 
        }
        // Notice we removed the 'return;' here so the clock still updates!
    } 
    else {
        // --- AWAKE: PROCESS NORMAL BUTTON CLICKS ---
        
        // BTN1: MENU / SELECT
        int reading1 = digitalRead(BTN1);
        if (reading1 != btn1State && (millis() - btn1DebounceTime) > 100) {
            btn1State = reading1;
            btn1DebounceTime = millis();
            
            if (btn1State == HIGH) { 
                lastActivityTime = millis(); 
                
                if (!isMenuOpen) {
                    if (ui_MenuScreen != NULL) {
                        lv_scr_load_anim(ui_MenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
                        isMenuOpen = true; 
                    }
                } 
                else {
                    uint16_t selectedOpt = lv_roller_get_selected(ui_UiMenuRoller);
                    
                    if (selectedOpt == 0) { 
                        if (ui_MainScreen != NULL) {
                            lv_scr_load_anim(ui_MainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
                            isMenuOpen = false; 
                        }
                    } 
                    else if (selectedOpt == 1) { 
                        if (ui_GyroScreen != NULL) {
                            lv_scr_load_anim(ui_GyroScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
                            isMenuOpen = false;
                        }
                    }
                    else if (selectedOpt == 2) { 
                        if (ui_LaserScreen != NULL) {
                            lv_scr_load_anim(ui_LaserScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
                            isMenuOpen = false;
                        }
                    }
                    else if (selectedOpt == 3) { 
                        Serial.println("Fitness App Selected.");
                    }
                }
            }
        }

        // BTN2: SEND PING (Main) / CYCLE (Menu) / TOGGLE LASER 
        int reading2 = digitalRead(BTN2);
        if (reading2 != btn2State && (millis() - btn2DebounceTime) > 100) {
            btn2State = reading2;
            btn2DebounceTime = millis();
            
            if (btn2State == HIGH) {
                lastActivityTime = millis(); 
                
                if (isMenuOpen) {
                    uint16_t currentOpt = lv_roller_get_selected(ui_UiMenuRoller);
                    uint16_t totalOpts = lv_roller_get_option_cnt(ui_UiMenuRoller);
                    uint16_t nextOpt = (currentOpt + 1) % totalOpts;
                    lv_roller_set_selected(ui_UiMenuRoller, nextOpt, LV_ANIM_ON);
                }
                else if (currentScreen == ui_MainScreen) {
                    // BTN2 on the main clock face sends to mirror VIA THE LAPTOP.
                    // Menu cycling and the existing laser-screen button stay unchanged.
                    watchLinkSendPing();
                }
                else if (currentScreen == ui_LaserScreen) {
                    laserState = !laserState; 
                    digitalWrite(LASER_EN, laserState ? HIGH : LOW); 
                    
                    if (ui_LaserSwitch != NULL) {
                        if (laserState) lv_obj_add_state(ui_LaserSwitch, LV_STATE_CHECKED);
                        else lv_obj_clear_state(ui_LaserSwitch, LV_STATE_CHECKED);
                    }
                }
            }
        }
    }

    // ==========================================
    // CLOCK, UI & BATTERY UPDATES (1 Hz)
    // THIS NOW RUNS EVEN WHEN isScreenOff == true!
    // ==========================================
    if (millis() - lastTimeUpdate >= 1000) {
        lastTimeUpdate = millis();
        
        DateTime now = rtc.now();
        int displayHour = now.hour() % 12;
        if (displayHour == 0) displayHour = 12; 
        
        char hourString[4]; 
        char minuteString[4]; 
        sprintf(hourString, "%02d", displayHour); 
        sprintf(minuteString, "%02d", now.minute());
        if (ui_UiHourLabel != NULL) lv_label_set_text(ui_UiHourLabel, hourString);
        if (ui_UiMinLabel != NULL)  lv_label_set_text(ui_UiMinLabel, minuteString);

        if (ui_StepsLabel != NULL) lv_label_set_text(ui_StepsLabel, "--");
        if (ui_StepsArc != NULL) lv_arc_set_value(ui_StepsArc, 0);

        bool isCharging = (digitalRead(PIN_CHG) == LOW); 
        float batPercentage = lipo.getSOC();
        int batPercent = constrain((int)batPercentage, 0, 100);

        char batString[16];
        if (isCharging) sprintf(batString, "CHG %d%%", batPercent);
        else sprintf(batString, "%d%%", batPercent);
        
        if (ui_UiBatLabel != NULL) lv_label_set_text(ui_UiBatLabel, batString);
        if (ui_UiBatBar != NULL)   lv_bar_set_value(ui_UiBatBar, batPercent, LV_ANIM_ON);
    }
}

// #include <Arduino.h>
// #include <TFT_eSPI.h>
// #include <Wire.h>
// #include <RTClib.h>
// #include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
// #include <lvgl.h>
// #include "ui.h" 
// #include <esp_sleep.h> 
// #include <driver/gpio.h>
// #include <math.h> 

// // --- PINS & ADDRESSES ---
// const int I2C_SDA = 13; 
// const int I2C_SCL = 14; 
// const int PIN_CHG = 21; 
// const int BTN_MAIN = 18; 

// // --- APP PINS ---
// const int LASER_EN = 6; 
// bool laserState = false; 

// // --- PWM BACKLIGHT SETTINGS ---
// const int PWM_CHANNEL = 0;
// const int PWM_FREQ = 5000;
// const int PWM_RES = 8; // 8-bit resolution (0-255)
// const int MAX_BRIGHTNESS = 255;
// const int DIM_BRIGHTNESS = 15; // Tweak this between 1 and 30 for the perfect dimness!

// // --- BUTTON PINS ---
// const int BTN1 = 48; 
// const int BTN2 = 47; 

// int btn1State = LOW;
// unsigned long btn1DebounceTime = 0;

// int btn2State = LOW;
// unsigned long btn2DebounceTime = 0;

// bool isMenuOpen = false;

// // --- POWER MANAGEMENT VARIABLES ---
// const unsigned long IDLE_TIMEOUT_MS = 300000;         // 5 minutes (Screen Dims)
// const unsigned long DEEP_SLEEP_TIMEOUT_MS = 1800000;  // 30 minutes (Full Shutdown)
// const unsigned long DEEP_SLEEP_HOLD_TIME_MS = 1500;   // 1.5 Seconds hold time
// unsigned long lastActivityTime = 0;
// bool isScreenOff = false;
// lv_obj_t * screenBeforeSleep = NULL; 

// // --- MAIN BUTTON STATE MACHINE ---
// bool mainBtnIsPressed = false;
// unsigned long mainBtnPressTime = 0;
// unsigned long mainBtnReleaseTime = 0;
// bool deepSleepTriggered = false;

// // --- RTC (DEEP SLEEP) MEMORY ---
// // 0 = Main, 1 = Gyro, 2 = Laser
// RTC_DATA_ATTR int savedScreenID = 0; 
// RTC_DATA_ATTR uint32_t savedCompileTime = 0;

// // --- HARDWARE OBJECTS ---
// RTC_DS3231 rtc; 
// SFE_MAX1704X lipo;
// TFT_eSPI tft = TFT_eSPI();

// void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
//     uint32_t w = (area->x2 - area->x1 + 1);
//     uint32_t h = (area->y2 - area->y1 + 1);
//     tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
//     lv_disp_flush_ready(disp_drv);
// }

// unsigned long lastTimeUpdate = 0;

// // --- SCREEN OFF (LIGHT SLEEP) HELPER ---
// void turnOffScreen() {
//     Serial.println("Going to Light Sleep (Dimming Screen)...");
    
//     // Smoothly fade the backlight down to the dim setting
//     for (int i = MAX_BRIGHTNESS; i >= DIM_BRIGHTNESS; i--) {
//         ledcWrite(PWM_CHANNEL, i);
//         delay(3);
//     }
    
//     isScreenOff = true;
// }

// // --- WAKE UP FROM LIGHT SLEEP HELPER ---
// void turnOnScreen() {
//     Serial.println("Waking Screen...");
    
//     // Smoothly fade the backlight back up to max
//     for (int i = DIM_BRIGHTNESS; i <= MAX_BRIGHTNESS; i++) {
//         ledcWrite(PWM_CHANNEL, i);
//         delay(2);
//     }
    
//     isScreenOff = false;
//     lastActivityTime = millis();
//     lastTimeUpdate = millis();
// }

// // --- DEEP SLEEP HELPER ---
// void enterDeepSleep() {
//     Serial.println("Preparing for Deep Sleep...");

//     lv_obj_t * currentScreen = lv_disp_get_scr_act(NULL);
//     if (currentScreen == ui_MainScreen) savedScreenID = 0;
//     else if (currentScreen == ui_GyroScreen) savedScreenID = 1;
//     else if (currentScreen == ui_LaserScreen) savedScreenID = 2;

//     digitalWrite(LASER_EN, LOW);
//     laserState = false;
//     if (ui_LaserSwitch != NULL) {
//         lv_obj_clear_state(ui_LaserSwitch, LV_STATE_CHECKED); 
//     }

//     // Smoothly fade the backlight completely to ZERO
//     int currentBrightness = isScreenOff ? DIM_BRIGHTNESS : MAX_BRIGHTNESS;
//     for (int i = currentBrightness; i >= 0; i--) {
//         ledcWrite(PWM_CHANNEL, i);
//         delay(3);
//     }

//     // Release PWM and safely lock the pin LOW to stop battery leaks
//     ledcDetachPin(TFT_BL);
//     pinMode(TFT_BL, OUTPUT);
//     digitalWrite(TFT_BL, LOW); 
//     gpio_hold_en((gpio_num_t)TFT_BL); 
//     gpio_deep_sleep_hold_en();
    
//     tft.writecommand(0x10); // Put TFT driver chip to sleep

//     // ONLY the Main Button can wake from Deep Sleep
//     esp_sleep_enable_ext0_wakeup(GPIO_NUM_18, 1); 
    
//     Serial.println("Goodnight!");
//     esp_deep_sleep_start();
// }

// void setup() {
//     gpio_hold_dis((gpio_num_t)TFT_BL);
//     ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
//     ledcAttachPin(TFT_BL, PWM_CHANNEL);
//     ledcWrite(PWM_CHANNEL, 0); // Keep screen black during boot sequence

//     Serial.begin(115200);
//     Serial.println("--- Booting WatchyUI ---");
    
//     pinMode(PIN_CHG, INPUT_PULLUP);
//     pinMode(BTN_MAIN, INPUT); 
    
//     pinMode(BTN1, INPUT_PULLDOWN); 
//     pinMode(BTN2, INPUT_PULLDOWN);

//     pinMode(LASER_EN, OUTPUT);
//     digitalWrite(LASER_EN, LOW); 

//     Wire.begin(I2C_SDA, I2C_SCL);
//     rtc.begin();

//     // --- SMART RTC SYNC ---
//     DateTime compiledTime = DateTime(F(__DATE__), F(__TIME__));
//     if (savedCompileTime != compiledTime.unixtime()) {
//         Serial.println("New code upload detected! Syncing RTC to PC time...");
//         rtc.adjust(compiledTime);
//         savedCompileTime = compiledTime.unixtime(); 
//     }

//     // Init MAX17048 Fuel Gauge
//     if (!lipo.begin()) Serial.println("MAX17048 not detected!");
//     else Serial.println("MAX17048 Initialized");

//     tft.begin();
//     tft.setRotation(0);
//     tft.fillScreen(TFT_BLACK);

//     lv_init();
//     static lv_disp_draw_buf_t draw_buf;
//     static lv_color_t buf[240 * 28]; 
//     lv_disp_draw_buf_init(&draw_buf, buf, NULL, 240 * 28);

//     static lv_disp_drv_t disp_drv;
//     lv_disp_drv_init(&disp_drv);
//     disp_drv.hor_res = 240;
//     disp_drv.ver_res = 280;
//     disp_drv.flush_cb = my_disp_flush;
//     disp_drv.draw_buf = &draw_buf;
//     lv_disp_drv_register(&disp_drv);

//     ui_init(); 
    
//     isMenuOpen = false;
//     isScreenOff = false;
    
//     if (savedScreenID == 1 && ui_GyroScreen != NULL) {
//         lv_disp_load_scr(ui_GyroScreen);
//     } 
//     else if (savedScreenID == 2 && ui_LaserScreen != NULL) {
//         lv_disp_load_scr(ui_LaserScreen);
//     } 
//     else if (ui_MainScreen != NULL) {
//         lv_disp_load_scr(ui_MainScreen);
//         savedScreenID = 0; 
//     }

//     // Fade the screen on for a premium boot effect!
//     for (int i = 0; i <= MAX_BRIGHTNESS; i++) {
//         ledcWrite(PWM_CHANNEL, i);
//         delay(2);
//     }

//     while(digitalRead(BTN_MAIN) == HIGH) { delay(10); }
//     lastActivityTime = millis(); 
//     Serial.println("--- Setup Complete ---");
// }

// void loop() {
//     lv_timer_handler(); 
//     delay(5); 

//     lv_obj_t * currentScreen = lv_disp_get_scr_act(NULL);

//     // ==========================================
//     // MAIN BUTTON: LIGHT SLEEP / DEEP SLEEP
//     // ==========================================
//     if (digitalRead(BTN_MAIN) == HIGH) {
//         if (!mainBtnIsPressed) {
//             if (millis() - mainBtnReleaseTime > 50) {
//                 mainBtnIsPressed = true;
//                 mainBtnPressTime = millis();
//                 deepSleepTriggered = false;
//             }
//         }
        
//         if (mainBtnIsPressed) {
//             unsigned long holdTime = millis() - mainBtnPressTime;
            
//             if (!isScreenOff) {
//                 int arcVal = map(constrain(holdTime, 0, DEEP_SLEEP_HOLD_TIME_MS), 0, DEEP_SLEEP_HOLD_TIME_MS, 0, 100);
                
//                 if (currentScreen == ui_MainScreen && ui_SleepArcMain != NULL) lv_arc_set_value(ui_SleepArcMain, arcVal);
//                 else if (currentScreen == ui_GyroScreen && ui_SleepArcGyro != NULL) lv_arc_set_value(ui_SleepArcGyro, arcVal);
//                 else if (currentScreen == ui_LaserScreen && ui_SleepArcLaser != NULL) lv_arc_set_value(ui_SleepArcLaser, arcVal);
                
//                 lv_timer_handler(); 
//                 lv_refr_now(NULL);
//             }

//             if (holdTime >= DEEP_SLEEP_HOLD_TIME_MS) {
//                 deepSleepTriggered = true;
//             }
//         }
//     } 
//     else { 
//         if (mainBtnIsPressed) {
//             delay(30); 
//             if (digitalRead(BTN_MAIN) == LOW) {
//                 mainBtnIsPressed = false;
//                 mainBtnReleaseTime = millis();
                
//                 if (deepSleepTriggered) {
//                     enterDeepSleep();
//                 } 
//                 else {
//                     if (!isScreenOff) {
//                         if (ui_SleepArcMain != NULL) lv_arc_set_value(ui_SleepArcMain, 0);
//                         if (ui_SleepArcGyro != NULL) lv_arc_set_value(ui_SleepArcGyro, 0);
//                         if (ui_SleepArcLaser != NULL) lv_arc_set_value(ui_SleepArcLaser, 0);
//                         lv_timer_handler();
//                         lv_refr_now(NULL);
//                     }
                    
//                     if (isScreenOff) turnOnScreen();
//                     else turnOffScreen(); 
//                 }
//             }
//         }
//     }
    
//     // ==========================================
//     // AUTO TIMEOUT CHECKS
//     // ==========================================
//     if (millis() - lastActivityTime >= DEEP_SLEEP_TIMEOUT_MS) enterDeepSleep();
//     if (!isScreenOff && millis() - lastActivityTime >= IDLE_TIMEOUT_MS) turnOffScreen();

//     // ==========================================
//     // BUTTON WAKE & APP LOGIC
//     // ==========================================
//     if (isScreenOff) {
//         // --- ASLEEP: ONLY LISTEN FOR WAKE COMMANDS ---
//         if (digitalRead(BTN1) == HIGH || digitalRead(BTN2) == HIGH) {
//             turnOnScreen();
//             while(digitalRead(BTN1) == HIGH || digitalRead(BTN2) == HIGH) { delay(10); }
//             btn1DebounceTime = btn2DebounceTime = millis(); 
//         }
//         // Notice we removed the 'return;' here so the clock still updates!
//     } 
//     else {
//         // --- AWAKE: PROCESS NORMAL BUTTON CLICKS ---
        
//         // BTN1: MENU / SELECT
//         int reading1 = digitalRead(BTN1);
//         if (reading1 != btn1State && (millis() - btn1DebounceTime) > 100) {
//             btn1State = reading1;
//             btn1DebounceTime = millis();
            
//             if (btn1State == HIGH) { 
//                 lastActivityTime = millis(); 
                
//                 if (!isMenuOpen) {
//                     if (ui_MenuScreen != NULL) {
//                         lv_scr_load_anim(ui_MenuScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
//                         isMenuOpen = true; 
//                     }
//                 } 
//                 else {
//                     uint16_t selectedOpt = lv_roller_get_selected(ui_UiMenuRoller);
                    
//                     if (selectedOpt == 0) { 
//                         if (ui_MainScreen != NULL) {
//                             lv_scr_load_anim(ui_MainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
//                             isMenuOpen = false; 
//                         }
//                     } 
//                     else if (selectedOpt == 1) { 
//                         if (ui_GyroScreen != NULL) {
//                             lv_scr_load_anim(ui_GyroScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
//                             isMenuOpen = false;
//                         }
//                     }
//                     else if (selectedOpt == 2) { 
//                         if (ui_LaserScreen != NULL) {
//                             lv_scr_load_anim(ui_LaserScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
//                             isMenuOpen = false;
//                         }
//                     }
//                     else if (selectedOpt == 3) { 
//                         Serial.println("Fitness App Selected.");
//                     }
//                 }
//             }
//         }

//         // BTN2: CYCLE (Menu) / TOGGLE LASER 
//         int reading2 = digitalRead(BTN2);
//         if (reading2 != btn2State && (millis() - btn2DebounceTime) > 100) {
//             btn2State = reading2;
//             btn2DebounceTime = millis();
            
//             if (btn2State == HIGH) {
//                 lastActivityTime = millis(); 
                
//                 if (isMenuOpen) {
//                     uint16_t currentOpt = lv_roller_get_selected(ui_UiMenuRoller);
//                     uint16_t totalOpts = lv_roller_get_option_cnt(ui_UiMenuRoller);
//                     uint16_t nextOpt = (currentOpt + 1) % totalOpts;
//                     lv_roller_set_selected(ui_UiMenuRoller, nextOpt, LV_ANIM_ON);
//                 }
//                 else if (currentScreen == ui_LaserScreen) {
//                     laserState = !laserState; 
//                     digitalWrite(LASER_EN, laserState ? HIGH : LOW); 
                    
//                     if (ui_LaserSwitch != NULL) {
//                         if (laserState) lv_obj_add_state(ui_LaserSwitch, LV_STATE_CHECKED);
//                         else lv_obj_clear_state(ui_LaserSwitch, LV_STATE_CHECKED);
//                     }
//                 }
//             }
//         }
//     }

//     // ==========================================
//     // CLOCK, UI & BATTERY UPDATES (1 Hz)
//     // THIS NOW RUNS EVEN WHEN isScreenOff == true!
//     // ==========================================
//     if (millis() - lastTimeUpdate >= 1000) {
//         lastTimeUpdate = millis();
        
//         DateTime now = rtc.now();
//         int displayHour = now.hour() % 12;
//         if (displayHour == 0) displayHour = 12; 
        
//         char hourString[4]; 
//         char minuteString[4]; 
//         sprintf(hourString, "%02d", displayHour); 
//         sprintf(minuteString, "%02d", now.minute());
//         if (ui_UiHourLabel != NULL) lv_label_set_text(ui_UiHourLabel, hourString);
//         if (ui_UiMinLabel != NULL)  lv_label_set_text(ui_UiMinLabel, minuteString);

//         if (ui_StepsLabel != NULL) lv_label_set_text(ui_StepsLabel, "--");
//         if (ui_StepsArc != NULL) lv_arc_set_value(ui_StepsArc, 0);

//         bool isCharging = (digitalRead(PIN_CHG) == LOW); 
//         float batPercentage = lipo.getSOC();
//         int batPercent = constrain((int)batPercentage, 0, 100);

//         char batString[16];
//         if (isCharging) sprintf(batString, "CHG %d%%", batPercent);
//         else sprintf(batString, "%d%%", batPercent);
        
//         if (ui_UiBatLabel != NULL) lv_label_set_text(ui_UiBatLabel, batString);
//         if (ui_UiBatBar != NULL)   lv_bar_set_value(ui_UiBatBar, batPercent, LV_ANIM_ON);
//     }
// }