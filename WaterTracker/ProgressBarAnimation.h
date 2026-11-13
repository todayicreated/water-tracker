#pragma once
#include <stdint.h>
struct ProgressBarAnimation {
  static constexpr uint32_t colourInMs=200,growMs=1100,holdMs=150,colourOutMs=650,frameMs=33;
  static constexpr uint32_t durationMs=colourInMs+growMs+holdMs+colourOutMs;
  bool active=false;
  uint32_t began=0,lastFrame=0;
  int fromWidth=0,toWidth=0,drawnWidth=0;
  uint16_t drawnColour=0xFFFF;
  void start(uint32_t now,int from,int to){
    active=true;began=now;lastFrame=now-frameMs;
    fromWidth=from;toWidth=to;drawnWidth=from;drawnColour=0xFFFF;
  }
  void stop(){active=false;}
  bool frameDue(uint32_t now)const{return active&&uint32_t(now-lastFrame)>=frameMs;}
  static float eased(float t){if(t<0)t=0;if(t>1)t=1;return t*t*(3-2*t);}
  int widthAt(uint32_t elapsed)const{
    if(elapsed<=colourInMs)return fromWidth;
    if(elapsed>=colourInMs+growMs)return toWidth;
    return fromWidth+int((toWidth-fromWidth)*eased((elapsed-colourInMs)/float(growMs))+0.5f);
  }
  static uint8_t blueAt(uint32_t elapsed){
    if(elapsed<colourInMs)return uint8_t(255*eased(elapsed/float(colourInMs))+0.5f);
    if(elapsed<colourInMs+growMs+holdMs)return 255;
    if(elapsed>=durationMs)return 0;
    return uint8_t(255*(1-eased((elapsed-colourInMs-growMs-holdMs)/float(colourOutMs)))+0.5f);
  }
};
