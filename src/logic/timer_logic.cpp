#include "timer_logic.h"

namespace timerlogic {

void PomodoroTimer::begin(Mode m, uint32_t now, uint32_t durationMs) {
  mode_ = m;
  startMs_ = now;
  durMs_ = durationMs;
  paused_ = false;
  pausedRemain_ = 0;
}

void PomodoroTimer::startPomodoro(uint32_t now) {
  round_ = 0;
  begin(M_WORK, now, WORK_MS);
}

void PomodoroTimer::startFree(uint32_t now, uint32_t durationMs) {
  if (durationMs == 0) return;
  begin(M_FREE, now, durationMs);
}

void PomodoroTimer::reset() {
  mode_ = M_IDLE;
  paused_ = false;
  round_ = 0;
  durMs_ = 0;
  pausedRemain_ = 0;
}

uint32_t PomodoroTimer::remainingMs(uint32_t now) const {
  if (mode_ == M_IDLE) return 0;
  if (paused_) return pausedRemain_;

  uint32_t elapsed = now - startMs_;
  return elapsed >= durMs_ ? 0 : durMs_ - elapsed;
}

void PomodoroTimer::togglePause(uint32_t now) {
  if (mode_ == M_IDLE) return;

  if (!paused_) {
    pausedRemain_ = remainingMs(now);
    paused_ = true;
  } else {
    // Kalan sureyi koruyarak yeniden baslat: start = now - (toplam - kalan).
    startMs_ = now - (durMs_ - pausedRemain_);
    paused_ = false;
  }
}

bool PomodoroTimer::update(uint32_t now) {
  if (mode_ == M_IDLE || paused_) return false;
  if (now - startMs_ < durMs_) return false;

  finished_ = mode_;

  switch (mode_) {
    case M_WORK:
      round_++;
      if (round_ % ROUNDS_BEFORE_LONG == 0) begin(M_LONG, now, LONG_MS);
      else begin(M_SHORT, now, SHORT_MS);
      break;
    case M_SHORT:
    case M_LONG:
      begin(M_WORK, now, WORK_MS);
      break;
    case M_FREE:
    default:
      reset();
      break;
  }

  return true;
}

}  // namespace timerlogic
