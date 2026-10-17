#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <driver/i2s_std.h>
#include <FluxGarage_RoboEyes.h>
#include <ESP32Servo.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <vector>

// ---------- Sure olcumu ----------
static uint32_t tStart = 0;
#define MARK(label) \
  Serial.printf("[SURE] %s: %lu ms\n", label, (unsigned long)(millis() - tStart))

// ---------- OLED ----------
static constexpr int OLED_WIDTH = 128;
static constexpr int OLED_HEIGHT = 64;
static constexpr int OLED_SDA = 21;
static constexpr int OLED_SCL = 22;
static constexpr int OLED_RESET = -1;

Adafruit_SH1106G display(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    OLED_RESET
);

RoboEyes<Adafruit_SH1106G> roboEyes(display);

// ---------- Davranis ortak durumu ----------
enum AppPhase : uint8_t {
  PHASE_IDLE,       // konusma yok
  PHASE_LISTENING,  // dinliyor (takip penceresi dahil)
  PHASE_THINKING,   // STT + LLM + TTS hazirligi
  PHASE_SPEAKING,   // ses caliyor
  PHASE_FAREWELL    // konusma sessizlikle bitti
};

enum SoundId : uint8_t { SND_NONE, SND_LISTEN, SND_HAPPY, SND_ANGRY, SND_YAWN };

static volatile AppPhase appPhase = PHASE_IDLE;
static volatile uint8_t ttsLevel = 0;               // 0..255, calan sesin siddeti
static volatile SoundId pendingSound = SND_NONE;    // loop() calar
static volatile uint32_t lastInteractionMs = 0;

bool setupOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  if (!display.begin(0x3C, true)) {
    Serial.println("HATA: SH1106 OLED baslatilamadi.");
    return false;
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  roboEyes.begin(OLED_WIDTH, OLED_HEIGHT, 60);
  roboEyes.setPosition(DEFAULT);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 5, 10);
  roboEyes.open();

  Serial.println("OLED basariyla baslatildi.");
  return true;
}

// ---------- Servo ----------
Servo headServo;

constexpr int SERVO_PIN = 19;
constexpr int SERVO_CENTER = 90;

static constexpr float HEAD_TURN_DEG = 25.0f;        // sag/sol bakista bas donusu
static constexpr int   HEAD_DIRECTION = 1;           // yon ters ise -1 yapin
static constexpr bool  SERVO_DETACH_WHEN_IDLE = false;

static float headAngle = SERVO_CENTER;               // anlik aci
static float headFrom = SERVO_CENTER, headTo = SERVO_CENTER;
static uint32_t headMoveStart = 0, headMoveDur = 1;
static bool headMoving = false;
static uint32_t headLastMotion = 0;

void setupServo() {
    headServo.setPeriodHertz(50);

    int servoChannel = headServo.attach(SERVO_PIN, 1000, 2000);
    Serial.printf("Servo attach sonucu: %d\n", servoChannel);

    if (!headServo.attached()) {
        Serial.println("HATA: Servo PWM kanali baslatilamadi!");
        return;
    }

    headServo.write(SERVO_CENTER);
    headAngle = SERVO_CENTER;
    Serial.println("Servo orta konuma getirildi.");
}

static void headWriteAngle(float deg) {
    deg = constrain(deg, 0.0f, 180.0f);
    headAngle = deg;

    if (!headServo.attached()) {
        headServo.attach(SERVO_PIN, 1000, 2000);
    }

    headServo.writeMicroseconds(1000 + static_cast<int>(deg * (1000.0f / 180.0f)));
}

static float easeInOut(float t) {
    return 0.5f - 0.5f * cosf(PI * t);   // yavas - hizli - yavas
}

// Baslangic her zaman anlik aci: ani sicrama olmaz, hareket ortasinda da yeniden planlanir.
static void headMoveTo(float target, uint32_t durationMs = 0) {
    target = constrain(target, 20.0f, 160.0f);

    if (durationMs == 0) {
        float d = 250.0f + fabsf(target - headAngle) * 18.0f;
        if (d < 350.0f) d = 350.0f;
        if (d > 1300.0f) d = 1300.0f;
        durationMs = static_cast<uint32_t>(d);
    }

    headFrom = headAngle;
    headTo = target;
    headMoveStart = millis();
    headMoveDur = durationMs;
    headMoving = true;
}

static void headUpdate() {
    uint32_t now = millis();

    if (headMoving) {
        float t = static_cast<float>(now - headMoveStart) / static_cast<float>(headMoveDur);

        if (t >= 1.0f) {
            t = 1.0f;
            headMoving = false;
            headLastMotion = now;
        }

        headWriteAngle(headFrom + (headTo - headFrom) * easeInOut(t));
    } else if (SERVO_DETACH_WHEN_IDLE && headServo.attached() &&
               now - headLastMotion > 1000) {
        headServo.detach();
    }
}

// ---------- Touch ----------
constexpr int TOUCH_PIN = 4;

static constexpr unsigned long TOUCH_DEBOUNCE_MS = 40;
static constexpr unsigned long TOUCH_LONG_PRESS_MS = 800;

// Olaylar sayac olarak tutulur; her tuketici kendi "son gordugum" degerini saklar.
static volatile uint32_t touchPressSeq = 0;   // her basista artar
static volatile uint32_t touchTapSeq = 0;     // kisa dokunus birakilinca artar
static volatile uint32_t touchLongSeq = 0;    // uzun basma tamamlaninca artar

static bool takeEvent(volatile uint32_t &seq, uint32_t &seen) {
    uint32_t v = seq;
    if (v == seen) return false;
    seen = v;
    return true;
}

static void touchTask(void *parameter) {
    (void)parameter;

    int lastRaw = LOW, stable = LOW;
    unsigned long lastChange = 0, pressStart = 0;
    bool longFired = false;

    for (;;) {
        unsigned long now = millis();
        int raw = digitalRead(TOUCH_PIN);

        if (raw != lastRaw) {
            lastRaw = raw;
            lastChange = now;
        }

        if (now - lastChange >= TOUCH_DEBOUNCE_MS && raw != stable) {
            stable = raw;
            lastInteractionMs = now;

            if (stable == HIGH) {
                pressStart = now;
                longFired = false;
                touchPressSeq = touchPressSeq + 1;
            } else if (!longFired) {
                touchTapSeq = touchTapSeq + 1;   // uzun basma degilse kisa dokunus
            }
        }

        if (stable == HIGH && !longFired &&
            now - pressStart >= TOUCH_LONG_PRESS_MS) {
            longFired = true;
            touchLongSeq = touchLongSeq + 1;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void setupTouch() {
    pinMode(TOUCH_PIN, INPUT);
    xTaskCreate(touchTask, "Touch", 3072, nullptr, 1, nullptr);
    Serial.println("Touch sensor hazir.");
}

// =====================================================================
//  Davranis motoru (gozler + bas servosu)
// =====================================================================

// 3x3 bakis konumlari ve her birine karsilik gelen bas donusu (-1..1, sag = +).
static const uint8_t GAZE_POS[9]  = { DEFAULT, N, NE, E, SE, S, SW, W, NW };
static const float   GAZE_HEAD[9] = { 0.0f, 0.0f, 0.6f, 1.0f, 0.6f, 0.0f, -0.6f, -1.0f, -0.6f };

static constexpr uint32_t ROAM_MIN_MS = 3000;
static constexpr uint32_t ROAM_MAX_MS = 10000;
static constexpr uint32_t DROWSY_AFTER_MS = 90000;    // test icin 10000 yapin
static constexpr uint32_t SLEEP_AFTER_MS = 180000;    // test icin 20000 yapin
static constexpr uint32_t ANGRY_TAP_COUNT = 4;
static constexpr uint32_t ANGRY_TAP_WINDOW_MS = 4000;
static constexpr uint32_t LOVE_MS = 1600;
static constexpr uint32_t ANGRY_MS = 3500;
static constexpr uint32_t SHAKE_MS = 1300;            // 5 Hz x 1.3 sn = 6.5 salinim (merkezde biter)
static constexpr uint32_t FAREWELL_MS = 1500;
static constexpr uint32_t WAKE_MS = 1400;
static constexpr int EYE_BASE_HEIGHT = 36;            // kutuphane varsayilani (goz boyunuz farkliysa ayarlayin)

// ---- Dusunme (yukleme) animasyonu ----
static constexpr int EYE_BASE_WIDTH = 36;              // kutuphane varsayilani; goz boyutunu degistirdiyseniz guncelleyin
static constexpr int EYE_GAP = 10;                     // iki goz arasi
static constexpr int EYE_RADIUS = 8;                   // goz kose yuvarlagi
static constexpr int LOADER_DIAMETER = 20;             // daire capi (eskiden 14)
static constexpr int LOADER_CENTER_DISTANCE = 34;      // iki dairenin merkezleri arasi (eskiden 46)
static constexpr float LOADER_AMPLITUDE = 12.0f;       // yukari-asagi hareket (piksel)
static constexpr uint32_t LOADER_PERIOD_MS = 1100;     // bir tam yukari-asagi dongu
static constexpr uint32_t LOADER_MORPH_MS = 320;       // goz <-> daire gecisi

enum BMode : uint8_t {
  B_ROAM, B_DROWSY, B_SLEEP, B_WAKING,
  B_LISTEN, B_THINK, B_SPEAK,
  B_LOVE, B_ANGRY, B_FAREWELL,
  B_UNTHINK                      // yukleme animasyonundan gozlere donus
};

enum : uint8_t { EVT_NONE, EVT_BLINK2, EVT_LOOK_BACK, EVT_YAWN_OPEN, EVT_YAWN_END };

static BMode bMode = B_ROAM;
static uint32_t bModeSince = 0;
static uint32_t nextRoamAt = 0;
static uint32_t shakeStartAt = 0;
static uint32_t ignoreTapUntil = 0;
static uint32_t seenPress = 0, seenTap = 0;
static uint32_t tapTimes[ANGRY_TAP_COUNT] = {0};
static uint8_t tapIndex = 0;
static int gazeIndex = 0;
static AppPhase lastPhase = PHASE_IDLE;
static uint8_t smoothLevel = 0;
static int appliedHeight = EYE_BASE_HEIGHT;
static uint8_t evt = EVT_NONE;
static uint32_t evtAt = 0;
static bool headDelayArmed = false;
static uint32_t headDelayAt = 0;
static float headDelayTarget = SERVO_CENTER;


static void applyEyeHeight(int h) {
  if (h == appliedHeight) return;
  appliedHeight = h;
  roboEyes.setHeight(h, h);
}

// Gozler once bakar, bas ~200 ms sonra doner.
static void eyesLook(int idx, bool followWithHead) {
  gazeIndex = idx;
  roboEyes.setPosition(GAZE_POS[idx]);

  if (followWithHead) {
    headDelayTarget = SERVO_CENTER + HEAD_DIRECTION * HEAD_TURN_DEG * GAZE_HEAD[idx];
    headDelayAt = millis() + 200;
    headDelayArmed = true;
  }
}

static void enterMode(BMode m) {
  uint32_t now = millis();
  BMode prev = bMode;

  bMode = m;
  bModeSince = now;
  headDelayArmed = false;
  evt = EVT_NONE;

  roboEyes.setCuriosity(OFF);            // yan bakista goz buyumesin
  roboEyes.setIdleMode(OFF, 5, 10);      // rastgele bakisi biz yonetiyoruz
  roboEyes.setAutoblinker(ON, 3, 2);
  if (m != B_SLEEP) roboEyes.open();
  if (m != B_SPEAK) applyEyeHeight(EYE_BASE_HEIGHT);

  switch (m) {
    case B_ROAM:
      roboEyes.setMood(DEFAULT);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER);
      nextRoamAt = now + random(ROAM_MIN_MS, ROAM_MAX_MS + 1);
      break;

    case B_DROWSY:
      roboEyes.setMood(TIRED);
      roboEyes.setAutoblinker(ON, 6, 3);
      eyesLook(5, false);                 // asagi bak
      headMoveTo(SERVO_CENTER);
      break;

    case B_SLEEP:
      roboEyes.setMood(DEFAULT);
      roboEyes.setAutoblinker(OFF, 3, 2);
      eyesLook(0, false);
      roboEyes.close();                   // iki ince cizgi      
      headMoveTo(SERVO_CENTER);
      break;

    case B_WAKING:
      roboEyes.setMood(TIRED);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER);
      pendingSound = SND_YAWN;
      break;

    case B_LISTEN:
      roboEyes.setMood(DEFAULT);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER);
      break;

    case B_THINK:
      roboEyes.setMood(DEFAULT);
      eyesLook(0, false);                 // animasyon bitince gozler ortada donsun
      headMoveTo(SERVO_CENTER);
      break;

    case B_UNTHINK:
      break;

    case B_SPEAK:
      roboEyes.setMood(DEFAULT);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER);
      break;

    case B_LOVE:
      roboEyes.setMood(HAPPY);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER);
      pendingSound = SND_HAPPY;
      break;

    case B_ANGRY:
      roboEyes.setMood(ANGRY);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER, 400);
      shakeStartAt = now + 450;
      pendingSound = SND_ANGRY;
      break;

    case B_FAREWELL:
      roboEyes.setMood(HAPPY);
      eyesLook(0, false);
      headMoveTo(SERVO_CENTER);
      break;
  }
}

// Bosta / uyku dokunma mantigi.
static void handleTouchIdle(uint32_t now) {
  bool pressed = (touchPressSeq != seenPress);
  bool tapped = (touchTapSeq != seenTap);
  seenPress = touchPressSeq;
  seenTap = touchTapSeq;

  if (bMode == B_SLEEP || bMode == B_DROWSY) {
    if (pressed) {
      ignoreTapUntil = now + 1200;        // uyandiran dokunus "sevilme" sayilmasin
      enterMode(B_WAKING);
    }
    return;
  }

  if (bMode == B_WAKING || bMode == B_ANGRY) return;
  if (!tapped || now < ignoreTapUntil) return;

  tapTimes[tapIndex] = now;
  tapIndex = (tapIndex + 1) % ANGRY_TAP_COUNT;

  uint32_t recent = 0;
  for (uint32_t t : tapTimes) {
    if (t != 0 && now - t <= ANGRY_TAP_WINDOW_MS) recent++;
  }

  if (recent >= ANGRY_TAP_COUNT) {
    memset(tapTimes, 0, sizeof(tapTimes));
    enterMode(B_ANGRY);
  } else {
    enterMode(B_LOVE);
  }
}

static void roamStep(uint32_t now) {
  nextRoamAt = now + random(ROAM_MIN_MS, ROAM_MAX_MS + 1);
  int r = random(0, 100);

  if (r < 10) {                                   // cift goz kirpma
    roboEyes.blink();
    evt = EVT_BLINK2;
    evtAt = now + 380;
  } else if (r < 18) {                            // once sola, sonra saga hizli bakis
    eyesLook(7, true);
    evt = EVT_LOOK_BACK;
    evtAt = now + 900;
  } else if (r < 24 && now - lastInteractionMs > 40000) {   // esneme
    pendingSound = SND_YAWN;
    roboEyes.setMood(TIRED);
    roboEyes.setAutoblinker(OFF, 3, 2);
    roboEyes.close();
    evt = EVT_YAWN_OPEN;
    evtAt = now + 1300;
    nextRoamAt = now + 5000;
  } else {                                        // normal: yeni konuma bak
    int idx;
    do { idx = random(0, 9); } while (idx == gazeIndex);
    eyesLook(idx, true);
  }
}

static void runPendingEvent(uint32_t now) {
  if (evt == EVT_NONE || now < evtAt) return;

  uint8_t e = evt;
  evt = EVT_NONE;

  switch (e) {
    case EVT_BLINK2:
      roboEyes.blink();
      break;
    case EVT_LOOK_BACK:
      eyesLook(3, true);
      break;
    case EVT_YAWN_OPEN:
      roboEyes.open();
      evt = EVT_YAWN_END;
      evtAt = now + 800;
      break;
    case EVT_YAWN_END:
      roboEyes.setMood(DEFAULT);
      roboEyes.setAutoblinker(ON, 3, 2);
      break;
  }
}

static void angryStep(uint32_t now) {
  if (now < shakeStartAt || now >= shakeStartAt + SHAKE_MS) return;

  float elapsed = static_cast<float>(now - shakeStartAt);
  float env = sinf(PI * elapsed / SHAKE_MS);                       // 0 -> 1 -> 0
  float ang = SERVO_CENTER + 9.0f * env * sinf(2.0f * PI * 5.0f * elapsed / 1000.0f);

  headMoving = false;
  headWriteAngle(ang);
}

// Yukleme animasyonundan cikinca guncel konusma fazina gore mod secilir.
static BMode modeAfterLoader() {
  switch (appPhase) {
    case PHASE_LISTENING: return B_LISTEN;
    case PHASE_THINKING:  return B_THINK;
    case PHASE_SPEAKING:  return B_SPEAK;
    default:              return B_ROAM;
  }
}

static void updateModeTimers(uint32_t now) {
  uint32_t inMode = now - bModeSince;

  switch (bMode) {
    case B_ROAM:
      if (now - lastInteractionMs >= DROWSY_AFTER_MS) { enterMode(B_DROWSY); break; }
      if (now >= nextRoamAt) roamStep(now);
      break;

    case B_DROWSY:
      if (now - lastInteractionMs >= SLEEP_AFTER_MS) enterMode(B_SLEEP);
      break;

    case B_UNTHINK:
      if (inMode >= LOADER_MORPH_MS) enterMode(modeAfterLoader());
      break;

    case B_LOVE:     if (inMode >= LOVE_MS)     enterMode(B_ROAM); break;
    case B_FAREWELL: if (inMode >= FAREWELL_MS) enterMode(B_ROAM); break;
    case B_WAKING:   if (inMode >= WAKE_MS)     enterMode(B_ROAM); break;

    case B_ANGRY:
      angryStep(now);
      if (inMode >= ANGRY_MS) enterMode(B_ROAM);
      break;

    default:
      break;
  }
}
static void drawLoader(uint32_t now) {
  static uint32_t lastDraw = 0;
  if (now - lastDraw < 25) return;                       // ~40 kare/sn
  lastDraw = now;

  float p = static_cast<float>(now - bModeSince) / LOADER_MORPH_MS;
  if (p > 1.0f) p = 1.0f;
  if (bMode == B_UNTHINK) p = 1.0f - p;                  // geri donus: daireden goze

  float e = easeInOut(p);                                // 0 = goz, 1 = daire

  float size = EYE_BASE_WIDTH + (LOADER_DIAMETER - EYE_BASE_WIDTH) * e;
  float radius = EYE_RADIUS + (LOADER_DIAMETER / 2.0f - EYE_RADIUS) * e;
  float off = LOADER_AMPLITUDE * e *
              sinf(2.0f * PI * (now % LOADER_PERIOD_MS) / LOADER_PERIOD_MS);

  const int totalW = 2 * EYE_BASE_WIDTH + EYE_GAP;
  const float eyeCxL = (OLED_WIDTH - totalW) / 2.0f + EYE_BASE_WIDTH / 2.0f;
  const float eyeCxR = eyeCxL + EYE_BASE_WIDTH + EYE_GAP;

  const float mid = OLED_WIDTH / 2.0f;
  const float loaderCxL = mid - LOADER_CENTER_DISTANCE / 2.0f;
  const float loaderCxR = mid + LOADER_CENTER_DISTANCE / 2.0f;

  // Goz merkezinden daire merkezine yumusak kayis: gozler gecis sirasinda iceri dogru toplanir.
  const float cxL = eyeCxL + (loaderCxL - eyeCxL) * e;
  const float cxR = eyeCxR + (loaderCxR - eyeCxR) * e;
  const float cy = OLED_HEIGHT / 2.0f;

  display.clearDisplay();
  display.fillRoundRect(lroundf(cxL - size / 2), lroundf(cy + off - size / 2),
                        lroundf(size), lroundf(size), lroundf(radius), SH110X_WHITE);
  display.fillRoundRect(lroundf(cxR - size / 2), lroundf(cy - off - size / 2),
                        lroundf(size), lroundf(size), lroundf(radius), SH110X_WHITE);
  display.display();
}

static void behaviorStep() {
  uint32_t now = millis();
  AppPhase phase = appPhase;
  bool inConversation = (phase == PHASE_LISTENING ||
                         phase == PHASE_THINKING ||
                         phase == PHASE_SPEAKING);

  if (inConversation) {
    // Konusma sirasinda dokunma, soz kesme mantigina aittir.
    lastInteractionMs = now;
    seenPress = touchPressSeq;
    seenTap = touchTapSeq;

    BMode want = (phase == PHASE_LISTENING) ? B_LISTEN
               : (phase == PHASE_THINKING)  ? B_THINK
                                            : B_SPEAK;

    if (bMode == B_UNTHINK) {
      // Yukleme animasyonundan cikis suruyor; bitince guncel faza gore mod secilir.
    } else if (bMode == B_THINK && want != B_THINK) {
      enterMode(B_UNTHINK);
    } else if (bMode != want) {
      enterMode(want);
    }
  } else {
    if (bMode == B_THINK) {
      enterMode(B_UNTHINK);              // konusma bitti/iptal: once yumusakca goze don
    } else if (bMode == B_UNTHINK) {
      // cikis suruyor
    } else if (phase == PHASE_FAREWELL && lastPhase != PHASE_FAREWELL) {
      enterMode(B_FAREWELL);
    } else if (bMode == B_LISTEN || bMode == B_SPEAK) {
      enterMode(B_ROAM);
    }

    if (bMode != B_UNTHINK) handleTouchIdle(now);
  }

  lastPhase = phase;

  updateModeTimers(now);
  runPendingEvent(now);

  if (headDelayArmed && now >= headDelayAt) {
    headDelayArmed = false;
    headMoveTo(headDelayTarget);
  }

  if (bMode == B_SPEAK) {
    // Sesin siddetine gore goz yuksekligi hafifce nabiz gibi atar.
    smoothLevel = static_cast<uint8_t>((smoothLevel * 3 + ttsLevel) / 4);
    applyEyeHeight(EYE_BASE_HEIGHT - 2 + (smoothLevel * 8) / 255);
  }

  headUpdate();

  if (bMode == B_THINK || bMode == B_UNTHINK) {
    drawLoader(now);                     // gozler iki daire: biri yukari, biri asagi
  } else {
    roboEyes.update();
  }
}

static void behaviorTask(void *parameter) {
  (void)parameter;

  seenPress = touchPressSeq;
  seenTap = touchTapSeq;
  lastInteractionMs = millis();
  enterMode(B_ROAM);

  for (;;) {
    behaviorStep();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void startBehaviorTask() {
  xTaskCreate(behaviorTask, "Behavior", 6144, nullptr, 1, nullptr);
}

// ---------- Mikrofon ----------
static constexpr gpio_num_t I2S_BCLK = GPIO_NUM_14;
static constexpr gpio_num_t I2S_WS   = GPIO_NUM_15;
static constexpr gpio_num_t I2S_DIN  = GPIO_NUM_32;

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t BUFFER_SAMPLES = 256;

static int32_t rawBuffer[BUFFER_SAMPLES];
static int16_t pcmBuffer[BUFFER_SAMPLES];
static i2s_chan_handle_t rxHandle = nullptr;

static constexpr uint16_t CHANNELS = 1;
static constexpr uint16_t BITS_PER_SAMPLE = 16;
static constexpr uint32_t BYTE_RATE =
    SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8;

// ---------- WAV ----------
bool writeWavHeader(File &file, uint32_t dataSize) {
  uint8_t h[44] = {};

  auto put16 = [&](int p, uint16_t v) {
    h[p] = v & 0xFF;
    h[p + 1] = (v >> 8) & 0xFF;
  };

  auto put32 = [&](int p, uint32_t v) {
    h[p] = v & 0xFF;
    h[p + 1] = (v >> 8) & 0xFF;
    h[p + 2] = (v >> 16) & 0xFF;
    h[p + 3] = (v >> 24) & 0xFF;
  };

  memcpy(h, "RIFF", 4);
  put32(4, 36 + dataSize);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  put32(16, 16);
  put16(20, 1);
  put16(22, CHANNELS);
  put32(24, SAMPLE_RATE);
  put32(28, BYTE_RATE);
  put16(32, CHANNELS * BITS_PER_SAMPLE / 8);
  put16(34, BITS_PER_SAMPLE);
  memcpy(h + 36, "data", 4);
  put32(40, dataSize);

  return file.write(h, sizeof(h)) == sizeof(h);
}

bool setupMicrophone() {
  i2s_chan_config_t channelConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

  esp_err_t err =
      i2s_new_channel(&channelConfig, nullptr, &rxHandle);

  if (err != ESP_OK) {
    Serial.printf("I2S kanal hatasi: %d\n", err);
    return false;
  }

  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);

  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = I2S_BCLK;
  config.gpio_cfg.ws = I2S_WS;
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = I2S_DIN;
  config.gpio_cfg.invert_flags.mclk_inv = false;
  config.gpio_cfg.invert_flags.bclk_inv = false;
  config.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(rxHandle, &config);
  if (err != ESP_OK) {
    Serial.printf("I2S yapilandirma hatasi: %d\n", err);
    i2s_del_channel(rxHandle);
    rxHandle = nullptr;
    return false;
  }

  err = i2s_channel_enable(rxHandle);
  if (err != ESP_OK) {
    Serial.printf("I2S etkinlestirme hatasi: %d\n", err);
    i2s_del_channel(rxHandle);
    rxHandle = nullptr;
    return false;
  }

  return true;
}

void shutdownMicrophone() {
  if (rxHandle != nullptr) {
    i2s_channel_disable(rxHandle);
    i2s_del_channel(rxHandle);
    rxHandle = nullptr;
  }
}

// ---------- Konusma algilama (VAD) ----------
static constexpr uint32_t VAD_BLOCK_MS = BUFFER_SAMPLES * 1000 / SAMPLE_RATE;   // 16 ms
static constexpr uint32_t VAD_FIRST_WAIT_MS = 6000;      // ilk tur: konusmaya baslama suresi
static constexpr uint32_t VAD_FOLLOWUP_WAIT_MS = 10000;   // takip penceresi
static constexpr uint32_t VAD_END_SILENCE_MS = 800;      // bu kadar sessizlik = bitti
static constexpr uint32_t VAD_MAX_SPEECH_MS = 8000;
static constexpr int VAD_MIN_THRESHOLD = 150;            // ortalama mutlak genlik
static constexpr int VAD_MAX_THRESHOLD = 800;
static constexpr float VAD_NOISE_FACTOR = 3.5f;
static constexpr size_t VAD_PREROLL_BLOCKS = 8;          // ~128 ms geri tampon
static constexpr size_t VAD_CAL_BLOCKS = 10;             // ~160 ms gurultu olcumu
static constexpr size_t VAD_START_BLOCKS = 2;            // ust uste 2 blok esigi asarsa konusma
static constexpr size_t VAD_TAIL_KEEP_BLOCKS = 8;        // sondaki sessizlikten ~128 ms birak
static constexpr size_t VAD_MIN_SPEECH_BLOCKS = 16;      // ~160 ms'den kisa sesler gurultu sayilir
static constexpr size_t VAD_SETTLE_BLOCKS = 14;          // mikrofon acilis gecicisini atla (~220 ms)

enum VadResult { VAD_OK, VAD_NO_SPEECH, VAD_ERROR };

// Kayittan sonra STT'ye gonderilecek gercek WAV boyutu (dosya daha buyuk kalabilir).
static size_t recordedWavBytes = 0;
static bool sttWasEmpty = false;

static int16_t vadRing[VAD_PREROLL_BLOCKS][BUFFER_SAMPLES];
static uint16_t vadRingLen[VAD_PREROLL_BLOCKS];
static size_t vadRingHead = 0;
static size_t vadRingCount = 0;

static void vadRingPush(const int16_t *s, size_t n) {
  memcpy(vadRing[vadRingHead], s, n * sizeof(int16_t));
  vadRingLen[vadRingHead] = static_cast<uint16_t>(n);
  vadRingHead = (vadRingHead + 1) % VAD_PREROLL_BLOCKS;
  if (vadRingCount < VAD_PREROLL_BLOCKS) vadRingCount++;
}

// Halka tamponu eskiden yeniye dosyaya yazar. Yazilan ornek sayisi, hatada -1.
static int32_t vadRingFlush(File &file) {
  int32_t written = 0;
  size_t idx = (vadRingHead + VAD_PREROLL_BLOCKS - vadRingCount) % VAD_PREROLL_BLOCKS;

  for (size_t k = 0; k < vadRingCount; k++) {
    size_t bytes = vadRingLen[idx] * sizeof(int16_t);

    if (file.write(reinterpret_cast<uint8_t *>(vadRing[idx]), bytes) != bytes) {
      return -1;
    }

    written += vadRingLen[idx];
    idx = (idx + 1) % VAD_PREROLL_BLOCKS;
  }

  vadRingCount = 0;
  return written;
}

static int blockLevel(const int16_t *s, size_t n) {
  if (n == 0) return 0;

  uint32_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    sum += s[i] < 0 ? -static_cast<int32_t>(s[i]) : s[i];
  }

  return static_cast<int>(sum / n);
}

// Konusma baslayana kadar bekler, bitince (veya sessizlik suresi dolunca) kaydi sonlandirir.
static VadResult recordUtterance(uint32_t waitForSpeechMs) {
  File file = LittleFS.open("/recording.wav", "w");
  if (!file) {
    Serial.println("HATA: WAV dosyasi acilamadi.");
    return VAD_ERROR;
  }

  uint8_t blank[44] = {};
  if (file.write(blank, sizeof(blank)) != sizeof(blank)) {
    file.close();
    return VAD_ERROR;
  }

  vadRingHead = 0;
  vadRingCount = 0;

  const uint32_t startMs = millis();
  const size_t endSilenceBlocks = VAD_END_SILENCE_MS / VAD_BLOCK_MS;
  const size_t maxSpeechBlocks = VAD_MAX_SPEECH_MS / VAD_BLOCK_MS;

  uint32_t samplesWritten = 0;
  bool speaking = false;
  size_t calBlocks = 0, aboveRun = 0, silenceBlocks = 0, speechBlocks = 0;
  size_t settleBlocks = 0;
  uint32_t calSum = 0;
  int noise = 0;
  int startThr = VAD_MIN_THRESHOLD;
  int endThr = VAD_MIN_THRESHOLD * 6 / 10;
  int peakLevel = 0;

  auto updateThresholds = [&]() {
    startThr = constrain(static_cast<int>(noise * VAD_NOISE_FACTOR),
                         VAD_MIN_THRESHOLD, VAD_MAX_THRESHOLD);
    endThr = startThr * 6 / 10;
  };

  Serial.println("\n[VAD] Dinleniyor...");

  while (true) {
    size_t bytesRead = 0;

    esp_err_t err = i2s_channel_read(
        rxHandle, rawBuffer, sizeof(rawBuffer), &bytesRead, 1000);

    if (err != ESP_OK || bytesRead == 0) {
      Serial.printf("I2S okuma hatasi: %d\n", err);
      file.close();
      return VAD_ERROR;
    }

    if (settleBlocks < VAD_SETTLE_BLOCKS) {
      settleBlocks++;
      continue;   // acilis gecicisini atla
    }

    size_t count = bytesRead / sizeof(int32_t);

    for (size_t i = 0; i < count; i++) {
      int32_t v = rawBuffer[i] >> 14;
      pcmBuffer[i] = static_cast<int16_t>(constrain(v, -32768, 32767));
    }

    int level = blockLevel(pcmBuffer, count);

    // 1) Ortam gurultusunu olc.
    if (calBlocks < VAD_CAL_BLOCKS) {
      calSum += level;
      calBlocks++;
      vadRingPush(pcmBuffer, count);

      if (calBlocks == VAD_CAL_BLOCKS) {
        noise = static_cast<int>(calSum / calBlocks);
        updateThresholds();
        Serial.printf("[VAD] gurultu=%d esik=%d\n", noise, startThr);
      }

      continue;
    }

    // 2) Konusma baslamasini bekle.
    if (!speaking) {
      vadRingPush(pcmBuffer, count);

      if (level > startThr) {
        aboveRun++;
      } else {
        aboveRun = 0;
        noise = (noise * 31 + level) / 32;   // gurultu tabanini yavasca guncelle
        updateThresholds();
      }

      if (aboveRun >= VAD_START_BLOCKS) {
        int32_t w = vadRingFlush(file);

        if (w < 0) {
          Serial.println("HATA: Ses verisi yazilamadi.");
          file.close();
          return VAD_ERROR;
        }

        samplesWritten += w;
        speaking = true;
        silenceBlocks = 0;
        speechBlocks = 0;
        peakLevel = 0;
        Serial.println("[VAD] Konusma basladi.");
      } else if (millis() - startMs > waitForSpeechMs) {
        file.close();
        return VAD_NO_SPEECH;
      }

      continue;
    }

    // 3) Konusuyor: kaydet, sessizlik sayacini izle.
    size_t bytes = count * sizeof(int16_t);

    if (file.write(reinterpret_cast<uint8_t *>(pcmBuffer), bytes) != bytes) {
      Serial.println("HATA: Ses verisi yazilamadi.");
      file.close();
      return VAD_ERROR;
    }

    samplesWritten += count;
    speechBlocks++;

    if (level > peakLevel) peakLevel = level;

    if (level < endThr) silenceBlocks++;
    else silenceBlocks = 0;

    if (silenceBlocks >= endSilenceBlocks || speechBlocks >= maxSpeechBlocks) {
      size_t activeBlocks =
          speechBlocks > silenceBlocks ? speechBlocks - silenceBlocks : 0;

      if (activeBlocks < VAD_MIN_SPEECH_BLOCKS) {
        // Cok kisa (tik, ofleme vb.): yok say, beklemeye don.
        Serial.println("[VAD] Cok kisa ses; yok sayildi.");
        file.seek(44);
        samplesWritten = 0;
        speaking = false;
        aboveRun = 0;
        silenceBlocks = 0;
        speechBlocks = 0;
        vadRingCount = 0;

        if (millis() - startMs > waitForSpeechMs) {
          file.close();
          return VAD_NO_SPEECH;
        }

        continue;
      }

      break;
    }
  }

  // Sondaki sessizligi kirp (yalnizca birkac blok birak).
  size_t trimBlocks =
      silenceBlocks > VAD_TAIL_KEEP_BLOCKS ? silenceBlocks - VAD_TAIL_KEEP_BLOCKS : 0;
  uint32_t trimSamples = trimBlocks * BUFFER_SAMPLES;
  if (trimSamples > samplesWritten) trimSamples = 0;

  uint32_t dataBytes = (samplesWritten - trimSamples) * sizeof(int16_t);

  if (!file.seek(0) || !writeWavHeader(file, dataBytes)) {
    Serial.println("HATA: WAV basligi yazilamadi.");
    file.close();
    return VAD_ERROR;
  }

  file.close();
  recordedWavBytes = dataBytes + 44;

  Serial.printf("[VAD] Kayit bitti: %lu ms, tepe seviye=%d, dosya=%u bayt\n",
                static_cast<unsigned long>(dataBytes / 32),
                peakLevel,
                static_cast<unsigned int>(recordedWavBytes));

  return VAD_OK;
}

static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// ---------- Akisli WAV -> Base64 JSON ----------
class WavJsonStream : public Stream {
private:
    File &file;
    const char *prefix;
    const char *suffix;

    size_t fileSize;
    size_t totalLength;
    size_t emitted = 0;

    size_t prefixPos = 0;
    size_t suffixPos = 0;
    size_t rawRead = 0;
    size_t base64Read = 0;
    size_t base64Length;

    char quartet[4];
    size_t quartetPos = 0;
    size_t quartetLength = 0;

    bool hasPeek = false;
    int peekValue = -1;
    bool readError = false;

    uint8_t blockBuf[768];          // 3'un kati: dosya 768'lik bloklarla okunur
    size_t blockPos = 0;
    size_t blockLen = 0;
    size_t fetched = 0;             // dosyadan okunan toplam bayt

    // `want` bayti RAM tamponundan verir; tampon bosalinca dosyadan toplu okur.
    size_t takeRaw(uint8_t *out, size_t want) {
        size_t got = 0;

        while (got < want) {
            if (blockPos >= blockLen) {
                size_t left = fileSize - fetched;
                if (left == 0) break;

                size_t n = left < sizeof(blockBuf) ? left : sizeof(blockBuf);
                blockLen = file.read(blockBuf, n);
                blockPos = 0;

                if (blockLen == 0) break;
                fetched += blockLen;
            }

            out[got++] = blockBuf[blockPos++];
        }

        return got;
    }

    int nextByte() {
        if (prefixPos < strlen(prefix)) {
            return prefix[prefixPos++];
        }

        if (base64Read < base64Length) {
            if (quartetPos >= quartetLength) {
                uint8_t input[3] = {};

                size_t remaining = fileSize - rawRead;
                size_t expected = remaining >= 3 ? 3 : remaining;

                size_t count = takeRaw(input, expected);

                if (count != expected) {
                    readError = true;
                    return -1;
                }

                rawRead += count;

                uint32_t value =
                    (uint32_t)input[0] << 16;

                if (count > 1) {
                    value |= (uint32_t)input[1] << 8;
                }

                if (count > 2) {
                    value |= input[2];
                }

                quartet[0] =
                    BASE64_TABLE[(value >> 18) & 0x3F];
                quartet[1] =
                    BASE64_TABLE[(value >> 12) & 0x3F];
                quartet[2] = count > 1
                    ? BASE64_TABLE[(value >> 6) & 0x3F]
                    : '=';
                quartet[3] = count > 2
                    ? BASE64_TABLE[value & 0x3F]
                    : '=';

                quartetPos = 0;
                quartetLength = 4;
            }

            base64Read++;
            return quartet[quartetPos++];
        }

        if (suffixPos < strlen(suffix)) {
            return suffix[suffixPos++];
        }

        return -1;
    }

public:
    WavJsonStream(
        File &source,
        size_t sourceSize,
        const char *jsonPrefix,
        const char *jsonSuffix
    ) : file(source),
        prefix(jsonPrefix),
        suffix(jsonSuffix),
        fileSize(sourceSize) {

        base64Length = ((fileSize + 2) / 3) * 4;

        totalLength =
            strlen(prefix) + base64Length + strlen(suffix);
    }

    int available() override {
        size_t remaining = totalLength - emitted;

        return remaining > INT_MAX
            ? INT_MAX
            : static_cast<int>(remaining);
    }

    int read() override {
        int value;

        if (hasPeek) {
            value = peekValue;
            hasPeek = false;
            peekValue = -1;
        } else {
            value = nextByte();
        }

        if (value >= 0) {
            emitted++;
        }

        return value;
    }

    int peek() override {
        if (!hasPeek) {
            peekValue = nextByte();
            hasPeek = peekValue >= 0;
        }

        return hasPeek ? peekValue : -1;
    }

    void flush() override {}

    size_t write(uint8_t) override {
        return 0;
    }

    bool failed() const {
        return readError;
    }

    size_t length() const {
        return totalLength;
    }
};

// ---------- Openrouter STT ----------
String transcribeAudio() {
    Serial.println("\nOpenRouter Deepgram Nova-3 STT baslatiliyor...");
    sttWasEmpty = false;

    File wav = LittleFS.open("/recording.wav", "r");

    if (!wav) {
        Serial.println("HATA: WAV dosyasi acilamadi.");
        return "";
    }

    size_t fileSize = recordedWavBytes;   // dosyada artik fazladan bayt kalmis olabilir

    if (fileSize < 44 || fileSize > 300000 || fileSize > wav.size()) {
        Serial.printf(
            "HATA: WAV dosya boyutu gecersiz: %u bayt\n",
            (unsigned int)fileSize
        );

        wav.close();
        return "";
    }

    const char *prefix =
        "{\"model\":\"deepgram/nova-3\","
        "\"stream\":true,"
        "\"input_audio\":{\"data\":\"";

    const char *suffix =
        "\",\"format\":\"wav\"},\"language\":\"tr\"}";

    WavJsonStream payload(
        wav,
        fileSize,
        prefix,
        suffix
    );

    WiFiClientSecure client;
    client.setInsecure(); // Yalnizca prototip testi icin.

    HTTPClient http;
    http.setTimeout(60000);

    if (!http.begin(
            client,
            "https://openrouter.ai/api/v1/audio/transcriptions")) {
        Serial.println(
            "HATA: OpenRouter STT baglantisi baslatilamadi."
        );

        wav.close();
        return "";
    }

    const char *responseHeaders[] = {"Content-Type"};
    http.collectHeaders(responseHeaders, 1);

    http.addHeader(
        "Authorization",
        String("Bearer ") + OPENROUTER_API_KEY
    );

    http.addHeader("Content-Type", "application/json");

    Serial.printf(
        "WAV boyutu: %u bayt\n",
        (unsigned int)fileSize
    );

    Serial.printf(
        "Akisli JSON istegi: %u bayt\n",
        (unsigned int)payload.length()
    );

    Serial.println("Nova-3 istegi gonderiliyor...");
  
    MARK("STT istek basliyor");

    int status = http.sendRequest(
        "POST",
        &payload,
        payload.length()
    );

    String responseBody = http.getString();

    MARK("STT yanit geldi");

    Serial.printf("STT HTTP durumu: %d\n", status);

    bool streamFailed = payload.failed();

    Serial.printf(
        "STT yanit Content-Type: %s\n",
        http.header("Content-Type").c_str()
    );

    http.end();
    wav.close();

    if (streamFailed) {
        Serial.println(
            "HATA: WAV verisi akisa aktarilirken okuma hatasi."
        );
        return "";
    }

    if (status < 200 || status >= 300) {
        Serial.println("STT API hatasi:");
        Serial.println(responseBody);
        return "";
    }

    JsonDocument result;

    DeserializationError error =
        deserializeJson(result, responseBody);

    if (error) {
        Serial.print("STT JSON okuma hatasi: ");
        Serial.println(error.c_str());
        Serial.println(responseBody);
        return "";
    }

    String transcript = result["text"] | "";

    transcript.trim();

    if (transcript.isEmpty()) {
        Serial.println("[STT] Metin bos (anlasilir konusma yok).");
        sttWasEmpty = true;
        return "";
    }

    Serial.println("\n========== TANINAN METIN ==========");
    Serial.println(transcript);
    Serial.println("===================================");

    return transcript;
}

// =====================================================================
//  Hoparlor (I2S TX)
// =====================================================================

// MAX98357A pinleri
static constexpr gpio_num_t TTS_BCLK = GPIO_NUM_14;
static constexpr gpio_num_t TTS_WS   = GPIO_NUM_15;
static constexpr gpio_num_t TTS_DOUT = GPIO_NUM_23;

static constexpr uint32_t TTS_SAMPLE_RATE = 24000;
static constexpr float TTS_VOLUME = 0.7f;   // 0.0 - 1.0

// playPcmChunk icin: 1536 ornek (3072 bayt) -> stereo 32 bit.
static int32_t ttsI2sBuffer[3072];

static i2s_chan_handle_t txHandle = nullptr;

bool setupSpeaker() {
  i2s_chan_config_t channelConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

  channelConfig.auto_clear = true;     // tampon bosalirsa sessizlik cal
  channelConfig.dma_desc_num = 12;
  channelConfig.dma_frame_num = 240;   // 12 x 240 / 24000 = ~120 ms tampon

  esp_err_t err =
      i2s_new_channel(&channelConfig, &txHandle, nullptr);

  if (err != ESP_OK) {
    Serial.printf("I2S TX kanal hatasi: %d\n", err);
    return false;
  }

  i2s_std_config_t config = {};

  config.clk_cfg =
      I2S_STD_CLK_DEFAULT_CONFIG(TTS_SAMPLE_RATE);

  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
    I2S_DATA_BIT_WIDTH_32BIT,
    I2S_SLOT_MODE_STEREO
);

  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = TTS_BCLK;
  config.gpio_cfg.ws = TTS_WS;
  config.gpio_cfg.dout = TTS_DOUT;
  config.gpio_cfg.din = I2S_GPIO_UNUSED;

  config.gpio_cfg.invert_flags.mclk_inv = false;
  config.gpio_cfg.invert_flags.bclk_inv = false;
  config.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(txHandle, &config);

  if (err != ESP_OK) {
    Serial.printf("I2S TX yapilandirma hatasi: %d\n", err);
    i2s_del_channel(txHandle);
    txHandle = nullptr;
    return false;
  }

  err = i2s_channel_enable(txHandle);

  if (err != ESP_OK) {
    Serial.printf("I2S TX baslatma hatasi: %d\n", err);
    i2s_del_channel(txHandle);
    txHandle = nullptr;
    return false;
  }

  return true;
}

// Kapanista kisa sessizlik yazip kanali kapatir (tik sesini onler).
void shutdownSpeaker() {
  if (txHandle == nullptr) return;

  memset(ttsI2sBuffer, 0, sizeof(ttsI2sBuffer));
  for (int i = 0; i < 3; i++) {
    size_t w = 0;
    i2s_channel_write(txHandle, ttsI2sBuffer, sizeof(ttsI2sBuffer), &w, 1000);
  }
  delay(100);

  i2s_channel_disable(txHandle);
  i2s_del_channel(txHandle);
  txHandle = nullptr;
}

// Tepeleri sert kirpmak yerine yumusakca sikistirir: kirpilma cizirtisi yerine hafif sikisma.
static inline int16_t softLimit(float x) {
  constexpr float KNEE = 22000.0f;
  constexpr float HEADROOM = 32700.0f - KNEE;

  float a = x < 0.0f ? -x : x;

  if (a > KNEE) {
    float u = (a - KNEE) / HEADROOM;
    a = KNEE + HEADROOM * (u / (1.0f + u));
  }

  return static_cast<int16_t>(x < 0.0f ? -a : a);
}

// Mono 16-bit PCM sesini stereo 32-bit I2S formatina aktar.
bool playPcmChunk(const uint8_t *pcm, size_t pcmLength) {
  size_t sampleCount = pcmLength / 2;

  if (sampleCount > 1536) {
    Serial.println("HATA: PCM parcasi tamponu asiyor.");
    return false;
  }

  int32_t peak = 0;

  for (size_t i = 0; i < sampleCount; i++) {
    uint16_t raw =
        static_cast<uint16_t>(pcm[i * 2]) |
        (static_cast<uint16_t>(pcm[i * 2 + 1]) << 8);

    int16_t sample = static_cast<int16_t>(raw);

    int32_t a = sample < 0 ? -static_cast<int32_t>(sample) : sample;
    if (a > peak) peak = a;

    // 16-bit ornegi 32-bit slotun ust bitlerine yerlestir.
    int32_t scaled = static_cast<int32_t>(softLimit(sample * TTS_VOLUME)) * 65536L;

    ttsI2sBuffer[i * 2] = scaled;       // Sol kanal
    ttsI2sBuffer[i * 2 + 1] = scaled;   // Sag kanal
  }

  int32_t lv = peak / 96;
  ttsLevel = static_cast<uint8_t>(lv > 255 ? 255 : lv);

  size_t totalBytes = sampleCount * 2 * sizeof(int32_t);
  size_t offset = 0;

  while (offset < totalBytes) {
    size_t written = 0;

    esp_err_t err = i2s_channel_write(
        txHandle,
        reinterpret_cast<const uint8_t *>(ttsI2sBuffer) + offset,
        totalBytes - offset,
        &written,
        1000
    );

    if (err != ESP_OK || written == 0) {
      Serial.printf("I2S ses yazma hatasi: %d\n", err);
      return false;
    }

    offset += written;
  }

  return true;
}

// =====================================================================
//  Yerel ses efektleri (internet gerekmez)
// =====================================================================

static void playToneSweep(float f0, float f1, uint32_t ms, int16_t amp) {
  static int16_t buf[512];

  uint32_t total = TTS_SAMPLE_RATE * ms / 1000;
  uint32_t edge = TTS_SAMPLE_RATE / 100;          // 10 ms yumusatma
  float phase = 0.0f;
  uint32_t done = 0;

  while (done < total) {
    size_t n = total - done;
    if (n > 512) n = 512;

    for (size_t i = 0; i < n; i++) {
      uint32_t k = done + i;
      float freq = f0 + (f1 - f0) * (static_cast<float>(k) / total);

      phase += 2.0f * PI * freq / TTS_SAMPLE_RATE;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;

      float env = 1.0f;
      if (k < edge) env = static_cast<float>(k) / edge;
      else if (total - k < edge) env = static_cast<float>(total - k) / edge;

      buf[i] = static_cast<int16_t>(sinf(phase) * amp * env);
    }

    if (!playPcmChunk(reinterpret_cast<const uint8_t *>(buf), n * 2)) return;
    done += n;
  }
}

static void playSound(SoundId id) {
  if (id == SND_NONE) return;
  if (!setupSpeaker()) return;

  switch (id) {
    case SND_LISTEN:
      playToneSweep(660, 880, 90, 12000);
      break;
    case SND_HAPPY:
      playToneSweep(700, 1100, 90, 12000);
      playToneSweep(100, 100, 40, 0);
      playToneSweep(900, 1400, 110, 12000);
      break;
    case SND_ANGRY:
      playToneSweep(200, 160, 160, 12000);
      playToneSweep(100, 100, 60, 0);
      playToneSweep(190, 150, 200, 12000);
      break;
    case SND_YAWN:
      playToneSweep(520, 220, 650, 9000);
      break;
    default:
      break;
  }

  shutdownSpeaker();
}

// Davranis motorunun biraktigi istegi calar (yalnizca loop() cagirir).
static void playPendingSound() {
  SoundId id = pendingSound;
  if (id == SND_NONE) return;
  pendingSound = SND_NONE;
  playSound(id);
}

// =====================================================================
//  Ses hatti: ortak durum
// =====================================================================

static constexpr size_t TTS_STREAM_BUFFER_SIZE = 32768;
static constexpr size_t TTS_PREBUFFER_SIZE = 4096;
static constexpr uint32_t TTS_WORKER_STACK = 12288;   // TLS icin genis yigin
static constexpr UBaseType_t TTS_QUEUE_LENGTH = 16;

static constexpr size_t SENTENCE_MIN_CHARS = 12;
static constexpr size_t SENTENCE_MAX_CHARS = 180;

static StreamBufferHandle_t ttsStreamBuffer = nullptr;
static QueueHandle_t ttsSentenceQueue = nullptr;   // char* (nullptr = bitis)

static volatile bool ttsStreamDone = false;        // tum PCM tampona yazildi
static volatile bool ttsWorkerFinished = false;
static volatile bool ttsPlaybackFinished = false;
static volatile bool ttsPlaybackFailed = false;
static volatile size_t ttsPlayedBytes = 0;
static bool ttsFirstSentenceMarked = false;
static std::vector<String> sentenceBacklog;   // LLM bitene kadar biriken cümleler

static uint8_t ttsPlaybackBuffer[3074];
static uint8_t ttsNetBuffer[1024];
static volatile bool speechAbort = false;

static uint32_t bargePressSeen = 0;

// Konusma sirasinda dokunma = soz kesme.
static void checkBargeIn() {
    bool pressed = (touchPressSeq != bargePressSeen);
    bargePressSeen = touchPressSeq;

    if (pressed && !speechAbort) {
        speechAbort = true;
        ttsPlaybackFailed = true;   // oynatici ve isci hemen durur
        Serial.println("[AKIS] Dokunma: konusma kesildi.");
    }
}

// =====================================================================
//  Tampona yazma ve elle chunked okuma
// =====================================================================

// Veriyi akis tamponuna yazar; tampon doluysa bekler.
static bool pushToTtsBuffer(const uint8_t *data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        if (ttsPlaybackFailed || ttsPlaybackFinished) return false;

        sent += xStreamBufferSend(
            ttsStreamBuffer,
            data + sent,
            length - sent,
            pdMS_TO_TICKS(100)
        );
    }

    return true;
}

// Bir satiri (\n'e kadar) okur; \r yok sayilir.
static bool readHttpLine(WiFiClient *c, char *out, size_t cap,
                         uint32_t timeoutMs) {
    size_t len = 0;
    uint32_t last = millis();

    while (millis() - last < timeoutMs) {
        if (ttsPlaybackFailed) return false;

        if (c->available() > 0) {
            int v = c->read();
            if (v < 0) continue;

            last = millis();

            if (v == '\n') {
                out[len] = '\0';
                return true;
            }

            if (v != '\r' && len + 1 < cap) {
                out[len++] = static_cast<char>(v);
            }
        } else if (!c->connected()) {
            return false;
        } else {
            delay(1);
        }
    }

    return false;
}

// Tam `count` bayti okuyup akis tamponuna aktarir.
static bool pumpBytes(WiFiClient *c, size_t count, uint32_t timeoutMs,
                      size_t &total) {
    uint32_t last = millis();

    while (count > 0) {
        if (ttsPlaybackFailed) return false;

        int avail = c->available();

        if (avail > 0) {
            size_t want = static_cast<size_t>(avail);
            if (want > count) want = count;
            if (want > sizeof(ttsNetBuffer)) want = sizeof(ttsNetBuffer);

            int got = c->read(ttsNetBuffer, want);

            if (got > 0) {
                if (!pushToTtsBuffer(ttsNetBuffer, got)) return false;
                count -= got;
                total += got;
                last = millis();
                continue;
            }
        } else if (!c->connected()) {
            return false;
        }

        if (millis() - last > timeoutMs) return false;
        delay(1);
    }

    return true;
}

// HTTP govdesini (chunked veya sabit uzunluk) akis tamponuna aktarir.
// `total`, hata olsa bile tampona yazilan bayt sayisini verir.
static bool streamTtsBody(WiFiClient *c, int contentLength, size_t &total) {
    constexpr uint32_t STEP_TIMEOUT_MS = 15000;
    total = 0;

    if (contentLength >= 0) {
        if (!pumpBytes(c, contentLength, STEP_TIMEOUT_MS, total)) {
            Serial.println("[TTS] HATA: govde okunamadi.");
            return false;
        }
        return true;
    }

    char line[24];

    while (true) {
        if (!readHttpLine(c, line, sizeof(line), STEP_TIMEOUT_MS)) {
            Serial.printf("[TTS] HATA: chunk basligi okunamadi (alinan: %u).\n",
                          static_cast<unsigned int>(total));
            return false;
        }

        if (line[0] == '\0') continue;

        size_t chunkSize = strtoul(line, nullptr, 16);

        if (chunkSize == 0) {
            // Akisin sonu: kapanistaki bos satiri tuket (baglanti yeniden kullanilabilsin).
            readHttpLine(c, line, sizeof(line), 2000);
            return true;
        }

        if (!pumpBytes(c, chunkSize, STEP_TIMEOUT_MS, total)) {
            Serial.printf("[TTS] HATA: chunk verisi eksik (alinan: %u).\n",
                          static_cast<unsigned int>(total));
            return false;
        }

        // Chunk sonundaki CRLF.
        if (!readHttpLine(c, line, sizeof(line), STEP_TIMEOUT_MS)) {
            Serial.println("[TTS] HATA: chunk sonu (CRLF) okunamadi.");
            return false;
        }
    }
}

// =====================================================================
//  Oynatma gorevi
// =====================================================================

static void ttsPlaybackTask(void *parameter) {
    (void)parameter;

    // Ilk ses icin yeterli veri birikene (veya akis bitene) kadar bekle.
    while (
        xStreamBufferBytesAvailable(ttsStreamBuffer) < TTS_PREBUFFER_SIZE
        && !ttsStreamDone
        && !ttsPlaybackFailed
    ) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (!ttsPlaybackFailed) {
        MARK("ilk ses");
        appPhase = PHASE_SPEAKING;

        bool hasCarry = false;
        uint8_t carry = 0;

        while (!ttsPlaybackFailed) {
            size_t received = xStreamBufferReceive(
                ttsStreamBuffer,
                ttsPlaybackBuffer + (hasCarry ? 1 : 0),
                3072 - (hasCarry ? 1 : 0),
                pdMS_TO_TICKS(100)
            );

            if (hasCarry) {
                ttsPlaybackBuffer[0] = carry;
                received += 1;
                hasCarry = false;
            }

            if (received == 0) {
                if (
                    ttsStreamDone
                    && xStreamBufferBytesAvailable(ttsStreamBuffer) == 0
                ) {
                    break;
                }

                continue;
            }

            if (received % 2 != 0) {
                carry = ttsPlaybackBuffer[received - 1];
                hasCarry = true;
                received--;
            }

            if (received == 0) {
                continue;
            }

            if (!playPcmChunk(ttsPlaybackBuffer, received)) {
                Serial.println("[TTS] HATA: playPcmChunk basarisiz.");
                ttsPlaybackFailed = true;
                break;
            }

            ttsPlayedBytes += received;
        }
    }

    Serial.printf(
        "[TTS] Oynatma bitti. Calinan veri: %u bayt\n",
        static_cast<unsigned int>(ttsPlayedBytes)
    );

    ttsLevel = 0;
    ttsPlaybackFinished = true;
    vTaskDelete(nullptr);
}

// =====================================================================
//  TTS iscisi: kuyruktaki cumleleri sirayla seslendirir
// =====================================================================

// Donus: 1 = tamam, 0 = kalici hata, -1 = baglanti hatasi (yeniden denenebilir)
static int ttsSynthesizeSentence(HTTPClient &http,
                                 WiFiClientSecure &client,
                                 const char *text) {
    if (!http.begin(client, "https://openrouter.ai/api/v1/audio/speech")) {
        return -1;
    }

    http.addHeader("Authorization", String("Bearer ") + OPENROUTER_API_KEY);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "audio/pcm");

    JsonDocument request;
    request["model"] = "elevenlabs/eleven-flash-v2.5";
    request["input"] = text;
    request["voice"] = "bella";
    request["response_format"] = "pcm";
    request["provider"]["options"]["elevenlabs"]["language_code"] = "tr";

    String body;
    serializeJson(request, body);

    int status = http.POST(body);

    MARK("TTS yanit basliklari");

    if (status < 0) {
        Serial.printf("[TTS] Baglanti hatasi: %s\n",
                      http.errorToString(status).c_str());
        http.end();
        return -1;
    }

    if (status < 200 || status >= 300) {
        Serial.printf("[TTS] HTTP hatasi: %d\n", status);
        Serial.println(http.getString());
        http.end();
        client.stop();
        return 0;
    }

    size_t total = 0;
    WiFiClient *net = http.getStreamPtr();
    bool ok = (net != nullptr) && streamTtsBody(net, http.getSize(), total);

    http.end();
    if (!ok) client.stop();

    // Cumleler arasi hizalama: tek bayt kalirsa sifirla tamamla.
    if (total % 2 != 0) {
        uint8_t pad = 0;
        pushToTtsBuffer(&pad, 1);
    }

    Serial.printf("[TTS] Cumle aktarildi: %u bayt\n",
                  static_cast<unsigned int>(total));

    return ok ? 1 : 0;
}

// Nesneler burada olusup yok olur (vTaskDelete destructor calistirmaz).
static void ttsWorkerRun() {
    WiFiClientSecure client;
    client.setInsecure();   // Yalnizca prototip.

    HTTPClient http;        // Tum cumleler icin ayni nesne: baglanti yeniden kullanilir.
    http.setTimeout(30000);
    http.setReuse(true);

    while (true) {
        char *sentence = nullptr;
        xQueueReceive(ttsSentenceQueue, &sentence, portMAX_DELAY);

        if (sentence == nullptr) break;   // bitis isareti

        if (!ttsPlaybackFailed) {
            int r = ttsSynthesizeSentence(http, client, sentence);

            if (r < 0) {
                // Eski baglanti kopmus olabilir; temiz baglantiyla bir kez daha dene.
                client.stop();
                r = ttsSynthesizeSentence(http, client, sentence);
            }

            if (r != 1  && !speechAbort) {
                Serial.println("[TTS] UYARI: cumle seslendirilemedi.");
            }
        }

        free(sentence);
    }
}

static void ttsWorkerTask(void *parameter) {
    (void)parameter;

    ttsWorkerRun();

    ttsStreamDone = true;
    ttsWorkerFinished = true;
    vTaskDelete(nullptr);
}

// =====================================================================
//  Cumle ayirma
// =====================================================================

static void enqueueSentence(const String &s) {
    if (!ttsFirstSentenceMarked) {
        ttsFirstSentenceMarked = true;
        MARK("ilk cumle");
    }

    Serial.printf("[TTS-KUYRUK] %s\n", s.c_str());
    sentenceBacklog.push_back(s);
}

// `pending` icindeki tamamlanmis cumleleri kuyruga yollar.
// Cumle sonu: . ! ? (ardindan bosluk) veya satir sonu.
static void flushSentences(String &pending, bool finalFlush) {
    while (true) {
        int n = pending.length();
        int cut = -1;

        for (int i = 0; i < n; i++) {
            char ch = pending[i];

            if (ch == '\n') {
                cut = i + 1;
                break;
            }

            if (ch == '.' || ch == '!' || ch == '?') {
                int j = i + 1;

                while (j < n && (pending[j] == '"' ||
                                 pending[j] == '\'' ||
                                 pending[j] == ')')) {
                    j++;
                }

                if (j >= n) break;   // sonraki karakter henuz gelmedi

                if (isspace(static_cast<unsigned char>(pending[j])) &&
                    j >= static_cast<int>(SENTENCE_MIN_CHARS)) {
                    cut = j;
                    break;
                }
            }
        }

        // Cok uzun cumle: son bosluktan bol.
        if (cut < 0 && n > static_cast<int>(SENTENCE_MAX_CHARS)) {
            int sp = pending.lastIndexOf(' ', SENTENCE_MAX_CHARS);
            if (sp > static_cast<int>(SENTENCE_MIN_CHARS)) cut = sp;
        }

        if (cut < 0) break;

        String s = pending.substring(0, cut);
        pending.remove(0, cut);
        s.trim();

        if (s.length() > 0) enqueueSentence(s);
    }

    if (finalFlush) {
        pending.trim();
        if (pending.length() > 0) enqueueSentence(pending);
        pending = "";
    }
}

// =====================================================================
//  Ses hattinin kurulumu ve kapanisi
// =====================================================================

static void releaseSpeechResources() {
    if (ttsSentenceQueue != nullptr) {
        char *leftover = nullptr;
        while (xQueueReceive(ttsSentenceQueue, &leftover, 0) == pdTRUE) {
            free(leftover);
        }
        vQueueDelete(ttsSentenceQueue);
        ttsSentenceQueue = nullptr;
    }

    if (ttsStreamBuffer != nullptr) {
        vStreamBufferDelete(ttsStreamBuffer);
        ttsStreamBuffer = nullptr;
    }

    shutdownSpeaker();
}

static bool startSpeechPipeline() {
    if (!setupSpeaker()) return false;

    ttsStreamBuffer = xStreamBufferCreate(TTS_STREAM_BUFFER_SIZE, 1);
    ttsSentenceQueue = xQueueCreate(TTS_QUEUE_LENGTH, sizeof(char *));

    if (ttsStreamBuffer == nullptr || ttsSentenceQueue == nullptr) {
        Serial.println("HATA: TTS tamponu/kuyrugu olusturulamadi.");
        releaseSpeechResources();
        return false;
    }

    ttsStreamDone = false;
    ttsWorkerFinished = false;
    ttsPlaybackFinished = false;
    ttsPlaybackFailed = false;
    ttsPlayedBytes = 0;

    if (xTaskCreate(ttsPlaybackTask, "TtsPlayback", 4096,
                    nullptr, 2, nullptr) != pdPASS) {
        Serial.println("HATA: Oynatma gorevi olusturulamadi.");
        releaseSpeechResources();
        return false;
    }

    return true;
}

// Isci ve oynatma gorevlerinin bitmesini bekler, kaynaklari serbest birakir.
static bool finishSpeechPipeline(bool workerStarted) {
    if (workerStarted) {
        char *endMarker = nullptr;
        xQueueSend(ttsSentenceQueue, &endMarker, portMAX_DELAY);

        while (!ttsWorkerFinished) {
            checkBargeIn();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        ttsStreamDone = true;
    }

    while (!ttsPlaybackFinished) {
        checkBargeIn();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    bool ok = !ttsPlaybackFailed && ttsPlayedBytes > 0;

    releaseSpeechResources();

    Serial.printf("[HEAP] En dusuk bos heap: %u bayt\n",
                  static_cast<unsigned int>(ESP.getMinFreeHeap()));

    return ok;
}

// =====================================================================
//  LLM (stream) -> cumleler -> TTS kuyrugu
// =====================================================================

// Baglanti nesneleri bu fonksiyon bitince yok olur (TLS bellegi erken serbest kalir).
  static bool streamLlmToSpeech(const String &transcript,
                                String &reply) {

    Serial.println("\nOpenRouter DeepSeek LLM streaming baslatiliyor...");

    WiFiClientSecure client;
    client.setInsecure(); // Yalnizca prototip testi icin.

    HTTPClient http;
    http.setTimeout(15000);

    const char *url =
        "https://openrouter.ai/api/v1/chat/completions";

    if (!http.begin(client, url)) {
        Serial.println("HATA: OpenRouter LLM baglantisi baslatilamadi.");
        return false;
    }

    http.addHeader(
        "Authorization",
        String("Bearer ") + OPENROUTER_API_KEY
    );
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "text/event-stream");

    JsonDocument request;

    request["model"] = "deepseek/deepseek-v4.1-flash";
    request["stream"] = true;

    JsonArray messages = request["messages"].to<JsonArray>();

    JsonObject systemMessage = messages.add<JsonObject>();
    systemMessage["role"] = "system";
    systemMessage["content"] =
        "Sen MasaDaş adinda fiziksel bir masaustu robotusun. "
        "Turkce konus. Cevaplarini kisa ve dogal tut. "
        "En fazla 1-2 cumle kullan. Gereksiz aciklama yapma. "
        "Kullanicinin sorusuna dogrudan cevap ver. "
        "Samimi ama abartisiz bir uslup kullan. Emoji kullanma. "
        "Sayilari rakamla degil yaziyla yaz (ornek: yirmi uc).";

    JsonObject userMessage = messages.add<JsonObject>();
    userMessage["role"] = "user";
    userMessage["content"] = transcript;

    request["max_tokens"] = 400;
    request["reasoning"]["enabled"] = false;
    request["temperature"] = 0.7;
    request["provider"]["sort"] = "latency";

    String requestBody;
    serializeJson(request, requestBody);

    Serial.println("DeepSeek streaming istegi gonderiliyor...");

    int status = http.POST(requestBody);
    Serial.printf("LLM HTTP durumu: %d\n", status);

    MARK("LLM baglandi");

    if (status < 200 || status >= 300) {
        Serial.println("OpenRouter LLM API hatasi:");
        Serial.println(http.getString());
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();

    String line;
    String pending;
    bool streamFinished = false;
    bool llmFailed = false;
    bool gotFirstToken = false;
    unsigned long lastDataTime = millis();

    while (!streamFinished) {
        checkBargeIn();
        if (speechAbort) break;

        while (stream->available() > 0) {
            char c = static_cast<char>(stream->read());
            lastDataTime = millis();

            if (c != '\n') {
                if (c != '\r') {
                    line += c;
                }
                continue;
            }

            if (line.startsWith("data:")) {
                String data = line.substring(5);
                data.trim();

                if (data == "[DONE]") {
                    streamFinished = true;
                    line = "";
                    break;
                }

                if (!data.isEmpty()) {
                    JsonDocument event;
                    DeserializationError error =
                        deserializeJson(event, data);

                    if (!error) {
                        if (event["error"].is<JsonObject>()) {
                            Serial.println("\nLLM akis hatasi:");
                            serializeJson(event["error"], Serial);
                            Serial.println();
                            llmFailed = true;
                            streamFinished = true;
                            line = "";
                            break;
                        }

                        const char *content =
                            event["choices"][0]["delta"]["content"]
                                | "";

                        if (content[0] != '\0') {
                            if (!gotFirstToken) {
                                gotFirstToken = true;
                                MARK("LLM ilk parca");
                            }
                            reply += content;
                            pending += content;
                            flushSentences(pending, false);
                        }
                    }
                }
            }

            line = "";
        }

        if (streamFinished) {
            break;
        }

        if (!http.connected() && stream->available() == 0) {
            break;
        }

        if (millis() - lastDataTime > 15000) {
            Serial.println("\nHATA: LLM akisinda zaman asimi.");
            break;
        }

        delay(1);
    }

    http.end();

    // Kalan metni de seslendir (soz kesilmediyse).
    if (!speechAbort) flushSentences(pending, true);
    reply.trim();

    return !llmFailed && !reply.isEmpty();
}

// Cumleyi isci gorevin kuyruguna koyar; kuyruk doluysa bekler.
static bool sendSentenceToWorker(const String &s) {
    char *copy = strdup(s.c_str());
    if (copy == nullptr) return false;

    while (xQueueSend(ttsSentenceQueue, &copy, pdMS_TO_TICKS(50)) != pdTRUE) {
        checkBargeIn();

        if (speechAbort) {
            free(copy);
            return false;
        }
    }

    return true;
}

// Once LLM yaniti okunur ve baglantisi kapatilir; sonra cumleler seslendirilir.
bool respondToTranscript(const String &transcript) {
    if (transcript.isEmpty()) {
        Serial.println("HATA: LLM girdisi bos.");
        return false;
    }

    speechAbort = false;
    bargePressSeen = touchPressSeq;
    ttsPlaybackFailed = false;
    ttsFirstSentenceMarked = false;
    sentenceBacklog.clear();

    String reply;
    bool llmOk = streamLlmToSpeech(transcript, reply);   // LLM baglantisi burada kapanir

    Serial.println("\n========== MASADAS YANITI ==========");
    Serial.println(reply);
    Serial.println("====================================");

    MARK("LLM bitti");

    if (speechAbort) {
        sentenceBacklog.clear();
        return true;   // Soz kesildi; cagiran taraf dinlemeye gecer.
    }

    if (sentenceBacklog.empty()) {
        return false;
    }

    Serial.printf("[HEAP] TTS oncesi bos: %u bayt, en buyuk blok: %u bayt\n",
                  static_cast<unsigned int>(ESP.getFreeHeap()),
                  static_cast<unsigned int>(ESP.getMaxAllocHeap()));

    if (!startSpeechPipeline()) {
        Serial.println("HATA: Ses hatti baslatilamadi.");
        sentenceBacklog.clear();
        return false;
    }

    bool workerStarted = xTaskCreate(ttsWorkerTask, "TtsWorker",
                                     TTS_WORKER_STACK, nullptr, 1,
                                     nullptr) == pdPASS;

    if (!workerStarted) {
        Serial.println("HATA: TTS isci gorevi olusturulamadi.");
        finishSpeechPipeline(false);
        sentenceBacklog.clear();
        return false;
    }

    for (const String &s : sentenceBacklog) {
        if (!sendSentenceToWorker(s)) break;
    }
    sentenceBacklog.clear();

    bool speechOk = finishSpeechPipeline(true);

    return llmOk && speechOk;
}

// ---------- Konusma akisi ----------
static bool systemReady = false;
static uint32_t loopLongSeen = 0;

static bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("Wi-Fi koptu, yeniden baglaniliyor...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
    delay(250);
  }

  return WiFi.status() == WL_CONNECTED;
}

// Uzun basmayla baslar; takip penceresinde konusuldukca tur tur surer.
static void runConversation() {
  if (!ensureWiFi()) {
    Serial.println("HATA: Wi-Fi baglantisi yok.");
    return;
  }

  bool followUp = false;
  bool endedBySilence = false;
  int emptyCount = 0;

  while (true) {
    appPhase = PHASE_LISTENING;

    if (!followUp) {
      // Parmak cekilene kadar bekle (birakma sesi kaydi bozmasin).
      uint32_t t = millis();
      while (digitalRead(TOUCH_PIN) == HIGH && millis() - t < 3000) {
        delay(10);
      }
      delay(150);

      playSound(SND_LISTEN);   // "simdi konusabilirsin" bipi
      delay(150);
    }

    if (!setupMicrophone()) {
      Serial.println("HATA: Mikrofon baslatilamadi.");
      break;
    }

    VadResult vad = recordUtterance(
        followUp ? VAD_FOLLOWUP_WAIT_MS : VAD_FIRST_WAIT_MS);
    shutdownMicrophone();

    if (vad == VAD_NO_SPEECH) {
      Serial.println(followUp ? "[VAD] Takip suresi doldu; bosta."
                              : "[VAD] Konusma algilanmadi.");
      endedBySilence = followUp;
      break;
    }

    if (vad == VAD_ERROR) {
      Serial.println("HATA: Ses kaydi basarisiz.");
      break;
    }

    appPhase = PHASE_THINKING;
    tStart = millis();   // Sureler konusma bittigi andan olculur.

    String transcript = transcribeAudio();
    MARK("STT bitti");

    if (transcript.isEmpty()) {
      if (sttWasEmpty && ++emptyCount <= 2) {
        Serial.println("[AKIS] Anlasilir konusma yok; dinlemeye devam.");
        followUp = true;
        continue;
      }

      Serial.println("Akis STT asamasinda durdu.");
      break;
    }

    emptyCount = 0;

    bool ok = respondToTranscript(transcript);
    MARK("toplam");

    if (speechAbort) {
      Serial.println("[AKIS] Soz kesildi; dinlemeye geciliyor.");
    } else if (!ok) {
      Serial.println("Yanit/TTS asamasi basarisiz.");
      break;
    }

    followUp = true;
    delay(400);   // Hoparlor yankisi mikrofona karismasin.
  }

  loopLongSeen = touchLongSeq;   // konusma sirasindaki uzun basmalar yeni konusma baslatmasin
  appPhase = endedBySilence ? PHASE_FAREWELL : PHASE_IDLE;
}
// ---------- Program akisi ----------

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!setupOLED()) {
    Serial.println("OLED testi basarisiz.");
    return;
  }

  Serial.println("\n=== MasaDas ===");

  setupServo();
  setupTouch();
  startBehaviorTask();

  if (String(WIFI_SSID).isEmpty() ||
      String(WIFI_PASSWORD).isEmpty() ||
      String(OPENROUTER_API_KEY).isEmpty()) {
    Serial.println("HATA: include/secrets.h bilgilerini kontrol et.");
    return;
  }

  if (!LittleFS.begin(false)) {
    Serial.println("HATA: LittleFS acilamadi.");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Wi-Fi baglantisi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 20000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HATA: Wi-Fi baglantisi kurulamadi.");
    return;
  }

  WiFi.setSleep(false);   // Paket gecikmesi dalgalanmasini azaltir.
  Serial.println("Wi-Fi baglandi.");

  systemReady = true;
  Serial.println("\nHazir. Konusmak icin touch sensorune uzun bas.");
}

void loop() {
  if (!systemReady) {
    delay(100);
    return;
  }

  if (takeEvent(touchLongSeq, loopLongSeen)) {
    runConversation();
  }

  playPendingSound();
  delay(10);
}

