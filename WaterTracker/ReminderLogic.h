#pragma once
#include <stdint.h>
#include <math.h>

// Existing nonnegative settings remain minutes; -1 is the 10-second preview.
struct ReminderTiming {
  static constexpr uint32_t intervalMs(int setting) {
    return setting==-1 ? 10000UL : (setting>0 ? uint32_t(setting)*60000UL : 0UL);
  }
};

struct ReminderGeometry {
  static constexpr int width=320,height=172,cx=160,cy=86;
  static constexpr int initialRadius=18,finalRadius=182;
  static constexpr uint32_t durationMs=10000,frameMs=33;
  static int radiusAt(uint32_t elapsed) {
    if(elapsed>=durationMs) return finalRadius;
    float t=elapsed/float(durationMs);
    float eased=t*t*(3.0f-2.0f*t);
    int radius=initialRadius+int((finalRadius-initialRadius)*eased);
    return radius<finalRadius?radius:finalRadius-1;
  }
  static int extent(int radius,int y) {
    int dy=y-cy,square=radius*radius-dy*dy;
    return square<0 ? -1 : int(sqrtf(float(square)));
  }
};

// Feed gross grams continuously while browsing, including settings overview/menu.
// Return true once per confirmed pickup, never merely because the scale is empty.
class ReminderLiftDetector {
public:
  void reset(){armed=false;candidate=false;}
  bool update(float grams,float empty,uint32_t now) {
    if(!isfinite(grams)||grams < -25||grams>15000||empty<1) {reset();return false;}
    float present=fmaxf(1.0f,empty-fmaxf(5.0f,empty*0.15f));
    if(grams>=present){armed=true;candidate=false;return false;}
    if(armed && grams<fmaxf(3.0f,empty*0.20f)) {
      if(!candidate){candidate=true;since=now;}
      if(uint32_t(now-since)>=180){armed=false;candidate=false;return true;}
    } else candidate=false;
    return false;
  }
private:
  bool armed=false,candidate=false;
  uint32_t since=0;
};
