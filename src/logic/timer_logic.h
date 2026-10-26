#pragma once
// Pomodoro / serbest sure zamanlayicisi (donanimdan bagimsiz, saf mantik).
// Sure karsilastirmasi millis() tasmasina dayanikli: now - start >= sure.

#include <stdint.h>

namespace timerlogic {

class PomodoroTimer {
public:
  enum Mode : uint8_t { M_IDLE, M_WORK, M_SHORT, M_LONG, M_FREE };

  static constexpr uint32_t WORK_MS = 25UL * 60UL * 1000UL;
  static constexpr uint32_t SHORT_MS = 5UL * 60UL * 1000UL;
  static constexpr uint32_t LONG_MS = 15UL * 60UL * 1000UL;
  static constexpr uint8_t ROUNDS_BEFORE_LONG = 4;

  void startPomodoro(uint32_t now);
  void startFree(uint32_t now, uint32_t durationMs);
  void togglePause(uint32_t now);
  void reset();

  // Faz bittiyse true doner (her bitis icin bir kez). Biten faz finishedMode() ile okunur.
  // Pomodoro bir sonraki faza kendiliginden gecer; serbest sure biter ve bosa duser.
  bool update(uint32_t now);

  Mode mode() const { return mode_; }
  Mode finishedMode() const { return finished_; }
  bool active() const { return mode_ != M_IDLE; }
  bool paused() const { return paused_; }
  uint8_t completedRounds() const { return round_; }
  uint32_t totalMs() const { return durMs_; }
  uint32_t remainingMs(uint32_t now) const;

private:
  void begin(Mode m, uint32_t now, uint32_t durationMs);

  Mode mode_ = M_IDLE;
  Mode finished_ = M_IDLE;
  bool paused_ = false;
  uint32_t startMs_ = 0;
  uint32_t durMs_ = 0;
  uint32_t pausedRemain_ = 0;
  uint8_t round_ = 0;
};

}  // namespace timerlogic
