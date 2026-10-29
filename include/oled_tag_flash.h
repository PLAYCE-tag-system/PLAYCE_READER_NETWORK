#pragma once

#include <Adafruit_SSD1306.h>
#include <Arduino.h>

class OledTagFlash {
 public:
  void trigger(Adafruit_SSD1306 &display) {
    if (phase_ < kLastPhase) return;
    phase_ = 0;
    phaseStartedAt_ = millis();
    display.invertDisplay(true);
  }

  void update(Adafruit_SSD1306 &display) {
    if (phase_ >= kLastPhase || millis() - phaseStartedAt_ < kPhaseMs) return;
    phaseStartedAt_ = millis();
    ++phase_;
    display.invertDisplay(phase_ % 2 == 0);
  }

 private:
  static constexpr uint8_t kLastPhase = 19;
  static constexpr unsigned long kPhaseMs = 80;
  uint8_t phase_ = kLastPhase;
  unsigned long phaseStartedAt_ = 0;
};
