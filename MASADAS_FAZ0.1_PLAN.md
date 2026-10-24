# MasaDaş Faz 0.1 — Devir Planı (Claude Code için)

Hazırlanma tarihi: 2026-10-24. Kaynak: "Dokunma olayları düzenlemesi" sohbeti (92 tur), proje hafızası, ekli `src/main.cpp` ve `platformio.ini`.

## 0. Bu belgenin güven düzeyi

Her madde şu etiketlerden biriyle işaretlidir:

- **[UYG+DOĞ]** Uygulandı ve kullanıcı cihazda test edip olumlu sonuç bildirdi.
- **[UYG?]** Uygulandı (kod gönderildi veya kullanıcı yaptığını söyledi), ama olumlu sonuç bildirilmedi.
- **[ÖNERİ]** Önerildi veya kod olarak verildi, uygulandığına dair kanıt yok.
- **[YOK]** Hiç uygulanmadı.
- **[BİLİNMİYOR]** Sohbetten çıkarılamadı.

Sınırlar:
- Sohbetin ilk 13 turunu ve sonlardaki özet/envanter mesajlarını tam okudum. Ortadaki turların (TTS takılma ayıklaması, servo testleri) bir kısmını yalnızca alıntılardan gördüm. Oradaki ayrıntılar eksik olabilir.
- Hiçbir şey cihazda denenmedi. "Doğrulandı" yalnızca kullanıcının bildirdiği test sonucu demektir. Claude'un "kodu okuyarak kontrol ettim" dediği yerler doğrulama sayılmaz.

## 1. İlk adım: depo durumunu doğrula (kod yazmadan önce)

Ekli `main.cpp`, Grup A'yı içermiyor gibi görünüyor: `bargePressSeen` hâlâ var, `conversationCancelled`/`VAD_CANCELLED`/`convTapSeen` yok, `TOUCH_WARMUP_MS` ve `INPUT_PULLDOWN` yok, öfke eşiği 4 dokunuş / 4 sn, `setup()` içinde `loopLongSeen` sıfırlaması yok, VAD 800 ms / 8 sn. Yani ekli dosya "geçen haftaki stabil taban" gibi duruyor.

Ancak sohbetin sonunda kullanıcı "stabil taban + Grup A + touch önceliği düzeltmesi" sürümünü göndermiş ve o sürüm "doğru görünüyor" diye okunmuştu. Bu sürümün şu an hangi dosyada/commit'te olduğu **[BİLİNMİYOR]**.

Yapılacak: `git log`, `git status` ve varsa yedek dosyalara bakıp hangi sürümün güncel olduğunu kullanıcıyla netleştir. Grup A gerçekten yoksa Bölüm 4A'daki kayıtlara göre yeniden eklenir.

## 2. Çalışma kuralları (sohbette öğrenilenler)

1. **Bir seferde tek grup.** Her gruptan sonra masal testi (aşağıda) yapılmadan sonrakine geçme.
2. **Faz 0.1'de TTS koduna dokunulmaz. TTS'te gerçek bir hata bulunursa kod değiştirmeden önce bana sor.**
3. **Yeni görev açarken öncelik 1'i geçme.** Oynatma görevi 2'de kalır.
4. **Touch görevinde `Serial` yazımı yok.** `[TOUCH]` logları yorumda kalır.
5. **Dosyada var olmayan kodu "mevcut" sanma.** Kullanıcı bunu birkaç kez düzeltti. Önce dosyayı oku, sonra "şu satırı bul, şununla değiştir" biçiminde kısa ve numaralı ilerle.
6. **Kullanıcı kafa karıştıran, dağınık anlatımı sevmiyor.** Kısa, sade, tek aşamalı anlat. Gereksiz token harcama.
7. **Dosya kodlaması UTF-8 kalmalı.** Bir kez kayıt sırasında `MasaDaş` ve yorumlardaki Türkçe karakterler bozulmuştu. Prompt metninde `ş` doğru görünmeli.
8. **Cihazda test edemediğini söyle.** "Çalışıyor" ancak kullanıcı bildirirse yazılır.

## 3. Mimari özeti (ekli `main.cpp`'ye göre, **[kodda görülen]**)

**Donanım:** esp32dev; SH1106 OLED 128x64 (I2C, SDA 21, SCL 22, 400 kHz); servo (GPIO 19, 1000–2000 µs); kapasitif touch (GPIO 4); I2S mikrofon (BCLK 14, WS 15, DIN 32, 16 kHz); MAX98357A hoparlör (BCLK 14, WS 15, DOUT 23, 24 kHz). Mikrofon ve hoparlör aynı BCLK/WS pinlerini ve I2S_NUM_0'ı paylaşır, bu yüzden kanallar sırayla açılıp kapanır.

**Yazılım:** PlatformIO, Arduino (pioarduino platformu 55.03.39), LittleFS; kütüphaneler Adafruit GFX, Adafruit SH110x, ESP32Servo, FluxGarage RoboEyes, ArduinoJson.

**Servisler (hepsi OpenRouter):** STT `deepgram/nova-3`; LLM `deepseek/deepseek-v4.1-flash` (akışlı, reasoning kapalı); TTS `elevenlabs/eleven-flash-v2.5`, ses `bella`, PCM, dil `tr`. TLS `setInsecure()` (prototip).

**Akış:** uzun basma → bip → VAD kayıt (`/recording.wav`, LittleFS) → STT (WAV base64 JSON akışı) → LLM akışı → cümlelere ayırma (`sentenceBacklog`) → LLM bittikten sonra TTS hattı başlar → işçi görev cümleleri sırayla TTS'e gönderir → akış tamponu → oynatma görevi → I2S.

**Not:** LLM→TTS canlı (LLM bitmeden ilk cümleyi seslendirme) akışı yok; bu bilinçli bırakılmıştı (heap). Cümleler önce birikir, LLM bitince seslendirilir.

**Görevler ve öncelikler (stabil değerler, korunacak):**

| Görev | Öncelik | Yığın |
|---|---|---|
| Touch | 1 | 3072 |
| Behavior (OLED + servo + davranış) | 1 | 6144 |
| TtsPlayback | 2 | 4096 |
| TtsWorker (TLS) | 1 | 12288 |
| `loop()` / `runConversation()` | Arduino varsayılanı | varsayılan |

**Bileşenler arası ilişkiler (önemli):**
- Durum paylaşımı: `appPhase`, `ttsLevel`, `pendingSound`, `lastInteractionMs` `volatile`. Davranış motoru `appPhase`'e bakıp gözleri/servoyu yönetir.
- Dokunma olayları sayaçtır (`touchPressSeq`, `touchTapSeq`, `touchLongSeq`). Her tüketici kendi "son gördüğüm" değerini tutar (`takeEvent`). Davranış motoru `seenPress/seenTap`, konuşma akışı `loopLongSeen` (ve Grup A'da `convTapSeen`) kullanır. Tüketiciler birbirinin olayını silmez.
- Söz kesme: `checkBargeIn` → `speechAbort` + `ttsPlaybackFailed`; oynatma, işçi ve `pushToTtsBuffer` bu bayraklara bakıp durur. `finishSpeechPipeline` iki görevin bitmesini bekler ve kaynakları serbest bırakır.
- VAD ↔ STT: kayıt dosyası boyutu `recordedWavBytes` ile izlenir; STT boyut sınırı (stabilde 300000 bayt ≈ 9 sn) VAD azami konuşma süresiyle (stabilde 8 sn) birlikte düşünülmeli. Biri yükselirse diğeri de yükselmeli.
- LLM → TTS: LLM cümleleri `flushSentences` ile ayrılır (min 12, maks 180 karakter), TTS sırası 16.
- Hoparlör ve mikrofon aynı I2S ve pinleri kullandığı için aynı anda açık olmamalı; `runConversation` her turda mikrofonu açıp kapatır, TTS hattı hoparlörü açar.
- TTS akış tamponu 32768 bayt (24 kHz 16-bit mono ≈ 0,68 sn). Ön tampon 4096.

## 4. Gruplar (A–G)

### Faz 0.1 maddeleriyle eşleme (**benim çıkarımım**, sohbette böyle yazılmadı)
- Madde 1.x (dokunma) → Grup A
- 2.1 STT → Grup B
- 2.2 LLM → Grup C (2.2.4 canlı akış: **KAPSAM DIŞI**)
- 2.3/2.4 TTS ve cümle akışı → stabil taban. 2.3.1–2.3.5: **Doğrulama maddesi: kod değiştirilmez, yalnızca test edilir; sorun çıkarsa bana sorulur.** 2.4.2–2.4.3 canlı akış: **KAPSAM DIŞI**; 2.4.6 ölçüm için `MARK` logları mevcut
- 3.x durumlar ve hata → Grup D
- Bip sesi düzeltmesi → Grup E (kullanıcı isteği, Faz listesinde ayrı madde yok)

### Grup A — Dokunma (TTS'e dolaylı dokunur: `checkBargeIn`)
**Amaç:** doğru dokunma tepkileri, dinleme/konuşma sırasında iptal, açılışta kendi kendine dinlemeye geçmeyi önleme.

**Kararlaştırılan teknik değişiklikler:**
1. `TOUCH_MIN_GAP_MS = 30` ve `pressValid` ile sıçrama koruması (ilk öneri 120 ms idi, hızlı dokunuşları yutuyordu, 30'a indirildi).
2. `TOUCH_WARMUP_MS = 2000`: görev başında olaylar yok sayılır.
3. `pinMode(TOUCH_PIN, INPUT_PULLDOWN)`.
4. `conversationCancelled()` + `convTapSeen` (`takeEvent(touchTapSeq, ...)` tabanlı ayrı tüketici).
5. `VadResult`'a `VAD_CANCELLED`; `recordUtterance` döngü başında iptal kontrolü; `runConversation` içinde `VAD_CANCELLED` ve STT sonrası iptal kontrolü; bip sonrası `convTapSeen = touchTapSeq`.
6. `checkBargeIn` basış yerine kısa dokunmaya (`conversationCancelled`) bakar; `bargePressSeen` silinir (`respondToTranscript` içindeki sıfırlama dahil).
7. `behaviorStep`: konuşma fazından çıkınca `seenTap/seenPress` eşitlenir ve `ignoreTapUntil = now + 300` (iptal dokunuşu "mutlu" tepki vermesin).
8. Öfke: `ANGRY_TAP_COUNT = 3`, `ANGRY_TAP_WINDOW_MS = 2000` (kullanıcı "4 dokunuş algılanmıyor, asıl amaç rahatsız edilince kızması" demişti).
9. `setup()` sonunda `systemReady = true` öncesi `loopLongSeen = touchLongSeq`.
10. Touch görevi **öncelik 1, yığın 3072**, `[TOUCH]` logları yorumda (Bölüm 5'e bakın).

**Durum:**
- Madde 1–9'un işlevsel hâli: ilk uygulamada kullanıcı "dokunma fonksiyonları temel olarak gayet iyi" dedi [UYG+DOĞ, 120→30 ms ve 3/2 sn öncesi sürümle]. 30 ms ve 3/2 sn ayarlarını ayrıca test ettiğine dair açık ifade yok [UYG?].
- Öncelik/yığın/log düzeltmesi ve diğer Grup A ayarları birlikte: kullanıcı masalı defalarca denedi, takılma/tekrar yok [UYG+DOĞ].
- `INPUT_PULLDOWN`, 2 sn ısınma, iptal mantığı: bu testte kodda duruyordu ve ses bozulmadı, yani TTS'i bozmadığı doğrulandı. Dokunma davranış testinin 5 maddesini (aşağıda) ayrıca bildirdiğine dair kayıt yok [UYG?].
- Reset sonrası kendi kendine dinlemeye geçmenin bitip bitmediği [BİLİNMİYOR].

**Önerilen ama uygulanmayan (A'ya ait):** touch titreşim kilidi (`TOUCH_FLUTTER...`), sensör HIGH'da takılırsa koruma [YOK].

### Grup B — VAD / STT (TTS çalarken çalışmaz, düşük risk)
**Amaç:** konuşma sınırı olmasın, düşünme/nefes payı olsun, yanlış algılama azalsın, STT hataları kontrollü yönetilsin.

**Kararlaştırılan değişiklikler (hepsi [ÖNERİ]/[UYG?], bu sohbette doğrulanmadı):**
- `VAD_END_SILENCE_MS` 800 → 1800 (ayar rehberi: çok bekliyorsa 1500, araya giriyorsa 2200).
- `VAD_MAX_SPEECH_MS` 8000 → 30000; STT dosya sınırı 300000 → 1000000; STT zaman aşımı 60 → 30 sn. 30 sn ≈ 960 KB, LittleFS alanı yetmezse "Ses verisi yazilamadi" çıkar.
- `VAD_PREROLL_BLOCKS` 8 → 16, `VAD_TAIL_KEEP_BLOCKS` 8 → 12 (kelime başı/sonu kesilmesin).
- Mikrofon girişine DC kayması filtresi (tek kutuplu yüksek geçiren, katsayı 0,995).
- Mikrofon DMA tamponu: `dma_desc_num = 8`, `dma_frame_num = 512`.
- VAD bitiş nedeni logu (sessizlik / azami süre).
- STT: `sttRetryable` (HTTP < 0, 429, ≥ 500 için bir kez yeniden deneme), `hasLetters` ile anlamsız metin kontrolü, üst üste 3 anlaşılmayan konuşmada (`emptyCount > 2`) veda edip boşa dönme (`endedBySilence`).

**Durum:** Kullanıcı bu değişiklikleri (ilk sürümlerini) uyguladığını söyledi ve 4 sorun bildirdi (konuşma sınırı, erken kesme, yanlış algılama, kendi kendine dinleme); bu sorunlara cevaben 1800 ms, 30 sn, DC filtresi vb. verildi. Çözümün sonucu bildirilmedi [UYG?]. Stabil taba dönüldüğü için **şu an kodda yok** [YOK].

### Grup C — LLM
**Amaç:** sesli kullanıma uygun kısa yanıt, hata/zaman aşımı yönetimi, yanlış STT'ye karşı dayanıklılık.

**Kararlaştırılan değişiklikler:**
- Yeni sistem promptu: sesli okunacak, kısa cümle, markdown/emoji/bağlantı yok, sayılar yazıyla, "STT hatalı olabilir, anlamazsan tekrar iste", masal/hikâye için kural. (Tam metin sohbette birkaç kez değişti; son hâli [BİLİNMİYOR].)
- `max_tokens` 400 → 250 (ilk öneri 150 idi; envanterde 250 yazıyor), `http.setTimeout` 10 sn, toplam süre sınırı 25 sn, boş yanıtta tek yeniden deneme.
- Sohbet hafızası (son 3 tur, `historyAdd/historyClear`): kod olarak önerildi ama envanterde "hiç uygulanmadı" yazıyor [ÖNERİ/YOK]. Şu an LLM'e önceki konuşma gitmiyor, takip sorusu ("peki yarın?") anlaşılmaz.

**Durum:** Prompt/limit/yeniden deneme önceki çalışmada uygulanmıştı [UYG?]; stabil tabanda **yok** [YOK].

### Grup D — Durumlar ve hata davranışı (TTS'e dokunmaz)
**Amaç:** madde 3.1–3.7.

**Önceki (geri alınan) çalışmada yapılanlar [UYG?]:** `PHASE_CANCELLED`, `PHASE_ERROR`, `B_ERROR`, `SND_ERROR`, `ERROR_MS` (üzgün göz + hata sesi); `runConversation` içinde `hadError`/`cancelled` bayrakları, tur başında önceki veriyi temizleme, sonda mikrofon/hoparlörü kapatma; `ensureWiFi` yükleme animasyonuyla; `DEBUG_FAKE_NO_WIFI` test bayrağı; `loop()` içinde 15 sn'de bir yeniden bağlanma ve `setAutoReconnect`; açılışta Wi-Fi gelmezse kilitlenmeme, `LittleFS.begin(true)`; I2S çakışma koruması (`playSound` kanal meşgulse çalmaz, `setupMicrophone`/`setupSpeaker` eski kanalı temizler).

**Durum:** Stabil tabanda **yok**. Ekli kodda `setup()` içindeki erken `return`'ler (OLED/secrets/LittleFS/Wi-Fi) hâlâ sistemi `systemReady=false` ile kilitliyor [kodda görülen].

### Grup E — Ses efektleri (hoparlöre dokunur, TTS akışına değil)
**Amaç:** bip seslerinin kesik gelmesini düzeltmek.

**Karar:** `playToneSweep`/`playSound` yerine sesi önce RAM'de üret (`addSweep`, `addSilence`, başta/sonda 40 ms sessizlik), sonra I2S'e tek akışta yaz. Ara belleği yaklaşık 38 KB'lık (800 ms × 24 kHz × 2 bayt) `malloc` ile ayırıyor. **[Benim gözlemim]** Bu, heap tüketimini artırır; TTS öncesi heap logu ( `[HEAP]` ) ile birlikte izlenmeli.

**Durum:** Kullanıcı, bipin kesik geldiğini bildirmişti; düzeltmenin sonucu bildirilmedi [UYG?]. Stabil tabanda **yok** [YOK]. Bip hâlâ kesik gelirse sorunun güç kaynaklı olabileceği not edilmişti.

### Grup F — Hız için yapılanlar (TTS'e dokunur, **şüpheli**) — **KAPSAM DIŞI (iptal, 2026-10-24)**
- Ortak TLS bağlantısı: global `netClient`/`netHttp`, `warmTask`/`startWarm`/`waitWarm` (kayıt sırasında bağlantıyı önceden açma). Envanterde ilk sesin ~9,4 sn → ~6,2 sn'ye indiği yazıyor (ölçüm kaynağı sohbette görünmüyor).
- LLM akış kapanışını tüketme (`0\r\n\r\n`), hata/iptalde `netClient.stop()`.
- **Durum:** Geri alındı, stabilde yok. Takılmanın kaynağı olabileceği düşünülüyor ama **kanıtlanmadı**.

### Grup G — TTS yamaları (**hepsi şüpheli**, takılma bunlardan sonra başladı) — **KAPSAM DIŞI (iptal, 2026-10-24)**
- Ölçüm logları (`Veri bekleme`, cümle ms, RSSI, "Sunucu baglantiyi kapatti").
- Zaman aşımları 15/30 sn → 5/8 sn (bir ara 3 sn).
- Ön tampon 4096 → 28000, `TTS_REBUFFER_SIZE`, tampon boşalınca sessizlik besleme.
- Yeniden deneme mantığı değişikliği.
- Cümleleri tek istekte birleştirme: kötüleştirdi.
- Flash tabanlı çalma (`/s0.pcm`...): sonuç iyi çıkmadı.
- **Durum:** Geri alındı. Başlangıçtaki "yapmayacağız" listesi: ortak `netClient`/`warmTask`, kısa zaman aşımları, 28000 ön tampon, sessizlik besleme.

Not: Önce F/G "hiç eklenmeyecek" denmişti; daha sonra "en sona kalsın, takılma geri gelirse suçlu bunlardan biridir" olarak yumuşatıldı. Güncel karar kullanıcıyla netleştirilmeli [BİLİNMİYOR].

## 5. Stabil sürüme dönme kararı

**Gerekçe (sohbetten):**
- Bir gün boyunca eklenen değişikliklerin üzerinde TTS'te ilk 2–3. kelimede takılma/tekrar ve cümleler arası saniyelerce boşluk başladı. Paylaşılan TLS, zaman aşımı, ön tampon, flash tamponu, cümle birleştirme gibi birçok yama sorunu çözmedi, bazıları kötüleştirdi.
- Kullanıcı geçen haftaki kodu geri yükledi: takılma, tekrar, boşluk yok.
- Karar: stabil taban + değişiklikleri **gruplar halinde, TTS'e dokunmayanlardan başlayarak** yeniden eklemek.

**Ayrıştırma sonucu (en kıymetli bilgi):**
- Grup A eklenince takılma geri geldi. Touch görevinin önceliği 3 / yığın 4096 ve `[TOUCH]` `Serial.printf` logları geri alındı (öncelik 1, yığın 3072, loglar yorum) → takılma tamamen kayboldu [UYG+DOĞ].
- İkisi birlikte değiştiği için hangisinin tek başına suçlu olduğu **bilinmiyor**. Önerilen mekanizma (yüksek öncelikli görev oynatma görevini I2S yazımı sırasında geciktiriyor) **ölçülmedi**, yorumdur.
- Kalıcı kural: yeni görev/log eklerken oynatma ve I2S zamanlamasını bozma.

**Korunacak (stabil tabandan):** `TTS_PREBUFFER_SIZE = 4096`, `STEP_TIMEOUT_MS = 15000`, TTS `setTimeout(30000)`, `setReuse(true)`, cümle cümle istek, `TTS_STREAM_BUFFER_SIZE = 32768`, `TTS_QUEUE_LENGTH = 16`, oynatma/işçi/touch/behavior öncelik ve yığınları (Bölüm 3), işçi içinde yerel `WiFiClientSecure`/`HTTPClient`, LLM'in bitip bağlantısının kapandıktan sonra TTS'in başlaması.

**Korunacak (Grup A'dan, TTS'i bozmadığı doğrulandı):** `INPUT_PULLDOWN`, 2 sn ısınma, 30 ms sıçrama koruması, kısa dokunmayla iptal, 3 dokunuş / 2 sn öfke, `loopLongSeen` sıfırlama.

## 6. Bilinen hatalar ve çözülmemiş sorunlar

1. **Açılışta/reset sonrası kendi kendine dinleme, touch'ın takılı kalması.** Kullanıcı bunun "kesinlikle touch sensörüyle ilgili" olduğunu söyledi. Olası kaynaklar: açılışta biriken uzun basma olayı (yazılım, `loopLongSeen` ile giderilmesi önerildi), besleme gürültüsü (donanım, **doğrulanmadı**). Çözümün sonucu bildirilmedi.
2. **Kendi kendine konuşma ("geldim işte burdayım", "noldu bir şey mi dedin").** Kodda yazılı cümle yok; LLM üretiyor. Büyük olasılıkla sahte uzun basma + mikrofon gürültüsü → STT'den kısa metin → LLM. Önlem önerildi: kayıt tepe seviyesi düşükse STT'yi atla [YOK]. Sabit açılış/veda/hata cümleleri önerildi [YOK].
3. **Servo hırlama/sürtünme sesi ve servo hareketinde OLED kararması.** Güç hattı şüphesi (USB, breadboard, ortak hat). 5 V adaptörle denemede servo hiç çalışmadı (neden belirlenemedi). Sadece servo içeren yumuşatılmış test kodunda servo düzgün çalıştı; kullanıcı sesin normal servo sesi olabileceğini söyledi. Ana projedeki kök neden **belirlenmedi**. Önerilenler [YOK]: `headUpdate` içinde PWM yazımını 20 ms'de bir sınırlama, `SERVO_DETACH_WHEN_IDLE = true`, öfke sallanmasının 14° / 6 Hz yapılması, `HEAD_TURN_DEG`'i 35'e çıkarma, servo için ayrı 5 V / 2 A besleme ve 470–1000 µF kondansatör. Test sketch'ındaki değerlerin ana projeye taşınması beklemede.
4. **OpenRouter 402 hatası** (ses isteklerinde anahtar bakiyesi en az 0,50 $ olmalı). Çözüldüğü açıkça bildirilmedi ama sonraki testler yapıldığı için büyük olasılıkla çözüldü [BİLİNMİYOR].
5. **Yükleme hatası `Failed to calculate md5sum`.** Öneriler verildi (monitörü kapat, 115200 yükleme hızı, kablo/port, BOOT tuşu, besleme). Sonuç [BİLİNMİYOR].
6. **Canlı LLM→TTS akışı yok** (2.2.4, 2.4.2–2.4.3: **KAPSAM DIŞI**). Cümleler LLM bitince seslendiriliyor. Heap/zaman ölçümü ile ele alınacaktı [YOK].
7. **Gecikme ölçümü (2.4.6):** `MARK` logları var (`STT istek`, `LLM ilk parça`, `ilk cümle`, `ilk ses`, `toplam`). Stabil tabanın güncel gecikme değerleri [BİLİNMİYOR].
8. **Sohbet hafızası yok.** Takip sorularında bağlam kaybolur.
9. **`setup()` kilitlenmesi.** Wi-Fi/LittleFS/OLED başarısız olursa robot sessizce `systemReady=false` kalır (Grup D'de çözülecekti).
10. **`appPhase = PHASE_SPEAKING` taşıma ("Düzeltme A"):** TTS ayıklaması sırasında önerilmişti, durumu [BİLİNMİYOR]; stabil tabanda `appPhase = PHASE_SPEAKING` oynatma görevinde ilk ses geldiğinde atanıyor.
11. **Kodda gördüğüm küçük tutarsızlık (sohbette konuşulmadı):** `VAD_MIN_SPEECH_BLOCKS = 16` yorumda "~160 ms" yazıyor, ama blok süresi 16 ms olduğundan 16 blok ≈ 256 ms. Davranışı etkileyen bir hata değil, yalnızca yorum yanlış. Grup B'de VAD ayarlarına dokunulurken kontrol edilebilir.

## 7. Her grup sonunda yapılacak testler

**Tüm gruplarda zorunlu (TTS regresyonu):** Masal iste (uzun yanıt), 3–4 kez tekrarla. İlk cümlede kelime tekrarı, takılma veya cümleler arası saniyelerce boşluk olmamalı. Sonra reset at; robot kendi kendine dinlemeye geçmemeli.

**Grup A:**
1. Boşta tek kısa dokunma → mutlu tepki, bir kez.
2. Boşta 3 hızlı dokunma (2 sn içinde) → öfke; sonra tekrar tetiklenmemeli.
3. Uzun basma → bip → dinleme; parmağı çekince "mutlu" tepki gelmemeli.
4. Dinlerken kısa dokunma → iptal, boşa dönüş, mutlu tepki yok.
5. STT/LLM beklerken dokunma → iptal.
6. Konuşurken kısa dokunma → ses durur, dinlemeye geçer.
7. Uyku/uyuklamada dokunma → uyanır, ardından mutlu tepki yok.
8. Reset sonrası kendi kendine dinlemeye geçmemeli; `[TOUCH]` logları kapalı olmalı.
9. Touch görevi önceliği 1'de dokunma tepkisi yavaşlamış mı, kontrol et.

**Grup B:**
1. Normal cümle konuş → log'da "Bitis: sessizlik".
2. Konuşmanın ortasında nefes/düşünme duraklaması (yaklaşık 1,5 sn) → kesilmemeli.
3. 8 sn'den uzun konuş → kesilmemeli; "Ses verisi yazilamadi" çıkmamalı.
4. Sessiz ortamda mırıldan/öksür → boş STT, en fazla 2 kez dinlemeye döner, 3. seferde vedalaşıp boşa geçer.
5. Wi-Fi kapalıyken STT dene → yaklaşık 20–30 sn içinde kontrollü dönüş; göz animasyonu ve dokunma çalışmaya devam eder.
6. `[VAD] gurultu=… esik=…` ve `tepe seviye=…` loglarını kaydet; yanlış algılamada robotun anladığı ile gerçek cümleyi karşılaştır.

**Grup C:** Yanıtlar kısa; yıldız/madde işareti okunmamalı; sayılar yazıyla; anlamsız metinde "tekrar eder misin" benzeri; LLM hatasında tek yeniden deneme logda görünmeli; 25 sn toplam süre aşılırsa kontrollü çıkış.

**Grup D:** `DEBUG_FAKE_NO_WIFI` ile Wi-Fi yokken açılış; Wi-Fi koparken konuşma isteği; API hatasında üzgün göz + hata sesi; hata sonrası robotun boşta durumuna dönmesi; ardışık iki konuşmada eski verinin kalmaması; aynı anda iki kayıt/çalma çakışması olmaması.

**Grup E:** Dört bip (dinleme, mutlu, öfke, esneme) kesintisiz; servo hareket ederken de; `[HEAP]` loglarında TTS öncesi boş heap ve en büyük blok değeri bipler eklenmeden öncekine göre belirgin düşmemiş olmalı.

**Grup F/G (yalnızca onaylanırsa):** Önce tek tek ve izole; her adımdan sonra masal testi; takılma geri gelirse o adım geri alınır.

## 8. Uygulama sırası ve sonraki adım

Sohbette kararlaştırılan sıra:
1. **A** (dokunma), sonra **B** (VAD/STT)
2. **C** (LLM)
3. **D** (durumlar/hata) ve **E** (bip)
4. **F** ve **G** en sona; yalnızca gerekirse ve tek tek.

Mevcut nokta: Grup A uygulanmış ve TTS'i bozmadığı doğrulanmış bir sürüm vardı; Grup B'ye geçilmek üzereydi (kullanıcı önce Grup A'nın 5 maddelik dokunma testini yapıp sonucu yazacaktı).

**Bir sonraki adım:**
1. Bölüm 1'deki depo/sürüm kontrolünü yap ve kullanıcıya hangi dosyanın güncel olduğunu teyit ettir.
2. Grup A'nın kodda olduğundan emin ol (yoksa Bölüm 4A madde 1–10'a göre ekle, TTS'e dokunma).
3. Grup A dokunma testini (9 madde) ve masal testini yaptır; sonuçlara göre Grup B'ye geç.
4. Açık kalan kararlar: sohbet hafızası eklensin mi, servo/güç çözümü ne zaman ele alınacak, sabit açılış/veda cümleleri, F/G'nin akıbeti.

## 9. Durum günlüğü

- 2026-10-24 · Grup B Adım 1: `VAD_END_SILENCE_MS` 800 → 1800 yapıldı (tek satır); `pio run` derlemesi geçti (Flash %94,2); kullanıcı cihazda testi başarılı bildirdi [UYG+DOĞ].