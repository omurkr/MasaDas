# MasaDaş Faz 0.1 — Madde durum tablosu

Tarih: 2026-10-24. Kaynak: src/main.cpp (satır numaraları güncel dosyaya göre). Durum: Var / Kısmen / Yok / KAPSAM DIŞI.
Bu tablo koddan okunarak çıkarıldı; cihazda doğrulanmış sonuç anlamına gelmez.
2.1.x, 2.2.x, 2.3.x ve 2.4.x maddeleri için kural: kod değişmez, yalnızca mevcut davranış incelenir.

## 1. Dokunma

| Madde | Durum | Satır | Açıklama |
|---|---|---|---|
| 1.1 Ham giriş, basma/bırakma | Var | 191–241 | `touchTask` GPIO4'ü 5 ms'de okur; ham/stabil ayrımı var, `[TOUCH]` logları bilerek yorumda (218, 236). |
| 1.2 Kısa / uzun / hızlı art arda | Var | 177–179, 228, 235, 433–439 | Üç ayrı sayaç ve `tapTimes` ile hızlı art arda dokunma ayrışıyor. |
| 1.3 Sıçrama koruması | Var | 171, 173, 216, 222 | 40 ms debounce ve 30 ms asgari aralık (`pressValid`). |
| 1.4 Boşta tek dokunma → mutlu | Var | 416–447 | `handleTouchIdle` → `enterMode(B_LOVE)` (445). |
| 1.5 Boşta uzun basma → sesli iletişim | Var | 235, 2511–2512 | `touchLongSeq` `loop()` içinde `runConversation()` başlatır. |
| 1.6 Hızlı art arda dokunma → öfke | Var (3 dokunuş) | 261–262, 441–443 | Madde metni dört der; kod 3 dokunuş / 2 sn (plan kararı). |
| 1.7 Konuşma sırasında dokunma izlenir | Var | 191, 596–601 | Touch görevi bağımsız; davranış motoru konuşmada olayları tüketir. |
| 1.8 Dinlerken kısa dokunma → iptal | Var | 923, 2404 | `VAD_CANCELLED`; kalibrasyon bloklarında (≈380 ms) bekler, ilk konuşma bloğunda yakalanır. |
| 1.9 Konuşurken kısa dokunma → durdur | Var | 1615–1621, 2072, 2080 | `checkBargeIn` → `speechAbort` + `ttsPlaybackFailed`. |
| 1.10 Uykuda dokunma → uyan | Var | 422–427 | `B_SLEEP/B_DROWSY` + basış → `B_WAKING`. |
| 1.x Farklı durumlarda test | Kısmen | — | Kodda test yok (test/ boş); Grup A testi kullanıcı tarafından cihazda yapıldı. |

## 2. STT – LLM – TTS (kod değişmez, yalnızca mevcut davranış incelenir)

| Madde | Durum | Satır | Açıklama |
|---|---|---|---|
| 2.1.1 Kayıt/VAD incelemesi | Var | 844–1027 | `recordUtterance`: kalibrasyon, halka tampon, eşik, WAV başlığı. |
| 2.1.2 Başlangıç/bitiş algısı | Var | 926–958, 977 | 2 ardışık blok eşiği aşarsa başlar; sessizlik veya azami süre ile biter. |
| 2.1.3 Sessizlik süresi | Var | 782 | `VAD_END_SILENCE_MS = 1800` (Adım 1). |
| 2.1.4 Boş/anlaşılmaz STT | Kısmen | 1341–1345, 2419–2424 | Boş metin 2 kez tolere edilir; anlamsız metin (`hasLetters`) kontrolü yok. |
| 2.1.5 STT başarısızlığında takılmama | Kısmen | 1253, 1319–1323, 2426 | Zaman aşımı 60 sn ve hata sonrası akış sonlanır; yeniden deneme yok. |
| 2.2.1 Metnin LLM'e doğru gitmesi | Var | 2142–2144 | Kullanıcı mesajı olduğu gibi gönderilir; sohbet geçmişi yok. |
| 2.2.2 Kısa, seslendirmeye uygun yanıt | Kısmen | 2134–2146 | Sistem istemi 1–2 cümle der; `max_tokens` 400. |
| 2.2.3 Boş yanıt/API hatası/zaman aşımı | Kısmen | 2161–2166, 2208–2216, 2246–2249, 2260 | Hata ve 15 sn veri zaman aşımı var; yeniden deneme ve toplam süre sınırı yok. |
| 2.2.4 Canlı akış | KAPSAM DIŞI | — | Karar: kapsam dışı. |
| 2.3.1 PCM alım/oynatma | Var | 1715–1758, 1764–1836, 1460–1513 | Chunked okuma → akış tamponu → I2S; doğrulama maddesi: kod değiştirilmez, yalnızca test edilir; sorun çıkarsa bana sorulur. |
| 2.3.2 Bozuk/kesik/sessizlik nedenleri | Kısmen | 1375, 1445–1457 | `auto_clear` ve yumuşak sınırlayıcı var; doğrulama maddesi (kod değişmez). |
| 2.3.3 Tampon doluluk/boşalma | Var | 1590–1611, 1628–1643 | 32 KB akış tamponu, 100 ms'lik bloklu yazma; doğrulama maddesi. |
| 2.3.4 TTS hatasında kilitlenmeme | Kısmen | 1632, 1651, 1716, 2071–2082 | Ağ adımlarında zaman aşımı var, `finishSpeechPipeline` beklemelerinde genel sınır yok; doğrulama maddesi. |
| 2.3.5 Kesilince iptal/temizlik | Var | 1615–1621, 2019–2035, 2264–2278 | Bayraklar, kuyruk boşaltma, `shutdownSpeaker`; doğrulama maddesi. |
| 2.4 (genel) Cümle akışı | Var | 1961–2013 | `flushSentences`: min 12, azami 180 karakter; kod değişmez. 2.4.x alt maddelerinin metni elimde yok. |
| 2.4.2 | KAPSAM DIŞI | — | Karar: kapsam dışı (canlı akış). |
| 2.4.3 | KAPSAM DIŞI | — | Karar: kapsam dışı (canlı akış). |
| 2.4.6 Gecikme ölçümü | Var | 24–26 | `MARK` logları; kod değişmez. |
| 2.4.7 | KAPSAM DIŞI | — | Karar: kapsam dışı (metni elimde yok). |

## 3. Hata yönetimi

| Madde | Durum | Satır | Açıklama |
|---|---|---|---|
| 3.1 Durumların tanımı | Kısmen | 45–51 | IDLE/LISTENING/THINKING/SPEAKING/FAREWELL var; "iptal" ve "hata" durumu yok. |
| 3.2 Duruma göre geçerli dokunma | Kısmen | 416–447, 596–630 | Kurallar kodda dağınık, yazılı bir tablo yok. |
| 3.3 Önceki etkileşimi temizleme | Kısmen | 845, 2287–2290, 1213 | Kayıt yeniden yazılır, yedek cümleler temizlenir; `recordedWavBytes` hata yollarında sıfırlanmaz. |
| 3.4 Kayıt/oynatma çakışmasını önleme | Kısmen | 725–768, 1371–1426 | Aynı I2S_NUM_0; çakışma yalnızca çağrı sırasıyla önleniyor, koruma/kilit yok. |
| 3.5 API beklerken OLED/dokunma | Var | 191, 552, 657 | Touch ve Behavior ayrı görevler; bekleme animasyonu (`drawLoader`). |
| 3.6 Etkileşim sonu → boşta | Kısmen | 2446–2447 | Tüm yollar sonda `IDLE/FAREWELL` ayarlar; hata geri bildirimi yok. |
| 3.7 Wi-Fi/API hatasında kontrollü davranış | Kısmen | 2345–2363, 2426–2440 | Yalnızca log; kullanıcıya görsel/sesli hata yok. |

## 4. OLED – Servo – Ses

| Madde | Durum | Satır | Açıklama |
|---|---|---|---|
| 4.1 Göz animasyonları | Var | 74–78, 449–474 | RoboEyes, kırpma, rastgele bakış. |
| 4.2 Dinleme/işleme animasyonları | Var | 371–381, 552–587, 650 | `B_LISTEN`, `B_THINK` + `drawLoader`, `B_UNTHINK` ile çıkış. |
| 4.3 Konuşmaya geçiş | Var | 386–390, 606–612 | `B_SPEAK`, ses seviyesine göre göz yüksekliği (642–646). |
| 4.4 Servo–göz uyumu | Var | 315–324 | Bakış, bas 200 ms gecikmeyle takip eder. |
| 4.5 Mutlu/öfke/uyku çakışması | Var | 430, 596–601 | Konuşma sırasında bu tepkiler `handleTouchIdle` dışında kalır. |
| 4.6 Uyku/uyanma/boşta süreleri | Var | 259–260, 527, 532, 364–369 | 90 sn uykulu, 180 sn uyku, uyanma 1,4 sn. |
| 4.7 Donmadığını doğrula | Kısmen | 657–669 | Davranış görevi 5 ms döngüde; kanıt cihaz testi, kodda doğrulama yok. |

## 5. Kararlılık

| Madde | Durum | Satır | Açıklama |
|---|---|---|---|
| 5.1 Ayrı test senaryoları | Kısmen | — | Senaryolar planda (Bölüm 7); kodda otomatik test yok (test/ boş). |
| 5.2 Art arda konuşma turları | Kısmen | 2371–2444 | Kod destekliyor; cihazda masal testleri yapıldı. |
| 5.3 Başlatma/kesme senaryoları | Kısmen | — | Mekanizma var; sistematik tekrar testi kaydı yok. |
| 5.4 Wi-Fi kesintisi/boş STT/API hatası/sessizlik | Kısmen | 2345–2358, 2419–2428 | Kod yolları var, test sonucu yok. |
| 5.5 Uzun süreli çalıştırma | Yok | — | Test yapılmadı; `millis()` karşılaştırmalarının 49,7 gün taşması kontrol edilmedi. |
| 5.6 Kararlı sürümü Git'e kaydet | Yok | — | `git tag` boş; çalışma ağacında kaydedilmemiş değişiklik var. |
