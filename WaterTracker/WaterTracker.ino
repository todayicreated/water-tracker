// ESP32 water tracker firmware. See the repository README for hardware and setup.
#include <Arduino.h>
#include <SPI.h>
#include <FS.h> // Export FS names before TFT_eSPI defines FS_NO_GLOBALS.
#include <TFT_eSPI.h>
#if !defined(USER_SETUP_ID) || USER_SETUP_ID != 147
#error "Water tracker display setup missing: keep build_opt.h beside this sketch and use ESP32 core 3.x."
#endif
#include <Adafruit_NeoPixel.h>
#include "TrackerSans-Bold15.h"
#include "TrackerSans-Bold20.h"
#include "TrackerSans-Regular12.h"
#include "Jersey10-Clock136.h"
#include "Jersey10-Digits96.h"
#include "WaterIntakeTracker.h"
#include "WaterUpdateAnimation.h"
#include "ProgressBarAnimation.h"
#include "ReminderLogic.h"
#include "ReminderLeds.h"
#include "ReminderTextMask.h"
#include "src/hx711/HX711.h" // Use the bundled HX711 driver.
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_system.h>
#include <time.h>
#include <math.h>

constexpr int TFT_BL=22;
constexpr int ENC_A=33, ENC_B=32, ENC_SW=19, BACK_PIN=21;
constexpr int HX_DT=16, HX_SCK=17, LED_PIN=13, LED_COUNT=10;
constexpr uint16_t BLACK=0x0000, WHITE=0xFFFF, GREY=0x5AEB, ROW_SELECTED=0x9492,
                   BLUE=0x027F, RED=0xF800, YELLOW=0xFEA0, CYAN=0x04BF;
constexpr uint16_t WATER_BLUE=0x54FF; // RGB(85, 158, 252) quantized to RGB565.
constexpr uint32_t SETTINGS_MAGIC=0x57545202;
const char *PROFILE_NAMES[]={"B1", "B2", "G1", "G2"};
constexpr int PROFILE_COUNT=4;
constexpr int SETTINGS_ROWS=7, SETTINGS_VISIBLE=6, SETTINGS_PITCH=22, NAV_POSITIONS=3+SETTINGS_ROWS;
constexpr int ROW_CONTAINER_ZERO=3, ROW_PLATFORM_ZERO=4, ROW_CALIBRATE=5, ROW_LED_COLOUR=6;
constexpr float UNCALIBRATED_STABILITY_COUNTS=1500;

// Reuse a 320 × 40 pixel sprite to limit RAM use when redrawing the display.
TFT_eSPI tft;
TFT_eSprite canvas(&tft);
int tileY=0;
uint16_t textBackground=BLACK;
Adafruit_NeoPixel strip(LED_COUNT,LED_PIN,NEO_GRB+NEO_KHZ800);
HX711 scale;
WiFiManager wifi;
Preferences prefs;
struct Settings {
  uint32_t magic=SETTINGS_MAGIC;
  int32_t goal=1500, reminder=30, profile=0;
  // Example empty masses in grams. The container zero wizard replaces each value.
  float emptyG[PROFILE_COUNT]={35,150,250,350};
  bool measured[PROFILE_COUNT]={false,false,false,false};
  float countsPerGram=0; // Zero indicates that scale calibration is required.
  int32_t platformZero=0;
};
Settings cfg;
uint8_t ledColour=0; // Stored separately to preserve the existing settings/calibration blob.
struct DailyIntake {
  uint32_t magic=0x57444901;
  int32_t day=0;
  float ml=0;
};
DailyIntake intake;
WaterIntakeTracker drinkTracker;
ReminderLiftDetector reminderLift;
int progressPercent() {
  return constrain(int(intake.ml*100.0f/cfg.goal),0,100);
}
void saveIntake() {
  if(prefs.putBytes("intake",&intake,sizeof(intake))!=sizeof(intake))
    Serial.println("ERROR: saving daily intake failed");
}
enum Mode { PAGES, MENU, EDIT, ZERO_CONFIRM, CAL_EMPTY, CAL_MASS,
            CAL_LOAD, SAMPLING, MESSAGE, CAL_DONE,
            ZERO_TARGET, CONTAINER_SELECT, CONTAINER_PLACE, CONTAINER_ZERO_TARGET, ZERO_DONE };
Mode mode=PAGES, returnMode=MENU;
int page=2, row=0, draft=0, knownMass=20;
float calibrationEmpty=0;
int zeroProfile=0,zeroReturnRow=ROW_PLATFORM_ZERO;
bool waterUpdatePending=false,progressUpdatePending=false;
ProgressBarAnimation progressUpdate;
WaterUpdateAnimation waterUpdate;
int waterTargetMl=0,waterTargetGoal=1500,waterTargetPercent=0;
int waterSourceMl=0,waterSourceGoal=1500,waterSourcePercent=0;
float waterSurface[320];
WaterDirtySpan waterDirty[WaterUpdateAnimation::blocks];
String message;
bool dirty=true;
uint32_t messageUntil=0, lastClock=0, reminderStart=0;
bool reminderDue=false;

// Encoder ISR accumulates complete quadrature cycles while SPI/network are busy.
portMUX_TYPE encoderMux=portMUX_INITIALIZER_UNLOCKED;
volatile int encoderSteps=0;
volatile uint8_t encoderPrevious=0;
volatile int8_t encoderPartial=0;
// ISR lookup must remain readable while NVS writes temporarily disable flash.
DRAM_ATTR static const int8_t encoderTransitions[16]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
void IRAM_ATTR encoderISR() {
  uint8_t current=(digitalRead(ENC_A)<<1)|digitalRead(ENC_B);
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderPartial+=encoderTransitions[(encoderPrevious<<2)|current];
  encoderPrevious=current;
  if(encoderPartial>=4) { encoderSteps++; encoderPartial=0; }
  if(encoderPartial<=-4) { encoderSteps--; encoderPartial=0; }
  portEXIT_CRITICAL_ISR(&encoderMux);
}
struct Button {
  uint8_t pin; bool raw=true, stable=true, held=false;
  uint32_t changed=0, pressed=0;
  // 1=short release, 2=long press (never followed by a short event)
  int poll() {
    bool value=digitalRead(pin);
    if(value!=raw) { raw=value; changed=millis(); }
    if(millis()-changed>=30 && raw!=stable) {
      stable=raw;
      if(!stable) { pressed=millis(); held=false; }
      else if(!held) return 1;
    }
    if(!stable && !held && millis()-pressed>=800) { held=true; return 2; }
    return 0;
  }
};
Button enter{ENC_SW}, back{BACK_PIN};

// Network worker only sends timezone results through a queue. Display, scale,
// Preferences and settings remain owned by the Arduino loop.
struct ZoneResult { int32_t offset; char name[64]; };
QueueHandle_t zoneQueue;
int32_t utcOffset=0;
bool zoneKnown=false;
String responseBody;
esp_err_t httpEvent(esp_http_client_event_t *event) {
  if(event->event_id==HTTP_EVENT_ON_DATA) {
    if(responseBody.length()+event->data_len>8192) return ESP_FAIL;
    responseBody.concat(static_cast<const char*>(event->data),event->data_len);
  }
  return ESP_OK;
}
void networkWorker(void *) {
  for(;;) {
    bool success=false;
    if(WiFi.status()==WL_CONNECTED && time(nullptr)>1700000000) {
      responseBody="";
      esp_http_client_config_t request={};
      request.url="https://ipwho.is/?fields=success,timezone";
      request.crt_bundle_attach=esp_crt_bundle_attach;
      request.event_handler=httpEvent;
      request.timeout_ms=7000;
      request.buffer_size=1024;
      esp_http_client_handle_t client=esp_http_client_init(&request);
      if(client) {
        if(esp_http_client_perform(client)==ESP_OK &&
           esp_http_client_get_status_code(client)==200) {
          JsonDocument json;
          if(!deserializeJson(json,responseBody) && json["success"]==true &&
             json["timezone"]["offset"].is<int32_t>()) {
            ZoneResult zone{};
            zone.offset=json["timezone"]["offset"].as<int32_t>();
            strlcpy(zone.name,json["timezone"]["id"] | "Unknown",sizeof(zone.name));
            if(zone.offset>=-43200 && zone.offset<=50400) {
              xQueueOverwrite(zoneQueue,&zone); success=true;
            }
          }
        }
        esp_http_client_cleanup(client);
      }
    }
    // Refresh location and current DST offset every 15 minutes; retry failures.
    vTaskDelay(pdMS_TO_TICKS(success ? 900000 : 30000));
  }
}
void save() {
  if(prefs.putBytes("settings",&cfg,sizeof(cfg))!=sizeof(cfg))
    Serial.println("ERROR: saving settings failed");
}
void saveLedColour() {
  if(prefs.putUChar("ledColour",ledColour)!=1) Serial.println("ERROR: saving LED colour failed");
}
bool validSettings(const Settings &s) {
  if(s.magic!=SETTINGS_MAGIC || s.goal<100 || s.goal>10000 ||
     s.reminder < -1 || s.reminder>240 || s.profile<0 || s.profile>=PROFILE_COUNT ||
     !isfinite(s.countsPerGram) ||
     (s.countsPerGram!=0 && fabs(s.countsPerGram)<0.001f)) return false;
  for(float g:s.emptyG) if(!isfinite(g)||g<0||g>10000) return false;
  return true;
}
void serviceLeds(uint32_t now) {
  static bool wasReminder=false;
  static uint32_t started=0,lastFrame=0;
  static int previousColour=-1;
  static uint32_t previousPixels[LED_COUNT]={};
  bool starting=reminderDue && !wasReminder;
  if(starting) started=now;
  wasReminder=reminderDue;
  bool preview=mode==EDIT && row==ROW_LED_COLOUR;
  int colour=preview?draft:ledColour;
  if(reminderDue && !starting && colour==previousColour &&
     uint32_t(now-lastFrame)<ReminderLedPattern::frameMs) return;
  lastFrame=now; previousColour=colour;
  const ReminderLedColour &rgb=LED_COLOURS[colour];
  bool changed=false;
  for(int i=0;i<LED_COUNT;i++) {
    uint8_t level=reminderDue?ReminderLedPattern::levelAt(uint32_t(now-started),i,LED_COUNT):
                              (preview?255:0);
    uint32_t pixel=strip.Color((uint16_t(rgb.r)*level+127)/255,
                              (uint16_t(rgb.g)*level+127)/255,
                              (uint16_t(rgb.b)*level+127)/255);
    if(pixel!=previousPixels[i]) {
      strip.setPixelColor(i,pixel); previousPixels[i]=pixel; changed=true;
    }
  }
  if(changed) strip.show();
}
void notify(const String &s, Mode destination=MENU) {
  message=s; returnMode=destination; mode=MESSAGE;
  messageUntil=millis()+3500; dirty=true;
}
void acknowledgeReminder() {
  reminderDue=false; reminderStart=millis(); strip.clear(); strip.show(); dirty=true;
}
void serviceIntake() {
  time_t utc=time(nullptr);
  if(utc>1700000000 && zoneKnown) {
    time_t local=utc+utcOffset; struct tm parts; gmtime_r(&local,&parts);
    int32_t day=(parts.tm_year+1900)*10000+(parts.tm_mon+1)*100+parts.tm_mday;
    if(day!=intake.day) {
      // Undated first-boot intake belongs to the first synchronized local day.
      if(intake.day!=0) intake.ml=0;
      intake.day=day; saveIntake(); dirty=true;
    }
  }
  static int previousProfile=-1;
  static float previousFactor=0,previousEmpty=0;
  static int32_t previousZero=0;
  static uint32_t lastRead=0,lastReady=0;
  uint32_t now=millis();
  float empty=cfg.emptyG[cfg.profile];
  // Watch pickups while browsing settings as well. Editors/calibration never
  // feed either detector; settings measurements cannot become drinking events.
  if((mode!=PAGES && mode!=MENU && !reminderDue) || cfg.countsPerGram==0 || empty<1) {
    drinkTracker.reset(empty); reminderLift.reset(); lastReady=now; return;
  }
  if(previousProfile!=cfg.profile || previousFactor!=cfg.countsPerGram ||
     previousZero!=cfg.platformZero || previousEmpty!=empty) {
    drinkTracker.reset(empty); reminderLift.reset();
    previousProfile=cfg.profile; previousFactor=cfg.countsPerGram;
    previousZero=cfg.platformZero; previousEmpty=empty;
  }
  if(now-lastReady>3000) { drinkTracker.reset(empty); reminderLift.reset(); }
  if(now-lastRead<100 || !scale.is_ready()) return;
  lastRead=now;
  float raw=scale.read(10);
  if(scale.getLastError()!=HX711_OK || raw<=-8388607 || raw>=8388607) {
    drinkTracker.reset(empty); reminderLift.reset(); return;
  }
  lastReady=now;
  float grams=(raw-cfg.platformZero)/cfg.countsPerGram;
  bool pickedUp=reminderLift.update(grams,empty,now);
  if(reminderDue && pickedUp) acknowledgeReminder();
  if(mode!=PAGES || page==2) { drinkTracker.reset(empty); return; }
  float consumed=drinkTracker.update(grams,now);
  if(consumed>0) {
    intake.ml=fminf(100000.0f,intake.ml+consumed);
    if(page==0 && mode==PAGES) waterUpdatePending=true;
    if(page==1 && mode==PAGES) progressUpdatePending=true;
    saveIntake();
    // A return can credit a drink, but an active alert has only two dismissal
    // paths: a newly detected pickup or the separate button.
    if(!reminderDue) acknowledgeReminder();
    else dirty=true;
    Serial.printf("Drink: %.1f ml; daily total: %.1f ml\n",consumed,intake.ml);
  }
}

void cancel() {
  if(reminderDue) return;
  // An unfinished calibration never changes saved calibration/zero.
  if(mode==PAGES) return;
  if(mode==MENU) mode=PAGES;
  else mode=MENU;
  dirty=true;
}

// Samples are collected only when HX711 is ready: disconnected sensors do not
// block input, and calibration is committed only after both stages succeed.
enum SampleJob { ZERO_PLATFORM, CAL_ZERO, CAL_KNOWN, EMPTY_CONTAINER };
void startSamples(SampleJob job);
SampleJob sampleJob;
int sampleCount=0;
int64_t sampleSum=0;
long sampleMin=0, sampleMax=0;
uint32_t sampleStarted=0;
void startSamples(SampleJob job) {
  sampleJob=job; sampleCount=0; sampleSum=0;
  sampleMin=LONG_MAX; sampleMax=LONG_MIN; sampleStarted=millis();
  mode=SAMPLING; dirty=true;
}
void sampleError(const String &error) {
  Mode retry=sampleJob==CAL_ZERO?CAL_EMPTY:(sampleJob==CAL_KNOWN?CAL_LOAD:
             (sampleJob==ZERO_PLATFORM?ZERO_TARGET:CONTAINER_ZERO_TARGET));
  notify(error,retry);
}
void serviceSamples() {
  if(mode!=SAMPLING) return;
  if(millis()-sampleStarted>10000) { sampleError("Scale timeout. Try again."); return; }
  if(!scale.is_ready()) return;
  long raw=scale.read();
  if(raw<=-8388607 || raw>=8388607) { sampleError("Scale overload / wiring error"); return; }
  sampleSum+=raw; sampleMin=min(sampleMin,raw); sampleMax=max(sampleMax,raw);
  if(++sampleCount<20) return;
  float tolerance=cfg.countsPerGram==0 ? UNCALIBRATED_STABILITY_COUNTS :
                                          fabs(cfg.countsPerGram)*3.0f;
  if(sampleMax-sampleMin>tolerance) { sampleError("Unstable. Keep scale still."); return; }
  float average=static_cast<float>(sampleSum)/sampleCount;
  switch(sampleJob) {
    case ZERO_PLATFORM:
      cfg.platformZero=lroundf(average); save();
      mode=ZERO_DONE; messageUntil=millis()+2500; dirty=true; break;
    case CAL_ZERO:
      calibrationEmpty=average; mode=CAL_MASS; dirty=true; break;
    case CAL_KNOWN: {
      float delta=average-calibrationEmpty;
      if(fabs(delta)<max(1500.0f,tolerance*5)) { sampleError("No clear weight. Try again."); break; }
      cfg.countsPerGram=delta/knownMass;
      cfg.platformZero=lroundf(calibrationEmpty);
      save(); mode=CAL_DONE; messageUntil=millis()+2500; dirty=true; break;
    }
    case EMPTY_CONTAINER: {
      float grams=(average-cfg.platformZero)/cfg.countsPerGram;
      if(!isfinite(grams)||grams<1||grams>10000) { sampleError("Zero platform, then retry."); break; }
      cfg.emptyG[zeroProfile]=grams; cfg.measured[zeroProfile]=true;
      cfg.profile=zeroProfile;
      save(); mode=ZERO_DONE; messageUntil=millis()+2500; dirty=true; break;
    }
  }
}
void rotate(int steps) {
  if(reminderDue || !steps) return;
  dirty=true;
  if(mode==PAGES || mode==MENU) {
    // One ten-position ring: three overview pages, then seven settings rows.
    int position=mode==MENU ? 3+row : page;
    position=((position+steps)%NAV_POSITIONS+NAV_POSITIONS)%NAV_POSITIONS;
    if(position<3) { page=position; mode=PAGES; }
    else { page=2; row=position-3; mode=MENU; }
  }
  else if(mode==EDIT) {
    if(row==0) draft=constrain(draft+steps*50,100,10000);
    if(row==1) {
      // OFF -> 10s preview -> 5min -> 10min ... 240min.
      int index=draft==-1?1:(draft==0?0:1+(draft+4)/5);
      index=constrain(index+steps,0,49);
      draft=index==0?0:(index==1?-1:(index-1)*5);
    }
    if(row==2) draft=((draft+steps)%PROFILE_COUNT+PROFILE_COUNT)%PROFILE_COUNT;
    if(row==ROW_LED_COLOUR) draft=((draft+steps)%LED_COLOUR_COUNT+LED_COLOUR_COUNT)%LED_COLOUR_COUNT;
  } else if(mode==CAL_MASS) knownMass=constrain(knownMass+steps*10,10,5000);
  else if(mode==CONTAINER_SELECT)
    zeroProfile=((zeroProfile+steps)%PROFILE_COUNT+PROFILE_COUNT)%PROFILE_COUNT;
}
void click(bool longPress) {
  if(reminderDue) return; // Only pickup or the separate button dismisses the alert.
  if(longPress) { cancel(); return; }
  switch(mode) {
    case PAGES:
      return; // Scrolling enters/leaves settings rows; overview pages need no press.
    case MENU:
      if(row<3) {
        draft=row==0?cfg.goal:(row==1?cfg.reminder:cfg.profile); mode=EDIT;
      } else if(row==ROW_CONTAINER_ZERO) {
        if(cfg.countsPerGram==0) notify("Calibrate the scale first.");
        else {
          zeroProfile=cfg.profile; zeroReturnRow=ROW_CONTAINER_ZERO; mode=CONTAINER_SELECT;
        }
      } else if(row==ROW_PLATFORM_ZERO) {
        zeroReturnRow=ROW_PLATFORM_ZERO; mode=ZERO_CONFIRM;
      } else if(row==ROW_LED_COLOUR) { draft=ledColour; mode=EDIT; }
      else mode=CAL_EMPTY;
      break;
    case EDIT:
      if(row==0) cfg.goal=draft;
      if(row==1) { cfg.reminder=draft; acknowledgeReminder(); }
      if(row==2) cfg.profile=draft;
      if(row==ROW_LED_COLOUR) { ledColour=draft; saveLedColour(); }
      else save();
      mode=MENU; break;
    case ZERO_CONFIRM: mode=ZERO_TARGET; break;
    case ZERO_TARGET: startSamples(ZERO_PLATFORM); break;
    case CONTAINER_SELECT: mode=CONTAINER_PLACE; break;
    case CONTAINER_PLACE: mode=CONTAINER_ZERO_TARGET; break;
    case CONTAINER_ZERO_TARGET: startSamples(EMPTY_CONTAINER); break;
    case CAL_EMPTY: startSamples(CAL_ZERO); break;
    case CAL_MASS: mode=CAL_LOAD; break;
    case CAL_LOAD: startSamples(CAL_KNOWN); break;
    case MESSAGE: mode=returnMode; break;
    case CAL_DONE: page=2; row=ROW_CALIBRATE; mode=MENU; break;
    case ZERO_DONE: page=2; row=zeroReturnRow; mode=MENU; break;
    case SAMPLING: break;
  }
  dirty=true;
}

// Reminder animation uses the existing stripe sprite plus a flash text mask.
// Only newly covered circle pixels are sent after its initial complete frame.
bool reminderVisualStarted=false;
uint32_t reminderVisualAt=0,reminderLastFrame=0;
int reminderRadius=-1;
uint16_t reminderSpan[320];
void resetReminderVisual() {
  reminderVisualStarted=false; reminderRadius=-1;
}
bool reminderFrameDue(uint32_t now) {
  return !reminderVisualStarted || (reminderRadius<ReminderGeometry::finalRadius &&
                                   uint32_t(now-reminderLastFrame)>=ReminderGeometry::frameMs);
}
uint16_t reminderTextPixel(uint8_t alpha,uint16_t background) {
  if(alpha==0) return background;
  if(alpha==255) return WHITE;
  unsigned inverse=255-alpha;
  unsigned r=(31*alpha+((background>>11)&31)*inverse+127)/255;
  unsigned g=(63*alpha+((background>>5)&63)*inverse+127)/255;
  unsigned b=(31*alpha+(background&31)*inverse+127)/255;
  return (r<<11)|(g<<5)|b;
}
void paintReminderSpan(int y,int left,int right) {
  if(right<left) return;
  if(y<REMINDER_MASK_TOP || y>=REMINDER_MASK_TOP+REMINDER_MASK_HEIGHT) {
    tft.drawFastHLine(left,y,right-left+1,RED); return;
  }
  for(int x=left;x<=right;x++) {
    uint8_t alpha=pgm_read_byte(&REMINDER_ALPHA[(y-REMINDER_MASK_TOP)*320+x]);
    reminderSpan[x-left]=reminderTextPixel(alpha,RED);
  }
  bool previousSwap=tft.getSwapBytes(); tft.setSwapBytes(true);
  tft.pushImage(left,y,right-left+1,1,reminderSpan);
  tft.setSwapBytes(previousSwap);
}
void renderReminder() {
  if(!reminderVisualStarted) {
    int radius=ReminderGeometry::initialRadius;
    for(int top=0;top<172;top+=40) {
      canvas.fillSprite(BLACK);
      for(int y=top;y<min(top+40,172);y++) {
        int extent=ReminderGeometry::extent(radius,y);
        if(extent>=0) {
          int left=max(0,ReminderGeometry::cx-extent),right=min(319,ReminderGeometry::cx+extent);
          canvas.drawFastHLine(left,y-top,right-left+1,RED);
        }
        if(y>=REMINDER_MASK_TOP && y<REMINDER_MASK_TOP+REMINDER_MASK_HEIGHT) {
          for(int x=0;x<320;x++) {
            uint8_t alpha=pgm_read_byte(&REMINDER_ALPHA[(y-REMINDER_MASK_TOP)*320+x]);
            if(!alpha) continue;
            int dx=x-160,dy=y-86;
            uint16_t background=dx*dx+dy*dy<=radius*radius?RED:BLACK;
            canvas.drawPixel(x,y-top,reminderTextPixel(alpha,background));
          }
        }
      }
      canvas.pushSprite(0,top,0,0,320,min(40,172-top));
    }
    reminderRadius=radius; reminderVisualStarted=true;
    reminderVisualAt=millis(); reminderLastFrame=reminderVisualAt;
    return;
  }
  uint32_t now=millis();
  if(!reminderFrameDue(now)) return;
  reminderLastFrame=now;
  int radius=ReminderGeometry::radiusAt(uint32_t(now-reminderVisualAt));
  if(radius<=reminderRadius) return;
  tft.startWrite();
  for(int y=0;y<172;y++) {
    int extent=ReminderGeometry::extent(radius,y);
    if(extent<0) continue;
    int left=max(0,160-extent),right=min(319,160+extent);
    int previous=ReminderGeometry::extent(reminderRadius,y);
    if(previous<0) paintReminderSpan(y,left,right);
    else {
      int oldLeft=max(0,160-previous),oldRight=min(319,160+previous);
      paintReminderSpan(y,left,min(right,oldLeft-1));
      paintReminderSpan(y,max(left,oldRight+1),right);
    }
  }
  tft.endWrite(); reminderRadius=radius;
}

void text(const String &value,int x,int baseline,const uint8_t *font,uint16_t color=WHITE) {
  canvas.loadFont(font);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(color,textBackground);
  canvas.drawString(value,x,baseline-canvas.fontHeight()+3-tileY);
  canvas.unloadFont();
}
int textWidth(const String &value,const uint8_t *font) {
  canvas.loadFont(font);
  int width=canvas.textWidth(value);
  canvas.unloadFont();
  return width;
}
void rightText(const String &value,int right,int baseline,const uint8_t *font,uint16_t color=WHITE) {
  text(value,right-textWidth(value,font),baseline,font,color);
}
void cancelWaterUpdate() {
  waterUpdate.stop(); waterUpdatePending=false;
}
// Paint only the newly uncovered rows, using the same layout as a normal blue page.
void composeBlueTile(int top,int rows,int consumed,int goal,int percent) {
  tileY=top;canvas.fillSprite(BLACK);textBackground=BLACK;
  if(top<136) {
    String amount=String(consumed);
    const uint8_t *font=Jersey10Clock136;
    int width=textWidth(amount,font),unitWidth=textWidth("ml",TrackerSansBold20);
    if((320+width)/2+8+unitWidth>310) {font=Jersey10Digits96;width=textWidth(amount,font);}
    int numberX=(320-width)/2;
    text(amount,numberX,135,font);
    // These supplied digit fonts have descents 15/11; Bold 20 has descent 4.
    int unitBaseline=135-(font==Jersey10Clock136?15:11)+4;
    text("ml",numberX+width+8,unitBaseline,TrackerSansBold20);
  }
  if(top+rows>136) {
    text(String(goal)+"ml",10,168,TrackerSansBold20);
    rightText(String(percent)+"%",310,168,TrackerSansBold20);
  }
}
void drawBlueBand(int top,int height,int consumed,int goal,int percent) {
  for(int y=top;y<top+height;y+=40) {
    int rows=min(40,top+height-y);
    composeBlueTile(y,rows,consumed,goal,percent);
    canvas.pushSprite(0,y,0,0,320,rows);
  }
}
uint16_t waterBlend(uint16_t background,uint8_t alpha) {
  if(alpha==0) return background;
  if(alpha==255) return WATER_BLUE;
  unsigned inverse=255-alpha;
  unsigned r=(((WATER_BLUE>>11)&31)*alpha+((background>>11)&31)*inverse+127)/255;
  unsigned g=(((WATER_BLUE>>5)&63)*alpha+((background>>5)&63)*inverse+127)/255;
  unsigned b=((WATER_BLUE&31)*alpha+(background&31)*inverse+127)/255;
  return (r<<11)|(g<<5)|b;
}
bool renderWaterUpdate(uint32_t now) {
  if(!waterUpdate.frameDue(now)) return false;
  uint32_t elapsed=uint32_t(now-waterUpdate.began);
  bool afterCover=elapsed>=WaterUpdateAnimation::riseMs;
  bool switchingScene=afterCover && !waterUpdate.covered;
  int dirtyTop=172,dirtyBottom=40;
  for(int block=0;block<WaterUpdateAnimation::blocks;block++) {
    int top=172,bottom=40;
    for(int x=block*8;x<block*8+8;x++) {
      float oldY=WaterUpdateAnimation::surfaceAt(x,waterUpdate.lastElapsed);
      float newY=WaterUpdateAnimation::surfaceAt(x,elapsed);
      waterSurface[x]=newY;
      if(oldY==newY && !switchingScene) continue;
      top=min(top,switchingScene?40:int(fminf(oldY,newY)));
      bottom=max(bottom,min(172,int(fmaxf(oldY,newY))+1));
    }
    waterDirty[block]={top,bottom};
    if(bottom>top){dirtyTop=min(dirtyTop,top);dirtyBottom=max(dirtyBottom,bottom);}
  }
  // Recompose the narrow moving surface once; transfer only changed 8-pixel blocks.
  bool swap=tft.getSwapBytes();tft.setSwapBytes(false);tft.startWrite();
  for(int y=dirtyTop;y<dirtyBottom;y+=40) {
    int rows=min(40,dirtyBottom-y);
    composeBlueTile(y,rows,afterCover?waterTargetMl:waterSourceMl,
                    afterCover?waterTargetGoal:waterSourceGoal,
                    afterCover?waterTargetPercent:waterSourcePercent);
    for(int x=0;x<320;x++) {
      int edge=int(waterSurface[x]);
      int first=max(y,edge+1);
      if(first<y+rows) canvas.drawFastVLine(x,first-y,y+rows-first,WATER_BLUE);
      if(edge>=y && edge<y+rows) {
        uint8_t alpha=uint8_t((1.0f-(waterSurface[x]-edge))*255.0f+0.5f);
        canvas.drawPixel(x,edge-y,waterBlend(canvas.readPixel(x,edge-y),alpha));
      }
    }
    const uint16_t *pixels=static_cast<const uint16_t*>(canvas.getPointer());
    for(int block=0;block<WaterUpdateAnimation::blocks;block++) {
      int first=max(y,waterDirty[block].top),last=min(y+rows,waterDirty[block].bottom);
      if(last<=first) continue;
      int index=0;
      for(int row=first;row<last;row++)for(int x=block*8;x<block*8+8;x++)
        reminderSpan[index++]=pixels[(row-y)*320+x];
      // Sprite pixels are already byte-swapped; reuse the reminder's 640-byte scratch.
      tft.pushImage(block*8,first,8,last-first,reminderSpan);
    }
  }
  tft.endWrite();tft.setSwapBytes(swap);
  waterUpdate.top=WaterUpdateAnimation::topAt(elapsed);waterUpdate.lastFrame=now;
  waterUpdate.lastElapsed=elapsed;waterUpdate.covered=afterCover;
  if(elapsed>=WaterUpdateAnimation::durationMs) {waterUpdate.active=false;return true;}
  return false;
}

void cancelProgressUpdate() {
  progressUpdate.stop();progressUpdatePending=false;
}
void drawRedProgressBar(int width,uint16_t colour,bool fullFooter=false) {
  tileY=136;canvas.fillSprite(BLACK);
  canvas.fillRoundRect(22,3,276,15,7,GREY);
  if(width>0) canvas.fillRoundRect(27,7,width,7,min(3,width/2),colour);
  if(fullFooter) canvas.pushSprite(0,136,0,0,320,36);
  else canvas.pushSprite(22,139,22,3,276,15); // Only the track changes during animation.
}
bool renderProgressUpdate(uint32_t now) {
  if(!progressUpdate.frameDue(now))return false;
  uint32_t elapsed=uint32_t(now-progressUpdate.began);
  int width=progressUpdate.widthAt(elapsed);
  uint16_t colour=waterBlend(WHITE,ProgressBarAnimation::blueAt(elapsed));
  if(width!=progressUpdate.drawnWidth||colour!=progressUpdate.drawnColour) {
    drawRedProgressBar(width,colour);
    progressUpdate.drawnWidth=width;progressUpdate.drawnColour=colour;
  }
  progressUpdate.lastFrame=now;
  if(elapsed>=ProgressBarAnimation::durationMs){progressUpdate.active=false;return true;}
  return false;
}

bool calibrationActive() {
  return mode==CAL_EMPTY || mode==CAL_MASS || mode==CAL_LOAD ||
         (mode==SAMPLING && (sampleJob==CAL_ZERO || sampleJob==CAL_KNOWN));
}
void drawCalibration() {
  int active=(mode==CAL_MASS)?1:((mode==CAL_LOAD ||
             (mode==SAMPLING && sampleJob==CAL_KNOWN))?2:0);
  canvas.fillRoundRect(8,40-tileY,304,22,11,GREY);
  textBackground=GREY;
  text("CALIBRATION",17,57,TrackerSansBold15);
  const char *steps[]={"1. REMOVE EVERYTHING","2. SET WEIGHT","3. PLACE WEIGHT"};
  for(int i=0;i<3;i++) {
    int y=77+i*25;
    uint16_t background=i==active?ROW_SELECTED:GREY;
    canvas.fillRoundRect(8,y-tileY,304,22,11,background);
    textBackground=background;
    text(steps[i],17,y+17,TrackerSansBold15);
    if(i==1) rightText(String(knownMass)+"g",301,y+17,TrackerSansRegular12);
  }
  if(mode==SAMPLING) {
    textBackground=BLACK;
    String status="MEASURING...";
    text(status,(320-textWidth(status,TrackerSansRegular12))/2,166,TrackerSansRegular12);
  }
}
void drawCalibrationDone() {
  textBackground=BLACK;
  String label="CALIBRATION DONE";
  text(label,(320-textWidth(label,TrackerSansBold15))/2,93,TrackerSansBold15);
}
bool zeroActive() {
  return mode==ZERO_CONFIRM || mode==ZERO_TARGET || mode==CONTAINER_SELECT ||
         mode==CONTAINER_PLACE || mode==CONTAINER_ZERO_TARGET ||
         (mode==SAMPLING && (sampleJob==ZERO_PLATFORM || sampleJob==EMPTY_CONTAINER));
}
void drawZeroWizard() {
  bool container=mode==CONTAINER_SELECT || mode==CONTAINER_PLACE ||
                 mode==CONTAINER_ZERO_TARGET || (mode==SAMPLING && sampleJob==EMPTY_CONTAINER);
  int active=container ? (mode==CONTAINER_SELECT?0:(mode==CONTAINER_PLACE?1:2)) :
                         (mode==ZERO_CONFIRM?0:1);
  canvas.fillRoundRect(8,40-tileY,304,22,11,GREY);
  textBackground=GREY;
  text(container?"ZERO GLASS/BOTTLE":"ZERO",17,57,TrackerSansBold15);
  const char *platformSteps[]={"1. REMOVE EVERYTHING","2. ZERO TARGET"};
  const char *containerSteps[]={"1. SELECT ZERO TARGET","2. PLACE EMPTY GLASS/BOTTLE","3. ZERO TARGET"};
  for(int i=0;i<(container?3:2);i++) {
    int y=77+i*25;
    uint16_t background=i==active?ROW_SELECTED:GREY;
    canvas.fillRoundRect(8,y-tileY,304,22,11,background);
    textBackground=background;
    text(container?containerSteps[i]:platformSteps[i],17,y+17,TrackerSansBold15);
    if(container && i==0) rightText(PROFILE_NAMES[zeroProfile],301,y+17,TrackerSansRegular12);
  }
  if(mode==SAMPLING) {
    textBackground=BLACK;
    String status="MEASURING...";
    text(status,(320-textWidth(status,TrackerSansRegular12))/2,166,TrackerSansRegular12);
  }
}
void drawZeroDone() {
  textBackground=BLACK;
  String label="ZERO COMPLETE";
  text(label,(320-textWidth(label,TrackerSansBold15))/2,93,TrackerSansBold15);
}
void panel(const String &title,const String &line1,const String &line2,const String &line3) {
  canvas.fillRoundRect(8,43-tileY,304,121,10,GREY);
  textBackground=GREY;
  text(title,16,65,TrackerSansBold15,YELLOW);
  text(line1,16,90,TrackerSansRegular12);
  text(line2,16,114,TrackerSansRegular12);
  text(line3,16,148,TrackerSansRegular12,CYAN);
}
String rowValue(int index) {
  if(index==0) return String(mode==EDIT&&row==0?draft:cfg.goal)+"ml";
  if(index==1) {
    int value=mode==EDIT&&row==1?draft:cfg.reminder;
    return value==-1?"10s":(value?String(value)+"min":"OFF");
  }
  if(index==2) return PROFILE_NAMES[mode==EDIT&&row==2?draft:cfg.profile];
  if(index==ROW_LED_COLOUR) return LED_COLOURS[mode==EDIT&&row==ROW_LED_COLOUR?draft:ledColour].name;
  return "";
}
void drawSettingsRow(int index,int slot,const String &value,bool focused,bool editing) {
  const char *labels[]={"WATER GOAL","REMINDER TIME","GLASS/BOTTLE","ZERO GLASS/BOTTLE","ZERO","CALIBRATE","LED COLOUR"};
  tileY=0;
  canvas.fillSprite(BLACK);
  uint16_t background=focused?ROW_SELECTED:GREY;
  canvas.fillRoundRect(8,0,304,20,10,background);
  textBackground=background;
  text(labels[index],17,16,TrackerSansBold15);
  rightText(value,301,16,TrackerSansRegular12,editing?YELLOW:WHITE);
  // Transfer the completed row, including its black gap, in one sprite push.
  canvas.pushSprite(0,40+slot*SETTINGS_PITCH,0,0,320,SETTINGS_PITCH);
}
void draw() {
  static bool initialized=false, rowsVisible=false,reminderWasVisible=false;
  static String previousBody, previousRows[SETTINGS_ROWS];
  static int previousPage=-1,previousRedWidth=-1,previousSettingsFirst=0;
  static bool redVisible=false,blueVisible=false;
  static int previousIntake=-1,previousGoal=-1,previousBluePercent=-1;
  static String previousLargeClock;
  if(reminderDue) {
    cancelWaterUpdate();cancelProgressUpdate();
    reminderWasVisible=true; renderReminder(); dirty=false; return;
  }
  if(reminderWasVisible) {
    // Repaint each normal region once, without flashing a black full-screen clear.
    resetReminderVisual(); reminderWasVisible=false;
    rowsVisible=false; redVisible=false; blueVisible=false;
    previousPage=-1; previousBody="";
  }
  if(!initialized) {
    tft.fillScreen(BLACK); // Clear the physical panel only once at startup.
    initialized=true;
  }
  time_t utc=time(nullptr);
  String clockText="--:--";
  if(utc>1700000000 && zoneKnown) {
    time_t local=utc+utcOffset; struct tm parts; gmtime_r(&local,&parts);
    char value[6]; strftime(value,sizeof(value),"%H:%M",&parts); clockText=value;
  }
  bool showRed=page==1 && mode==PAGES && !reminderDue;
  bool showBlue=page==0 && mode==PAGES && !reminderDue;
  int percent=progressPercent();
  if(!showBlue) cancelWaterUpdate();
  if(!showRed) cancelProgressUpdate();
  if(previousPage!=page) {
    tileY=0; textBackground=BLACK; canvas.fillSprite(BLACK);
    uint16_t colors[]={BLUE,RED,YELLOW};
    for(int i=0;i<3;i++) {
      canvas.fillCircle(17+i*23,17,9,colors[i]);
      if(i==page) canvas.fillCircle(17+i*23,17,4,BLACK);
    }
    canvas.pushSprite(0,0);
    previousPage=page;
  }
  bool showRows=!reminderDue && page==2 && (mode==PAGES||mode==MENU||mode==EDIT);
  if(showBlue) {
    int consumed=int(floorf(intake.ml+0.5f));
    if(waterUpdate.active && consumed<waterTargetMl) {
      cancelWaterUpdate();blueVisible=false; // Local-day reset is not a drinking animation.
    }
    if(waterUpdatePending && !waterUpdate.active) {
      waterUpdatePending=false;
      if(blueVisible && consumed>previousIntake) {
        waterSourceMl=previousIntake;waterSourceGoal=previousGoal;waterSourcePercent=previousBluePercent;
        waterTargetMl=consumed;waterTargetGoal=cfg.goal;waterTargetPercent=percent;
        waterUpdate.start(millis());
      }
    }
    if(waterUpdate.active) {
      if(renderWaterUpdate(millis())) {
        previousIntake=waterTargetMl;previousGoal=waterTargetGoal;previousBluePercent=waterTargetPercent;
      }
      blueVisible=true;redVisible=false;rowsVisible=false;previousBody="";
      dirty=false;return;
    }
    if(!blueVisible || previousIntake!=consumed) {
      drawBlueBand(40,96,consumed,cfg.goal,percent);
      previousIntake=consumed;
    }
    if(!blueVisible || previousGoal!=cfg.goal || previousBluePercent!=percent) {
      drawBlueBand(136,36,consumed,cfg.goal,percent);
      previousGoal=cfg.goal;previousBluePercent=percent;
    }
    blueVisible=true; redVisible=false; rowsVisible=false; previousBody="";
  } else if(showRed) {
    blueVisible=false;
    if(!redVisible || previousLargeClock!=clockText) {
      int clockX=(320-textWidth(clockText,Jersey10Clock136))/2;
      for(tileY=40;tileY<136;tileY+=40) {
        canvas.fillSprite(BLACK); textBackground=BLACK;
        text(clockText,clockX,131,Jersey10Clock136);
        canvas.pushSprite(0,tileY,0,0,320,min(40,136-tileY));
      }
      previousLargeClock=clockText;
    }
    int targetWidth=constrain(int(266.0f*intake.ml/cfg.goal+0.5f),0,266);
    if(progressUpdate.active && targetWidth<progressUpdate.toWidth) {
      cancelProgressUpdate();previousRedWidth=-1;
    }
    if(progressUpdatePending && !progressUpdate.active) {
      progressUpdatePending=false;
      if(redVisible && targetWidth>=previousRedWidth)
        progressUpdate.start(millis(),previousRedWidth,targetWidth);
    }
    if(progressUpdate.active) {
      if(renderProgressUpdate(millis())) previousRedWidth=progressUpdate.toWidth;
    } else if(!redVisible || previousRedWidth!=targetWidth) {
      drawRedProgressBar(targetWidth,WHITE,!redVisible);
      previousRedWidth=targetWidth;
    }
    redVisible=true; rowsVisible=false; previousBody="";
  } else if(showRows) {
    blueVisible=false; redVisible=false;
    int first=mode==PAGES?0:previousSettingsFirst;
    if(mode!=PAGES) {
      if(row<first) first=row;
      if(row>=first+SETTINGS_VISIBLE) first=row-SETTINGS_VISIBLE+1;
    }
    bool windowChanged=first!=previousSettingsFirst;
    for(int slot=0;slot<SETTINGS_VISIBLE;slot++) {
      int i=first+slot;
      String value=rowValue(i);
      bool focused=mode!=PAGES && row==i;
      bool editing=focused && mode==EDIT;
      String key=value+":"+String(focused)+":"+String(editing);
      if(!rowsVisible || windowChanged || previousRows[i]!=key) {
        drawSettingsRow(i,slot,value,focused,editing);
        previousRows[i]=key;
      }
    }
    previousSettingsFirst=first;
    rowsVisible=true; previousBody="";
  } else {
    blueVisible=false; redVisible=false;
    String key=String(int(mode))+":"+String(page)+":"+String(reminderDue)+":"+
               String(knownMass)+":"+String(cfg.profile)+":"+String(zeroProfile)+":"+message;
    if(rowsVisible || previousBody!=key) {
      // Render modal content in complete stripes using the same sprite. The
      // sprite clips shapes/text at each stripe edge; the panel is never blanked.
      for(tileY=40;tileY<172;tileY+=40) {
        canvas.fillSprite(BLACK);
        if(mode==MESSAGE) panel("WATER TRACKER",message,"","Press to continue");
        else if(mode==CAL_DONE) drawCalibrationDone();
        else if(mode==ZERO_DONE) drawZeroDone();
        else if(zeroActive()) drawZeroWizard();
        else if(calibrationActive()) drawCalibration();
        else if(mode==SAMPLING) panel("MEASURING...","Keep the scale still.","","Hold to cancel");
        else panel("DATA SCREEN","Coming next.","Turn to switch screens.","Settings: yellow circle");
        canvas.pushSprite(0,tileY,0,0,320,min(40,172-tileY));
      }
      previousBody=key;
    }
    rowsVisible=false;
  }
  dirty=false;
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\nWater tracker boot; reset reason=%d; free heap=%u\n",
                int(esp_reset_reason()),ESP.getFreeHeap());
  pinMode(ENC_A,INPUT_PULLUP); pinMode(ENC_B,INPUT_PULLUP);
  pinMode(ENC_SW,INPUT_PULLUP); pinMode(BACK_PIN,INPUT_PULLUP);
  encoderPrevious=(digitalRead(ENC_A)<<1)|digitalRead(ENC_B);
  attachInterrupt(digitalPinToInterrupt(ENC_A),encoderISR,CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B),encoderISR,CHANGE);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,LOW);
  delay(150);
  tft.init(); tft.setRotation(3); // Landscape, flipped 180 degrees for this mounting.
  // Set colour order at runtime as well: build_opt.h changes can leave a cached
  // TFT_eSPI object using its previous MADCTL colour bit. Preserve rotation 3's
  // MV/MY bits, and explicitly set the BGR bit (0xA8 total).
  tft.writecommand(TFT_MADCTL);
  tft.writedata(TFT_MAD_MV | TFT_MAD_MY | TFT_MAD_BGR);
  canvas.setColorDepth(16);
  if(!canvas.createSprite(320,40)) {
    Serial.println("ERROR: row sprite allocation failed");
    tft.fillScreen(BLACK); digitalWrite(TFT_BL,HIGH);
    tft.setTextColor(WHITE,BLACK); tft.setCursor(10,30);
    tft.print("Display buffer allocation failed");
    while(true) delay(1000);
  }
  canvas.setTextWrap(false);
  digitalWrite(TFT_BL,HIGH);
  Serial.println("Display initialized: TFT_eSPI sprites, 320x172, 4 MHz, runtime BGR (MADCTL=0xA8)");
  strip.begin(); strip.setBrightness(35); strip.clear(); strip.show();
  scale.begin(HX_DT,HX_SCK,true,false); // ESP32 timing; no blocking startup reset.
  if(prefs.begin("water-settings",false)) {
    Settings loaded;
    if(prefs.getBytesLength("settings")==sizeof(loaded) &&
       prefs.getBytes("settings",&loaded,sizeof(loaded))==sizeof(loaded) && validSettings(loaded)) cfg=loaded;
    ledColour=prefs.getUChar("ledColour",0);
    if(ledColour>=LED_COLOUR_COUNT) ledColour=0;
    DailyIntake loadedIntake;
    if(prefs.getBytesLength("intake")==sizeof(loadedIntake) &&
       prefs.getBytes("intake",&loadedIntake,sizeof(loadedIntake))==sizeof(loadedIntake) &&
       loadedIntake.magic==intake.magic && isfinite(loadedIntake.ml) &&
       loadedIntake.ml>=0 && loadedIntake.ml<=100000 && loadedIntake.day>=0)
      intake=loadedIntake;
    zoneKnown=prefs.getBool("zoneKnown",false);
    utcOffset=prefs.getInt("utcOffset",0);
    if(utcOffset < -43200 || utcOffset > 50400) zoneKnown=false;
  } else Serial.println("ERROR: persistent storage unavailable");
  reminderStart=millis(); draw();
  Serial.printf("Settings drawn; free heap=%u\n",ESP.getFreeHeap());
  // NTP supplies UTC; the geolocation service supplies the current local offset.
  configTime(0,0,"pool.ntp.org","time.cloudflare.com");
  WiFi.setAutoReconnect(true);
  wifi.setDebugOutput(false);
  wifi.setConfigPortalBlocking(false);
  wifi.setConnectTimeout(8);
  wifi.setConfigPortalTimeout(0);
  Serial.printf("Starting Wi-Fi; free heap=%u\n",ESP.getFreeHeap());
  wifi.autoConnect("WaterTracker-Setup","drinkwater");
  Serial.printf("Wi-Fi setup returned; free heap=%u\n",ESP.getFreeHeap());
  zoneQueue=xQueueCreate(1,sizeof(ZoneResult));
  if(zoneQueue) {
    if(xTaskCreate(networkWorker,"timezone",12288,nullptr,1,nullptr)!=pdPASS)
      Serial.println("ERROR: timezone worker could not start");
  }
  Serial.println("Wi-Fi setup: join WaterTracker-Setup (password drinkwater), open 192.168.4.1");
}
void loop() {
  wifi.process();
  int steps;
  portENTER_CRITICAL(&encoderMux); steps=encoderSteps; encoderSteps=0; portEXIT_CRITICAL(&encoderMux);
  rotate(-steps); // Reverse physical encoder direction for every navigation/edit mode.
  int event=enter.poll(); if(event) click(event==2);
  bool backWasPressed=!back.stable;
  int backEvent=back.poll();
  static bool reminderBackConsumed=false;
  if(reminderDue && !backWasPressed && !back.stable) {
    acknowledgeReminder(); reminderBackConsumed=true;
  }
  // Consume the matching release/hold too, so dismissing never also navigates back.
  if(reminderBackConsumed) {
    if(back.stable) reminderBackConsumed=false;
  } else if(backEvent && !reminderDue) cancel();
  serviceSamples();
  ZoneResult zone;
  if(zoneQueue && xQueueReceive(zoneQueue,&zone,0)==pdTRUE) {
    if(!zoneKnown||utcOffset!=zone.offset) {
      utcOffset=zone.offset; zoneKnown=true;
      prefs.putInt("utcOffset",utcOffset); prefs.putBool("zoneKnown",true);
    }
    Serial.printf("Timezone: %s, UTC offset %ld seconds\n",zone.name,(long)zone.offset);
    dirty=true;
  }
  serviceIntake();
  uint32_t now=millis();
  if(mode==MESSAGE && static_cast<int32_t>(now-messageUntil)>=0) { mode=returnMode; dirty=true; }
  if(mode==CAL_DONE && static_cast<int32_t>(now-messageUntil)>=0) {
    page=2; row=ROW_CALIBRATE; mode=MENU; dirty=true;
  }
  if(mode==ZERO_DONE && static_cast<int32_t>(now-messageUntil)>=0) {
    page=2; row=zeroReturnRow; mode=MENU; dirty=true;
  }
  // Reminders repeat from boot, acknowledgement, or the latest detected drink.
  if(cfg.reminder && !reminderDue && now-reminderStart>=ReminderTiming::intervalMs(cfg.reminder) &&
     (mode==PAGES||mode==MENU)) { reminderDue=true; dirty=true; }
  if(now-lastClock>=1000) {
    lastClock=now;
    static int64_t lastMinute=-1;
    time_t utc=time(nullptr);
    int64_t minute=(utc>1700000000 && zoneKnown) ? (int64_t(utc)+utcOffset)/60 : -1;
    if(minute!=lastMinute) { lastMinute=minute; dirty=true; }
  }
  if(reminderDue && reminderFrameDue(now)) dirty=true;
  if((waterUpdatePending && !waterUpdate.active) || waterUpdate.frameDue(now)) dirty=true;
  if((progressUpdatePending && !progressUpdate.active) || progressUpdate.frameDue(now)) dirty=true;
  if(dirty) draw();
  serviceLeds(millis()); // Start fades after the initial reminder frame is on screen.
  delay(1);
}
