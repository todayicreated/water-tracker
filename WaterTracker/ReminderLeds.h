#pragma once
#include <stdint.h>
struct ReminderLedColour {const char *name;uint8_t r,g,b;};
static constexpr ReminderLedColour LED_COLOURS[]={
  {"BLUE",0,90,255},{"RED",255,0,0},{"ORANGE",255,80,0},
  {"YELLOW",255,190,0},{"GREEN",0,255,0},{"CYAN",0,255,255},
  {"PURPLE",150,0,255},{"PINK",255,0,100},{"WHITE",255,255,255}
};
static constexpr int LED_COLOUR_COUNT=sizeof(LED_COLOURS)/sizeof(LED_COLOURS[0]);
enum class LedFadeKind { HOLD, DARK, RISE, FALL };
struct LedFadeStage {uint32_t ms;LedFadeKind kind;};
static constexpr LedFadeStage LED_FADE_STAGES[]={
  {700,LedFadeKind::HOLD},{1000,LedFadeKind::FALL},{250,LedFadeKind::DARK},
  {1000,LedFadeKind::RISE},{350,LedFadeKind::HOLD},{1200,LedFadeKind::FALL},
  {350,LedFadeKind::DARK},{1000,LedFadeKind::RISE},{350,LedFadeKind::HOLD},
  {1400,LedFadeKind::FALL},{1200,LedFadeKind::DARK}
};
struct ReminderLedPattern {
  static constexpr uint32_t stepMs=450,frameMs=16;
  static uint32_t cycleMs(int count){
    uint32_t duration=count*stepMs;
    for(const auto &stage:LED_FADE_STAGES) duration+=stage.ms;
    return duration;
  }
  static uint8_t fade(uint32_t age,uint32_t duration,bool rising){
    float t=age/float(duration);
    if(t>1) t=1;
    float eased=t*t*(3.0f-2.0f*t);
    if(!rising) eased=1.0f-eased;
    // Approximate perceptual correction: an even-looking ramp at low brightness.
    return uint8_t(255.0f*eased*eased+0.5f);
  }
  static uint8_t levelAt(uint32_t elapsed,int pixel,int count){
    uint32_t phase=elapsed%cycleMs(count),fillTime=count*stepMs;
    if(phase<fillTime){
      int current=phase/stepMs;
      if(pixel<current) return 255;
      if(pixel>current) return 0;
      return fade(phase%stepMs,stepMs,true);
    }
    phase-=fillTime;
    for(const auto &stage:LED_FADE_STAGES){
      if(phase<stage.ms){
        if(stage.kind==LedFadeKind::HOLD) return 255;
        if(stage.kind==LedFadeKind::DARK) return 0;
        return fade(phase,stage.ms,stage.kind==LedFadeKind::RISE);
      }
      phase-=stage.ms;
    }
    return 0;
  }
};
