#pragma once
// All functions except watchLinkConnected() are called from Arduino loop/setup only.
bool watchLinkBegin();        // After ui_init(). Starts network task.
bool watchLinkPoll();         // Updates LVGL; true on NEW received notification.
bool watchLinkSendPing();     // Main loop button action; result arrives later.
void watchLinkPrepareSleep(); // Stop networking before existing deep sleep.
bool watchLinkConnected();    // Authenticated connection to LAPTOP, not Pi.
