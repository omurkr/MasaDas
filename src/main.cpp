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
#include <cstring>
#include <driver/i2s_std.h>
#include <FluxGarage_RoboEyes.h>
#include <ESP32Servo.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

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

// ---------- Mikrofon ----------
static constexpr gpio_num_t I2S_BCLK = GPIO_NUM_14;
static constexpr gpio_num_t I2S_WS   = GPIO_NUM_15;
static constexpr gpio_num_t I2S_DIN  = GPIO_NUM_32;

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint32_t RECORD_SECONDS = 3;
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
  config.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
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

bool recordWav() {
  File file = LittleFS.open("/recording.wav", "w");
  if (!file) {
    Serial.println("HATA: WAV dosyasi acilamadi.");
    return false;
  }

  uint8_t blank[44] = {};
  if (file.write(blank, sizeof(blank)) != sizeof(blank)) {
    file.close();
    return false;
  }

  const uint32_t targetSamples = SAMPLE_RATE * RECORD_SECONDS;
  uint32_t totalSamples = 0;

  Serial.println("\nMikrofona simdi konus. Kayit basladi.");

  while (totalSamples < targetSamples) {
    size_t bytesRead = 0;

    esp_err_t err = i2s_channel_read(
        rxHandle, rawBuffer, sizeof(rawBuffer), &bytesRead, 1000);

    if (err != ESP_OK || bytesRead == 0) {
      Serial.printf("I2S okuma hatasi: %d\n", err);
      file.close();
      return false;
    }

    size_t count = bytesRead / sizeof(int32_t);
    size_t remaining = targetSamples - totalSamples;
    if (count > remaining) count = remaining;

    for (size_t i = 0; i < count; i++) {
      pcmBuffer[i] = static_cast<int16_t>(rawBuffer[i] >> 14);
    }

    size_t bytesToWrite = count * sizeof(int16_t);
    if (file.write(reinterpret_cast<uint8_t *>(pcmBuffer),
                   bytesToWrite) != bytesToWrite) {
      Serial.println("HATA: Ses verisi yazilamadi.");
      file.close();
      return false;
    }

    totalSamples += count;
  }

  uint32_t dataSize = totalSamples * sizeof(int16_t);

  if (!file.seek(0) || !writeWavHeader(file, dataSize)) {
    Serial.println("HATA: WAV basligi yazilamadi.");
    file.close();
    return false;
  }

  file.close();
  Serial.printf("WAV kaydi tamamlandi: %lu bayt\n",
                static_cast<unsigned long>(dataSize + 44));
  return true;
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

    size_t fileSize = wav.size();

    if (fileSize < 44 || fileSize > 200000) {
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

// ------------- Deepseek LLM ------------------

String generateReply(const String &transcript) {
    if (transcript.isEmpty()) {
        Serial.println("HATA: LLM girdisi bos.");
        return "";
    }

    Serial.println("\nOpenRouter DeepSeek LLM streaming baslatiliyor...");

    WiFiClientSecure client;
    client.setInsecure(); // Yalnizca prototip testi icin.

    HTTPClient http;
    http.setTimeout(15000);

    const char *url =
        "https://openrouter.ai/api/v1/chat/completions";

    if (!http.begin(client, url)) {
        Serial.println("HATA: OpenRouter LLM baglantisi baslatilamadi.");
        return "";
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
        "Samimi ama abartisiz bir uslup kullan. Emoji kullanma.";

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
        return "";
    }

    WiFiClient *stream = http.getStreamPtr();

    String reply;
    String line;
    bool streamFinished = false;
    unsigned long lastDataTime = millis();

    Serial.println("LLM parcalari geliyor:");

    while (!streamFinished) {
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
                            http.end();
                            return "";
                        }

                        const char *content =
                            event["choices"][0]["delta"]["content"]
                                | "";

                        if (content[0] != '\0') {
                            reply += content;
                            Serial.print(content);
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
    reply.trim();

    Serial.println("\nLLM akisinin sonu.");

    if (reply.isEmpty()) {
        Serial.println("HATA: LLM yaniti bos veya akis tamamlanamadi.");
        return "";
    }

    return reply;
}

// MAX98357A pinleri
static constexpr gpio_num_t TTS_BCLK = GPIO_NUM_14;
static constexpr gpio_num_t TTS_WS   = GPIO_NUM_15;
static constexpr gpio_num_t TTS_DOUT = GPIO_NUM_23;

static constexpr uint32_t TTS_SAMPLE_RATE = 24000;
static constexpr float TTS_VOLUME = 0.3f;   // 0.0 - 1.0

// 4096 Base64 karakteri -> en fazla 3072 ses bayti.
// Global tamponlar, stack tasmasi riskini azaltir.
static uint8_t ttsPcmBytes[3072];
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

// ---------- TTS akisli oynatma altyapisi ----------

static constexpr size_t TTS_STREAM_BUFFER_SIZE = 32768;
static constexpr size_t TTS_PREBUFFER_SIZE = 4096;

static StreamBufferHandle_t ttsStreamBuffer = nullptr;

static volatile bool ttsStreamDone = false;
static volatile bool ttsPlaybackFinished = false;
static volatile bool ttsPlaybackFailed = false;

static volatile uint32_t ttsEmptyReadCount = 0;
static volatile uint32_t ttsLowBufferCount = 0;
static volatile uint32_t ttsMaxEmptyWaitMs = 0;
static volatile size_t ttsMinBufferBytes = SIZE_MAX;

static volatile size_t ttsPlayedBytes = 0;

static uint8_t ttsPlaybackBuffer[3074];

// ---------- Elle chunked okuma ----------

static uint8_t ttsNetBuffer[1024];

// Veriyi akis tamponuna yazar; tampon doluysa bekler.
static bool pushToTtsBuffer(const uint8_t *data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        if (ttsPlaybackFailed) return false;

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
// Basarida toplam bayt sayisini, hatada -1 dondurur.
static int streamTtsBody(WiFiClient *c, int contentLength) {
    constexpr uint32_t STEP_TIMEOUT_MS = 15000;
    size_t total = 0;

    if (contentLength >= 0) {
        if (!pumpBytes(c, contentLength, STEP_TIMEOUT_MS, total)) {
            Serial.println("[TTS] HATA: govde okunamadi.");
            return -1;
        }
        return static_cast<int>(total);
    }

    char line[24];

    while (true) {
        if (!readHttpLine(c, line, sizeof(line), STEP_TIMEOUT_MS)) {
            Serial.printf("[TTS] HATA: chunk basligi okunamadi (alinan: %u).\n",
                          static_cast<unsigned int>(total));
            return -1;
        }

        size_t chunkSize = strtoul(line, nullptr, 16);

        if (chunkSize == 0) {
            if (line[0] == '\0') continue;   // bos satir, atla
            break;                           // "0" = akisin sonu
        }

        if (!pumpBytes(c, chunkSize, STEP_TIMEOUT_MS, total)) {
            Serial.printf("[TTS] HATA: chunk verisi eksik (alinan: %u).\n",
                          static_cast<unsigned int>(total));
            return -1;
        }

        // Chunk sonundaki CRLF.
        if (!readHttpLine(c, line, sizeof(line), STEP_TIMEOUT_MS)) {
            Serial.println("[TTS] HATA: chunk sonu (CRLF) okunamadi.");
            return -1;
        }
    }

    return static_cast<int>(total);
}

static void ttsPlaybackTask(void *parameter) {
    (void)parameter;

    Serial.println("[TTS-DEBUG] Oynatma gorevi basladi.");

    Serial.printf(
        "[TTS-DEBUG] Baslangic tamponu bekleniyor: %u bayt\n",
        static_cast<unsigned int>(TTS_PREBUFFER_SIZE)
    );

    unsigned long prebufferDebugAt = millis();

    while (
        xStreamBufferBytesAvailable(ttsStreamBuffer)
            < TTS_PREBUFFER_SIZE
        && !ttsStreamDone
        && !ttsPlaybackFailed
    ) {
        if (millis() - prebufferDebugAt >= 1000) {
            Serial.printf(
                "[TTS-DEBUG] Tampon bekleniyor: %u / %u bayt | "
                "Aktarim bitti: %d | Oynatma hatasi: %d\n",
                static_cast<unsigned int>(
                    xStreamBufferBytesAvailable(ttsStreamBuffer)
                ),
                static_cast<unsigned int>(TTS_PREBUFFER_SIZE),
                static_cast<int>(ttsStreamDone),
                static_cast<int>(ttsPlaybackFailed)
            );

            prebufferDebugAt = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (ttsPlaybackFailed) {
        ttsPlaybackFinished = true;
        vTaskDelete(nullptr);
        return;
    }

    Serial.printf(
        "[TTS-DEBUG] Oynatma basliyor. Tampondaki veri: %u bayt\n",
        static_cast<unsigned int>(
            xStreamBufferBytesAvailable(ttsStreamBuffer)
        )
    );

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
            Serial.println(
                "[TTS-DEBUG] HATA: playPcmChunk basarisiz."
            );

            ttsPlaybackFailed = true;
            break;
        }

        ttsPlayedBytes += received;
        Serial.printf("[TTS-PLAY] Calinan toplam: %u bayt\n",
                    static_cast<unsigned int>(ttsPlayedBytes));
    }

    if (hasCarry && !ttsPlaybackFailed) {
        Serial.println(
            "[TTS-DEBUG] HATA: PCM verisinde tek bayt kaldi."
        );

        ttsPlaybackFailed = true;
    }


    Serial.printf(
        "[TTS-METRIK] Bos okuma sayisi: %lu\n",
        static_cast<unsigned long>(ttsEmptyReadCount)
    );

    Serial.printf(
        "[TTS-METRIK] Dusuk tampon sayisi: %lu\n",
        static_cast<unsigned long>(ttsLowBufferCount)
    );

    Serial.printf(
        "[TTS-METRIK] En dusuk tampon: %u bayt\n",
        static_cast<unsigned int>(ttsMinBufferBytes)
    );

    Serial.printf(
        "[TTS-METRIK] En uzun bos bekleme: %lu ms\n",
        static_cast<unsigned long>(ttsMaxEmptyWaitMs)
    );

    Serial.printf(
        "[TTS-DEBUG] Oynatma gorevi bitti. Calinan veri: %u bayt\n",
        static_cast<unsigned int>(ttsPlayedBytes)
    );

    ttsPlaybackFinished = true;
    vTaskDelete(nullptr);
}
 
bool speakText(const String &text) {
    if (text.isEmpty()) {
        Serial.println("HATA: TTS metni bos.");
        return false;
    }

    if (String(OPENROUTER_API_KEY).isEmpty()) {
        Serial.println("HATA: OpenRouter API anahtari bos.");
        return false;
    }

    Serial.println(
        "\nOpenRouter ElevenLabs Flash v2.5 TTS baslatiliyor..."
    );

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(60000);
    http.setReuse(false);

    const char *url =
        "https://openrouter.ai/api/v1/audio/speech";

    if (!http.begin(client, url)) {
        Serial.println("HATA: TTS API baglantisi baslatilamadi.");
        return false;
    }

    http.addHeader(
        "Authorization",
        String("Bearer ") + OPENROUTER_API_KEY
    );
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "audio/pcm");

    JsonDocument request;

    request["model"] = "elevenlabs/eleven-flash-v2.5";
    request["input"] = text;
    request["voice"] = "bella";
    request["response_format"] = "pcm";
    request["provider"]["options"]["elevenlabs"]["language_code"]
        = "tr";

    String requestBody;
    serializeJson(request, requestBody);

    Serial.println("TTS istegi gonderiliyor...");

    int status = http.POST(requestBody);

    Serial.printf("TTS HTTP durumu: %d\n", status);

    if (status < 200 || status >= 300) {
        Serial.println("TTS API hata cevabi:");
        Serial.println(http.getString());

        http.end();
        return false;
    }

    Serial.printf(
        "TTS yanit boyutu: %d bayt (-1 = chunked)\n",
        http.getSize()
    );

    // Hoparloru ve akis tamponunu hazirla.
    if (!setupSpeaker()) {
        http.end();
        return false;
    }

    ttsStreamBuffer = xStreamBufferCreate(
        TTS_STREAM_BUFFER_SIZE,
        1
    );

    if (ttsStreamBuffer == nullptr) {
        Serial.println("HATA: TTS akis tamponu olusturulamadi.");

        i2s_channel_disable(txHandle);
        i2s_del_channel(txHandle);
        txHandle = nullptr;

        http.end();
        return false;
    }

    ttsStreamDone = false;
    ttsPlaybackFinished = false;
    ttsPlaybackFailed = false;
    ttsPlayedBytes = 0;

    // Oynatma gorevini, HTTP verisi gelmeden once baslat.
    BaseType_t taskCreated = xTaskCreate(
        ttsPlaybackTask,
        "TtsPlayback",
        4096,
        nullptr,
        2,
        nullptr
    );

    if (taskCreated != pdPASS) {
        Serial.println("HATA: TTS oynatma gorevi olusturulamadi.");

        vStreamBufferDelete(ttsStreamBuffer);
        ttsStreamBuffer = nullptr;

        i2s_channel_disable(txHandle);
        i2s_del_channel(txHandle);
        txHandle = nullptr;

        http.end();
        return false;
    }

    Serial.println(
        "PCM akis aktarimi basliyor; tampon doldukca ses calinacak."
    );

    Serial.println("[TTS-DEBUG] HTTP akis aktarimi basladi.");

    unsigned long transferStart = millis();

    WiFiClient *netStream = http.getStreamPtr();

    int written = (netStream != nullptr)
        ? streamTtsBody(netStream, http.getSize())
        : -1;

    Serial.printf(
        "[TTS-DEBUG] Aktarim bitti. Sure: %lu ms, bayt: %d, min bos heap: %u\n",
        millis() - transferStart,
        written,
        static_cast<unsigned int>(ESP.getMinFreeHeap())
    );

    // Aktarim bitti; oynatma gorevi kalan veriyi tuketsin.
    ttsStreamDone = true;

    http.end();

    while (!ttsPlaybackFinished) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    size_t playedBytes = ttsPlayedBytes;
    bool playbackFailed = ttsPlaybackFailed;

    vStreamBufferDelete(ttsStreamBuffer);
    ttsStreamBuffer = nullptr;

    if (txHandle != nullptr) {
        i2s_channel_disable(txHandle);
        i2s_del_channel(txHandle);
        txHandle = nullptr;
    }

    if (written <= 0 || playbackFailed || playedBytes == 0) {
        Serial.println("HATA: Akisli TTS tamamlanamadi.");
        return false;
    }

    if (static_cast<size_t>(written) != playedBytes) {
        Serial.printf(
            "UYARI: Aktarilan ve calinan bayt sayisi farkli. "
            "Aktarilan=%d, calinan=%u\n",
            written,
            static_cast<unsigned int>(playedBytes)
        );
        return false;
    }

    Serial.printf(
        "Akisli oynatma tamamlandi: %u bayt\n",
        static_cast<unsigned int>(playedBytes)
    );

    Serial.println("OpenRouter ElevenLabs TTS tamamlandi.");

    return true;
}

// ---------- Program akisi ----------

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!setupOLED()) {
    Serial.println("OLED testi basarisiz.");
    return;
  }

  Serial.println("\n=== MasaDas Ses -> Metin Akisi ===");

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

  Serial.println("Wi-Fi baglandi.");

  if (!setupMicrophone()) {
    Serial.println("HATA: Mikrofon baslatilamadi.");
    return;
  }

  bool recorded = recordWav();

  if (rxHandle != nullptr) {
    i2s_channel_disable(rxHandle);
    i2s_del_channel(rxHandle);
    rxHandle = nullptr;
  }

  if (!recorded) {
    Serial.println("HATA: Ses kaydi basarisiz.");
    return;
  }

  File wavFile = LittleFS.open("/recording.wav", "r");
  if (!wavFile) {
    Serial.println("HATA: Kayit dosyasi bulunamadi.");
    return;
  }

  Serial.printf(
      "Kayit dosyasi boyutu: %u bayt\n",
      static_cast<unsigned int>(wavFile.size())
  );
  wavFile.close();

  Serial.println("\nSTT islemi baslatiliyor...");

  String transcript = transcribeAudio();

  if (transcript.isEmpty()) {
    Serial.println("Akis STT asamasinda durdu.");
    return;
  }

  Serial.println("\nLLM yaniti uretiliyor...");

  String reply = generateReply(transcript);

  if (reply.isEmpty()) {
    Serial.println("LLM asamasi basarisiz.");
    return;
  }

  Serial.println("\n========== MASADAS YANITI ==========");
  Serial.println(reply);
  Serial.println("====================================");

  Serial.println("\nMasaDas yaniti seslendiriliyor...");

  bool spoken = speakText(reply);

  if (!spoken) {
    Serial.println("TTS asamasi basarisiz.");
    return;
  }

  Serial.println("\nSes -> metin -> LLM -> TTS akisi basarili.");
}


void loop() {
    roboEyes.update();

    // Dokunma hareketi ayarlari
    constexpr unsigned long DEBOUNCE_MS = 40;
    constexpr unsigned long SHORT_PRESS_MAX_MS = 500;
    constexpr unsigned long SECOND_PRESS_TIMEOUT_MS = 1200;
    constexpr unsigned long LONG_PRESS_MS = 1500;

    enum TouchState {
        WAIT_FIRST_PRESS,
        WAIT_SECOND_PRESS,
        WAIT_LONG_PRESS,
        WAIT_RELEASE
    };

    static TouchState state = WAIT_FIRST_PRESS;

    static int lastRawState = LOW;
    static int stableState = LOW;

    static unsigned long lastChangeTime = 0;
    static unsigned long firstPressTime = 0;
    static unsigned long secondPressTime = 0;
    static unsigned long firstReleaseTime = 0;

    unsigned long now = millis();
    int rawState = digitalRead(TOUCH_PIN);

    // Sinyal kararliligi
    if (rawState != lastRawState) {
        lastRawState = rawState;
        lastChangeTime = now;
    }

    if (now - lastChangeTime < DEBOUNCE_MS) {
        return;
    }

    if (stableState != rawState) {
        stableState = rawState;

        if (stableState == HIGH) {
            if (state == WAIT_FIRST_PRESS) {
                firstPressTime = now;
            } else if (state == WAIT_SECOND_PRESS) {
                secondPressTime = now;
                state = WAIT_LONG_PRESS;
                Serial.println("Ikinci dokunma algilandi; basili tut.");
            }
        } else {
            if (state == WAIT_FIRST_PRESS) {
                unsigned long duration = now - firstPressTime;

                if (duration <= SHORT_PRESS_MAX_MS) {
                    firstReleaseTime = now;
                    state = WAIT_SECOND_PRESS;
                    Serial.println("Ilk kisa dokunma tamamlandi.");
                    Serial.println("1.2 saniye icinde tekrar dokun ve basili tut.");
                }
            } else if (state == WAIT_LONG_PRESS) {
                state = WAIT_FIRST_PRESS;
                Serial.println("Uzun basma tamamlanmadan birakildi; iptal.");
            } else if (state == WAIT_RELEASE) {
                state = WAIT_FIRST_PRESS;
            }
        }
    }

    // Ilk dokunmadan sonra ikinci dokunma icin zaman asimi
    if (state == WAIT_SECOND_PRESS &&
        now - firstReleaseTime > SECOND_PRESS_TIMEOUT_MS) {
        state = WAIT_FIRST_PRESS;
        Serial.println("Dokunma zaman asimi; yeniden bekleniyor.");
    }

    // Ikinci dokunma 1.5 saniye basili tutulursa tetikle
    if (state == WAIT_LONG_PRESS &&
        stableState == HIGH &&
        now - secondPressTime >= LONG_PRESS_MS) {
        Serial.println("BAS-KONUS TETIKLENDI");
        state = WAIT_RELEASE;
    }
}
