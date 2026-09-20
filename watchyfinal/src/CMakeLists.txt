SET(SOURCES ui_MainScreen.c
    ui_MenuScreen.c
    ui_LaserScreen.c
    ui_GyroScreen.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    fonts/ui_font_FontHour1000.c
    fonts/ui_font_FontHour10000.c
    fonts/ui_font_FontHour100000.c
    fonts/ui_font_FontMin100.c
    fonts/ui_font_FontMin10000.c
    fonts/ui_font_FontMin100000.c
    fonts/ui_font_FontMin5.c)

add_library(ui ${SOURCES})
