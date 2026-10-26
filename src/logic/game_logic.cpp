#include "game_logic.h"

namespace gamelogic {

// ---------- Tepki suresi ----------
void ReactionGame::start(uint32_t now, uint32_t waitMs) {
  state_ = S_WAIT;
  startMs_ = now;
  waitMs_ = waitMs;
  goAt_ = 0;
  reaction_ = 0;
  shown_ = false;
  newBest_ = false;
}

bool ReactionGame::update(uint32_t now) {
  if (state_ != S_WAIT) return false;
  if (now - startMs_ < waitMs_) return false;

  state_ = S_GO;
  shown_ = false;
  return true;
}

void ReactionGame::markShown(uint32_t shownAt) {
  goAt_ = shownAt;
  shown_ = true;
}

void ReactionGame::onPress(uint32_t pressAtMs) {
  if (state_ == S_WAIT) {
    state_ = S_EARLY;
    return;
  }

  if (state_ != S_GO) return;

  // Sinyal henuz cizilmediyse ya da dokunus sinyalden once basladiysa erken sayilir.
  if (!shown_ || static_cast<int32_t>(pressAtMs - goAt_) < 0) {
    state_ = S_EARLY;
    return;
  }

  uint32_t r = pressAtMs - goAt_;
  if (r < FALSE_START_MS) {
    state_ = S_EARLY;
    return;
  }

  reaction_ = r;
  state_ = S_RESULT;
  newBest_ = (best_ == 0 || r < best_);
  if (newBest_) best_ = r;
}

// ---------- Nokta yakala ----------
void DotGame::start(uint32_t now, bool goRight) {
  state_ = S_PLAY;
  x_ = goRight ? TRACK_MIN : TRACK_MAX;
  dir_ = goRight ? 1.0f : -1.0f;
  speed_ = 70.0f;
  zone_ = 14.0f;
  score_ = 0;
  lives_ = START_LIVES;
  last_ = now;
  overAt_ = 0;
  newBest_ = false;
}

void DotGame::update(uint32_t now) {
  if (state_ != S_PLAY) return;

  uint32_t dt = now - last_;
  last_ = now;
  if (dt > 100) dt = 100;   // uzun duraklamada nokta sicramasin

  x_ += dir_ * speed_ * static_cast<float>(dt) / 1000.0f;

  if (x_ >= TRACK_MAX) {
    x_ = TRACK_MAX;
    dir_ = -1.0f;
  } else if (x_ <= TRACK_MIN) {
    x_ = TRACK_MIN;
    dir_ = 1.0f;
  }
}

DotGame::Result DotGame::onTap(uint32_t now) {
  if (state_ != S_PLAY) return R_NONE;

  float d = x_ - CENTER;
  if (d < 0) d = -d;

  if (d <= zone_) {
    score_++;
    speed_ = 70.0f + 9.0f * static_cast<float>(score_);
    if (speed_ > 230.0f) speed_ = 230.0f;
    zone_ = 14.0f - 0.4f * static_cast<float>(score_);
    if (zone_ < 5.0f) zone_ = 5.0f;
    return R_HIT;
  }

  lives_--;
  if (lives_ <= 0) {
    state_ = S_OVER;
    overAt_ = now;
    newBest_ = (static_cast<uint32_t>(score_) > best_);
    if (newBest_) best_ = static_cast<uint32_t>(score_);
  }
  return R_MISS;
}

}  // namespace gamelogic
