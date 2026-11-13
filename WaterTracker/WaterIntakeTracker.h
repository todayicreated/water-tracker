#pragma once
#include <stdint.h>
#include <math.h>

// Hardware-independent lift / stable return detector. All masses are gross grams
// above the empty platform; 1 g of water is treated as approximately 1 ml.
class WaterIntakeTracker {
public:
  void reset(float emptyGrams) {
    empty=emptyGrams; phase=SEEK; haveWindow=false; liftCandidate=false;
  }
  float update(float grams,uint32_t now) {
    if(!isfinite(grams)||grams < -25 || grams > 15000) {
      haveWindow=false; liftCandidate=false; return 0;
    }
    if(!haveWindow) startWindow(grams,now);
    else {
      low=fminf(low,grams); high=fmaxf(high,grams); samples++;
      if(high-low>3.0f) startWindow(grams,now);
    }
    bool stable=samples>=5 && uint32_t(now-windowAt)>=1000;
    float presence=fmaxf(1.0f,empty-fmaxf(5.0f,empty*0.15f));
    float liftLimit=fmaxf(3.0f,empty*0.20f);
    if(phase==PRESENT && grams<liftLimit) {
      if(!liftCandidate) { liftCandidate=true; liftAt=now; }
      if(uint32_t(now-liftAt)>=180) {
        phase=LIFTED; liftCandidate=false;
        // Start a fresh stability window so lifting cannot itself be a return.
        startWindow(grams,now);
      }
      return 0;
    }
    liftCandidate=false;
    if(!stable || grams<presence) return 0;
    if(phase==LIFTED) {
      float difference=before-grams;
      float available=fmaxf(0.0f,before-empty);
      before=grams; phase=PRESENT;
      if(difference>=5.0f) return fminf(difference,available);
      return 0; // Same mass or refill: establish new baseline without credit.
    }
    before=grams; phase=PRESENT; // First placement or a stable on-platform change.
    return 0;
  }
private:
  enum Phase { SEEK, PRESENT, LIFTED } phase=SEEK;
  float empty=35,before=0,low=0,high=0;
  bool haveWindow=false,liftCandidate=false;
  uint32_t windowAt=0,liftAt=0,samples=0;
  void startWindow(float grams,uint32_t now) {
    low=high=grams; windowAt=now; samples=1; haveWindow=true;
  }
};
