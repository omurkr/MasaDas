# MasaDaş çalışma kuralları
Ayrıntılar ve grup durumları: MASADAS_FAZ0.1_PLAN.md (Bölüm 2 kuralların kaynağı).

1. Bir seferde tek grup; her gruptan sonra masal testi yapılmadan sonrakine geçme.
2. Faz 0.1'de TTS koduna dokunulmaz. TTS'te gerçek bir hata bulunursa kod değiştirmeden önce bana sor.
3. Yeni görev açarken öncelik 1'i geçme; TtsPlayback 2'de kalır. Touch 1/3072, Behavior 1/6144, TtsWorker 1/12288.
4. Touch görevinde `Serial` yazımı yok; `[TOUCH]` logları yorumda kalır.
5. Dosyada olmayan kodu "mevcut" sanma. Önce dosyayı oku; "şu satırı bul, şununla değiştir" biçiminde kısa ve numaralı ilerle.
6. Anlatım kısa, sade, tek aşamalı olsun; gereksiz token harcama.
7. Dosyalar UTF-8 kalmalı (`MasaDaş`, Türkçe yorumlar bozulmamalı).
8. Cihazda test edemezsin; "çalışıyor" yalnızca kullanıcı bildirirse yazılır.
9. Kod değiştirmeden, firmware yüklemeden ve Git durumunu değiştirmeden önce kullanıcı onayı al.
10. `include/secrets.h` içeriğini okuma, yazdırma veya commit'leme.
