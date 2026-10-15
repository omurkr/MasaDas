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

// ---------- Goz durumlari ----------
enum EyeState : uint8_t {
  EYE_IDLE,        // bosta
  EYE_LISTENING,   // dinliyor
  EYE_THINKING,    // dusunuyor
  EYE_SPEAKING     // konusuyor
};

static volatile EyeState eyeState = EYE_IDLE;

// Yalnizca goz gorevi cagirir (RoboEyes nesnesine tek gorev dokunur).
static void applyEyeState(EyeState s) {
  switch (s) {
    case EYE_IDLE:
      roboEyes.setMood(DEFAULT);
      roboEyes.setPosition(DEFAULT);
      roboEyes.setCuriosity(OFF);
      roboEyes.setIdleMode(ON, 5, 10);
      roboEyes.setAutoblinker(ON, 3, 2);
      roboEyes.open();
      break;

    case EYE_LISTENING:
      roboEyes.setMood(DEFAULT);
      roboEyes.setPosition(DEFAULT);
      roboEyes.setIdleMode(OFF, 5, 10);
      roboEyes.setCuriosity(ON);
      roboEyes.open();
      break;

    case EYE_THINKING:
      roboEyes.setMood(TIRED);
      roboEyes.setPosition(NE);
      roboEyes.setIdleMode(OFF, 5, 10);
      roboEyes.setCuriosity(OFF);
      break;

    case EYE_SPEAKING:
      roboEyes.setMood(HAPPY);
      roboEyes.setPosition(DEFAULT);
      roboEyes.setCuriosity(OFF);
      roboEyes.setIdleMode(ON, 3, 6);
      break;
  }
}

static void eyeTask(void *parameter) {
  (void)parameter;

  EyeState applied = EYE_IDLE;

  for (;;) {
    EyeState wanted = eyeState;

    if (wanted != applied) {
      applied = wanted;
      applyEyeState(wanted);
    }

    roboEyes.update();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void startEyeTask() {
  xTaskCreate(eyeTask, "Eyes", 5120, nullptr, 1, nullptr);
}

// ---------- Servo ----------
Servo headServo;

constexpr int SERVO_PIN = 19;
constexpr int SERVO_CENTER = 90;

void setupServo() {
    headServo.setPeriodHertz(50);

    int servoChannel = headServo.attach(SERVO_PIN, 1000, 2000);

    Serial.printf("Servo attach sonucu: %d\n", servoChannel);

    if (!headServo.attached()) {
        Serial.println("HATA: Servo PWM kanali baslatilamadi!");
        return;
    }

    Serial.println("Servo PWM kanali basariyla baslatildi.");

    headServo.write(SERVO_CENTER);
    Serial.println("Servo orta konuma getirildi.");
}

// ---------- Touch ----------
constexpr int TOUCH_PIN = 4;
void setupTouch() {
    pinMode(TOUCH_PIN, INPUT);
    Serial.println("Touch sensor hazir.");
}

static constexpr unsigned long TOUCH_DEBOUNCE_MS = 40;
static constexpr unsigned long TOUCH_LONG_PRESS_MS = 800;

// Sik sik cagirin. pressed: yeni basildi, longPress: uzun basma tamamlandi (basista bir kez).
static void pollTouch(bool &pressed, bool &longPress) {
  static int lastRaw = LOW;
  static int stable = LOW;
  static unsigned long lastChange = 0;
  static unsigned long pressStart = 0;
  static bool longFired = false;

  pressed = false;
  longPress = false;

  unsigned long now = millis();
  int raw = digitalRead(TOUCH_PIN);

  if (raw != lastRaw) {
    lastRaw = raw;
    lastChange = now;
  }

  if (now - lastChange >= TOUCH_DEBOUNCE_MS && raw != stable) {
    stable = raw;

    if (stable == HIGH) {
      pressStart = now;
      longFired = false;
      pressed = true;
    }
  }

  if (stable == HIGH && !longFired &&
      now - pressStart >= TOUCH_LONG_PRESS_MS) {
    longFired = true;
    longPress = true;
  }
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
static constexpr uint32_t VAD_FOLLOWUP_WAIT_MS = 5000;   // takip penceresi
static constexpr uint32_t VAD_END_SILENCE_MS = 800;      // bu kadar sessizlik = bitti
static constexpr uint32_t VAD_MAX_SPEECH_MS = 8000;
static constexpr int VAD_MIN_THRESHOLD = 150;            // ortalama mutlak genlik
static constexpr int VAD_MAX_THRESHOLD = 800;
static constexpr float VAD_NOISE_FACTOR = 3.5f;
static constexpr size_t VAD_PREROLL_BLOCKS = 8;          // ~128 ms geri tampon
static constexpr size_t VAD_CAL_BLOCKS = 10;             // ~160 ms gurultu olcumu
static constexpr size_t VAD_START_BLOCKS = 2;            // ust uste 2 blok esigi asarsa konusma
static constexpr size_t VAD_TAIL_KEEP_BLOCKS = 8;        // sondaki sessizlikten ~128 ms birak
static constexpr size_t VAD_MIN_SPEECH_BLOCKS = 10;      // ~160 ms'den kisa sesler gurultu sayilir

enum VadResult { VAD_OK, VAD_NO_SPEECH, VAD_ERROR };

// Kayittan sonra STT'ye gonderilecek gercek WAV boyutu (dosya daha buyuk kalabilir).
static size_t recordedWavBytes = 0;

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

    int nextByte() {
        if (prefixPos < strlen(prefix)) {
            return prefix[prefixPos++];
        }

        if (base64Read < base64Length) {
            if (quartetPos >= quartetLength) {
                uint8_t input[3] = {};

                size_t remaining = fileSize - rawRead;
                size_t expected = remaining >= 3 ? 3 : remaining;

                size_t count = file.read(input, expected);

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

    int status = http.sendRequest(
        "POST",
        &payload,
        payload.length()
    );

    String responseBody = http.getString();

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

    if (transcript.isEmpty()) {
        Serial.println(
            "HATA: STT yanitinda metin bulunamadi."
        );
        Serial.println(responseBody);
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
static constexpr float TTS_VOLUME = 0.3f;   // 0.0 - 1.0

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

// Mono 16-bit PCM sesini stereo 32-bit I2S formatina aktar.
bool playPcmChunk(const uint8_t *pcm, size_t pcmLength) {
  size_t sampleCount = pcmLength / 2;

  if (sampleCount > 1536) {
    Serial.println("HATA: PCM parcasi tamponu asiyor.");
    return false;
  }

  for (size_t i = 0; i < sampleCount; i++) {
    uint16_t raw =
        static_cast<uint16_t>(pcm[i * 2]) |
        (static_cast<uint16_t>(pcm[i * 2 + 1]) << 8);

    int16_t sample = static_cast<int16_t>(raw);

    // 16-bit ornegi 32-bit slotun ust bitlerine yerlestir.
    int32_t scaled = static_cast<int32_t>(sample * TTS_VOLUME) * 65536L;

    ttsI2sBuffer[i * 2] = scaled;       // Sol kanal
    ttsI2sBuffer[i * 2 + 1] = scaled;   // Sag kanal
  }

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

// Konusma sirasinda dokunma = soz kesme.
static void checkBargeIn() {
    bool pressed = false, longPress = false;
    pollTouch(pressed, longPress);

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
        eyeState = EYE_SPEAKING;

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

    String requestBody;
    serializeJson(request, requestBody);

    Serial.println("DeepSeek streaming istegi gonderiliyor...");

    int status = http.POST(requestBody);
    Serial.printf("LLM HTTP durumu: %d\n", status);

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

  while (true) {
    eyeState = EYE_LISTENING;

    if (!followUp) {
      // Parmak cekilene kadar bekle (birakma sesi kaydi bozmasin).
      uint32_t t = millis();
      while (digitalRead(TOUCH_PIN) == HIGH && millis() - t < 3000) {
        delay(10);
      }
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
      break;
    }

    if (vad == VAD_ERROR) {
      Serial.println("HATA: Ses kaydi basarisiz.");
      break;
    }

    eyeState = EYE_THINKING;
    tStart = millis();   // Sureler konusma bittigi andan olculur.

    String transcript = transcribeAudio();
    MARK("STT bitti");

    if (transcript.isEmpty()) {
      Serial.println("Akis STT asamasinda durdu.");
      break;
    }

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

  eyeState = EYE_IDLE;
}

// ---------- Program akisi ----------

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!setupOLED()) {
    Serial.println("OLED testi basarisiz.");
    return;
  }

  startEyeTask();

  Serial.println("\n=== MasaDas ===");

  setupServo();
  setupTouch();

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

  bool pressed = false, longPress = false;
  pollTouch(pressed, longPress);

  if (longPress) {
    runConversation();
  }

  delay(10);
}