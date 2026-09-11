#ifndef TAPE_LISTENER_H
#define TAPE_LISTENER_H

#include "../arduino_compat.h"
#include <functional>
#include "spectrum_mini.h"

// ═══════════════════════════════════════════════════════════
// 📼 TAPE LISTENER - для эмуляции кассетной ленты
// ═══════════════════════════════════════════════════════════
// 
// Принцип работы:
// 1. TapeListener управляет битом EAR (mic input) через setMicHigh/Low
// 2. Вызывает runForTicks() для запуска эмулятора на N t-states
// 3. Эмулятор читает порт 0xFE и получает текущее значение EAR бита
// 4. ROM выполняет LOAD "" и "слушает" кассетные сигналы
// 
// V3.115: Добавлен callback для рендеринга loading screen! 🎨
// V3.117: Добавлена генерация beeper звука во время загрузки! 🔊
// 
// Реализовано по аналогии с ESP32-Rainbow:
// - ZXSpectrumTapeListener из esp32-rainbow/firmware/src/TZX/
// ═══════════════════════════════════════════════════════════

#define CPU_FREQ 3500000
#define MILLI_SECOND (CPU_FREQ / 1000)

// Callback для рендеринга экрана
using RenderCallback = std::function<void()>;

// Forward declaration для аудио системы
extern void ZX_BeeperSubmitFrame(const uint16_t* accum312);
// Доступ к настройкам громкости
extern int soundVolume;      // 0-10
extern bool soundEnabled;    // true/false

class TapeListener {
public:
  ZXSpectrum* spectrum;  // Публичный доступ для tape_cas.cpp
  
protected:
  uint64_t totalTicks = 0;
  uint64_t elapsedTicks = 0;
  RenderCallback renderCallback;
  int renderCounter = 0;
  
  // ═══ АУДИО ДЛЯ BEEPER ЗВУКОВ ═══
  uint16_t audioBuffer[312];     // Буфер для 312 строк
  int audioLineCounter = 0;       // Счётчик строк
  uint64_t audioTicksAccumulator = 0;  // Накопленные t-states
  uint16_t lastAmplitude = 0;     // Предыдущее значение (для сглаживания)
  
public:
  TapeListener(ZXSpectrum* spec, RenderCallback callback = nullptr) 
    : spectrum(spec), renderCallback(callback) {
    memset(audioBuffer, 0, sizeof(audioBuffer));
  }
  virtual ~TapeListener() {}
  
  // ═══ УПРАВЛЕНИЕ EAR БИТОМ ═══
  
  inline void toggleMic() {
    spectrum->toggleMic();
  }
  
  inline void setMicHigh() {
    spectrum->setMicHigh();
  }
  
  inline void setMicLow() {
    spectrum->setMicLow();
  }
  
  // ═══ ЗАПУСК ЭМУЛЯТОРА ═══
  
  inline void runForTicks(uint64_t ticks) {
    addTicks(ticks);
    spectrum->runForCycles(ticks);
    
    // 🔊 АУДИО: накапливаем t-states и генерируем звук
    audioTicksAccumulator += ticks;
    
    // Каждые 224 t-states = 1 строка
    while (audioTicksAccumulator >= 224) {
      audioTicksAccumulator -= 224;
      
      // Записываем текущее значение EAR input с учётом громкости!
      // Во время tape emulation звук идёт от micLevel, а не soundBits!
      // Применяем громкость: 0-10 → 0.0-1.0
      // 🔊 TAPE LOADING: Автоматически на 2 уровня тише!
      int tapeVolume = soundVolume >= 2 ? (soundVolume - 2) : 0;
      float volumeScale = soundEnabled ? (float(tapeVolume) / 10.0f) : 0.0f;
      uint16_t targetAmplitude = spectrum->micLevel ? uint16_t(224 * volumeScale) : 0;
      
      // 🎵 СГЛАЖИВАНИЕ для чистоты: среднее между текущим и предыдущим
      uint16_t smoothedAmplitude = (targetAmplitude + lastAmplitude) / 2;
      audioBuffer[audioLineCounter] = smoothedAmplitude;
      lastAmplitude = targetAmplitude;
      audioLineCounter++;
      
      // Когда накопилось 312 строк = 1 кадр → отправляем!
      if (audioLineCounter >= 312) {
        ZX_BeeperSubmitFrame(audioBuffer);
        audioLineCounter = 0;
        memset(audioBuffer, 0, sizeof(audioBuffer));
      }
    }
  }
  
  inline void pause1Millis() {
    addTicks(MILLI_SECOND);
    spectrum->runForCycles(MILLI_SECOND);
    
    // Аудио для пауз тоже генерируем (с учётом громкости + сглаживание)
    audioTicksAccumulator += MILLI_SECOND;
    while (audioTicksAccumulator >= 224) {
      audioTicksAccumulator -= 224;
      // 🔊 TAPE LOADING: Автоматически на 2 уровня тише!
      int tapeVolume = soundVolume >= 2 ? (soundVolume - 2) : 0;
      float volumeScale = soundEnabled ? (float(tapeVolume) / 10.0f) : 0.0f;
      uint16_t targetAmplitude = spectrum->micLevel ? uint16_t(224 * volumeScale) : 0;
      uint16_t smoothedAmplitude = (targetAmplitude + lastAmplitude) / 2;
      audioBuffer[audioLineCounter] = smoothedAmplitude;
      lastAmplitude = targetAmplitude;
      audioLineCounter++;
      if (audioLineCounter >= 312) {
        ZX_BeeperSubmitFrame(audioBuffer);
        audioLineCounter = 0;
        memset(audioBuffer, 0, sizeof(audioBuffer));
      }
    }
  }
  
  // ═══ ПРОГРЕСС ═══
  
  inline void addTicks(uint64_t ticks) {
    totalTicks += ticks;
    elapsedTicks += ticks;
    
    // Каждые ~10 строк (10 * 224 t-states) обновляем прогресс
    if (elapsedTicks >= 10 * 224) {
      elapsedTicks = 0;
      // Здесь можно добавить callback для progress bar
      // Serial.printf(".");
    }
  }
  
  uint64_t getTotalTicks() const {
    return totalTicks;
  }
  
  // ═══ РЕНДЕРИНГ LOADING SCREEN ═══
  
  // Вызвать рендеринг (с throttling)
  inline void tryRender(int throttle = 10) {
    if (renderCallback && ++renderCounter >= throttle) {
      renderCounter = 0;
      renderCallback();
    }
  }
  
  // Принудительный рендеринг (без throttling)
  inline void forceRender() {
    if (renderCallback) {
      renderCallback();
    }
  }
};

#endif // TAPE_LISTENER_H

