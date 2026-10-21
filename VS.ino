#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include "soc/rtc_io_reg.h"
#include "soc/sens_reg.h"
#include "kylie_jenner_data.h"

#define VOLUME        80
#define ROTATION      1
#define LOOP_FOREVER  true

#define TFT_DC    2
#define TFT_CS   15
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_BL   21
#define SPEAKER_PIN 26
#define HEART_PIN    1
#define TOUCH_IRQ   36
#define LED_R        4
#define LED_G       16
#define LED_B       17

#define RB_SIZE 4096
#define RB_MASK (RB_SIZE - 1)

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, ROTATION, false);

JPEGDEC jpeg;
hw_timer_t *audioTimer = nullptr;

uint8_t rb[RB_SIZE];
volatile uint32_t rbHead = 0, rbTail = 0, samplesPlayed = 0;
uint32_t audioPos = 0;

void IRAM_ATTR onAudioTimer() {
  if (rbTail != rbHead) {
    uint8_t s = rb[rbTail & RB_MASK];
    rbTail++;
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, s, RTC_IO_PDAC2_DAC_S);
    samplesPlayed++;
  }
}

void fillAudio() {
  uint32_t h = rbHead;
  uint32_t space = RB_SIZE - (h - rbTail);
  uint32_t left = CLIP_AUDIO_LEN - audioPos;
  uint32_t n = space < left ? space : left;
  for (uint32_t i = 0; i < n; i++) {
    int v = 128 + ((int)clip_audio[audioPos + i] - 128) * VOLUME / 100;
    rb[(h + i) & RB_MASK] = (uint8_t)v;
  }
  audioPos += n;
  __sync_synchronize();
  rbHead = h + n;
}

bool audioFinished() { return audioPos >= CLIP_AUDIO_LEN && rbHead == rbTail; }

void setupAudioTimer() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  audioTimer = timerBegin(4000000);
  timerAttachInterrupt(audioTimer, &onAudioTimer);
  timerAlarm(audioTimer, 4000000 / CLIP_SAMPLE_RATE, true, 0);
#else
  audioTimer = timerBegin(0, 20, true);
  timerAttachInterrupt(audioTimer, &onAudioTimer, true);
  timerAlarmWrite(audioTimer, 4000000 / CLIP_SAMPLE_RATE, true);
  timerAlarmEnable(audioTimer);
#endif
  timerStop(audioTimer);
}

float glow = 0;

void setHeart(float b) {
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  analogWrite(HEART_PIN, (int)b);
}

void heartbeatIdle() {
  float t = (millis() % 1200) / 1200.0f;
  float b = 0;
  if (t < 0.12f)                   b = sinf(t / 0.12f * 3.1416f);
  else if (t > 0.20f && t < 0.32f) b = 0.7f * sinf((t - 0.20f) / 0.12f * 3.1416f);
  setHeart(8 + 200 * b);
}

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(LED_R, 255 - r);
  analogWrite(LED_G, 255 - g);
  analogWrite(LED_B, 255 - b);
}

void updateRGB() {
  uint32_t i = (uint64_t)samplesPlayed * CLIP_LEVEL_HZ / CLIP_SAMPLE_RATE;
  float level = i < CLIP_LEVEL_COUNT ? clip_levels[i] : 0;
  glow = level > glow ? level : glow * 0.90f;
  float hue = (millis() % 4000) / 4000.0f;
  float pinkMix = 0.5f + 0.5f * sinf(hue * 6.2832f);
  setRGB((uint8_t)(glow), (uint8_t)(glow * 0.05f), (uint8_t)(glow * (0.25f + 0.6f * pinkMix)));
  setHeart(15 + glow * 0.94f);
}

int drawMCU(JPEGDRAW *p) {
  gfx->draw16bitRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
  return 1;
}

void drawFrame(uint32_t i) {
  const uint8_t *data = clip_video + clip_frame_offsets[i];
  int len = clip_frame_offsets[i + 1] - clip_frame_offsets[i];
  if (jpeg.openRAM((uint8_t *)data, len, drawMCU)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    int x = (gfx->width() - jpeg.getWidth()) / 2;
    int y = (gfx->height() - jpeg.getHeight()) / 2;
    jpeg.decode(x, y, 0);
    jpeg.close();
  }
}

uint32_t frameIndex = 0;

void startPlayback() {
  timerStop(audioTimer);
  rbHead = rbTail = 0;
  samplesPlayed = 0;
  audioPos = 0;
  frameIndex = 0;
  glow = 0;
  fillAudio();
  timerStart(audioTimer);
}

bool screenTapped() {
  static uint32_t lastTap = 0;
  if (digitalRead(TOUCH_IRQ) == LOW && millis() - lastTap > 800) {
    lastTap = millis();
    return true;
  }
  return false;
}

uint32_t audioFrameNow() {
  return (uint64_t)samplesPlayed * CLIP_FPS / CLIP_SAMPLE_RATE;
}

void setup() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(HEART_PIN, OUTPUT);
  setHeart(0);
  pinMode(TOUCH_IRQ, INPUT);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setRGB(0, 0, 0);

  gfx->begin(40000000);
  gfx->fillScreen(0x0000);

  dacWrite(SPEAKER_PIN, 128);
  setupAudioTimer();
  for (int i = 0; i < 2; i++) {
    setHeart(255); delay(150); setHeart(0); delay(150);
  }
  startPlayback();
}

void loop() {
  fillAudio();
  updateRGB();

  if (screenTapped()) {
    startPlayback();
    return;
  }

  if (frameIndex < CLIP_FRAME_COUNT) {
    if (frameIndex + 1 >= audioFrameNow()) {
      while (frameIndex > audioFrameNow() && !audioFinished()) {
        fillAudio();
        updateRGB();
        delay(1);
      }
      drawFrame(frameIndex);
    }
    frameIndex++;
  }

  if (audioFinished()) {
    dacWrite(SPEAKER_PIN, 128);
    setRGB(0, 0, 0);
    if (LOOP_FOREVER) {
      uint32_t t0 = millis();
      while (millis() - t0 < 2400) { heartbeatIdle(); delay(5); }
    } else {
      while (!screenTapped()) { heartbeatIdle(); delay(5); }
    }
    startPlayback();
  }
}
