#pragma once
// Tepki suresi ve nokta oyunlarinin kurallari (donanimdan bagimsiz, saf mantik).
// Zaman disaridan verilir (millis); tasmaya dayanikli cikarma kullanilir.

#include <stdint.h>

namespace gamelogic {

// ---- Tepki suresi: sinyal gorununce en hizli dokun ----
class ReactionGame {
public:
  enum State : uint8_t { S_WAIT, S_GO, S_RESULT, S_EARLY };

  static constexpr uint32_t FALSE_START_MS = 80;   // bundan kisa tepki insan icin mumkun degil

  void start(uint32_t now, uint32_t waitMs);

  // Bekleme bitince GO'ya gecer; gecis aninda true doner.
  bool update(uint32_t now);

  // Sinyal ekranda gosterildi (goAt = ekrana yazma sonrasi zaman).
  void markShown(uint32_t shownAt);

  // pressAtMs: ham dokunus zamani (sensorun ilk degisimi).
  void onPress(uint32_t pressAtMs);

  State state() const { return state_; }
  bool goShown() const { return shown_; }
  uint32_t reactionMs() const { return reaction_; }
  bool newBest() const { return newBest_; }
  uint32_t best() const { return best_; }
  void setBest(uint32_t ms) { best_ = ms; }

private:
  State state_ = S_WAIT;
  uint32_t startMs_ = 0, waitMs_ = 0, goAt_ = 0, reaction_ = 0, best_ = 0;
  bool shown_ = false;
  bool newBest_ = false;
};

// ---- Nokta yakala: nokta gidip gelir, hedef bolgedeyken dokun ----
class DotGame {
public:
  enum State : uint8_t { S_PLAY, S_OVER };
  enum Result : uint8_t { R_NONE, R_HIT, R_MISS };

  static constexpr float TRACK_MIN = 6.0f;
  static constexpr float TRACK_MAX = 122.0f;
  static constexpr float CENTER = 64.0f;
  static constexpr int START_LIVES = 3;

  void start(uint32_t now, bool goRight);
  void update(uint32_t now);
  Result onTap(uint32_t now);

  State state() const { return state_; }
  float x() const { return x_; }
  float zoneHalf() const { return zone_; }
  int score() const { return score_; }
  int lives() const { return lives_; }
  uint32_t overAt() const { return overAt_; }
  bool newBest() const { return newBest_; }
  uint32_t best() const { return best_; }
  void setBest(uint32_t v) { best_ = v; }

private:
  State state_ = S_OVER;
  float x_ = TRACK_MIN, dir_ = 1.0f, speed_ = 70.0f, zone_ = 14.0f;
  int score_ = 0, lives_ = START_LIVES;
  uint32_t last_ = 0, overAt_ = 0, best_ = 0;
  bool newBest_ = false;
};

}  // namespace gamelogic
