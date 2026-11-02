// lcd.h - 16x2 LCD (I2C) that shows alerts and entry/exit messages (added by Member 2).
#pragma once

// Starts the LCD task. Safe to call once, after access_start().
void lcd_start(void);

// Shows two lines (max 16 characters each) for hold_ms milliseconds.
// Never blocks: safe to call from any task, but call it AFTER releasing any mutex.
// Does nothing if the LCD was not started.
void lcd_show(const char *line1, const char *line2, int hold_ms);
