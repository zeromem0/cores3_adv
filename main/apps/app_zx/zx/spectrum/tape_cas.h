#ifndef TAPE_CAS_H
#define TAPE_CAS_H

#include "../arduino_compat.h"
#include "tape_listener.h"

// ═══════════════════════════════════════════════════════════
// 📼 TAP/TZX CASSETTE EMULATOR
// ═══════════════════════════════════════════════════════════
// 
// Порт из ESP32-Rainbow (упрощённая версия только для .TAP)
// Оригинал: esp32-rainbow/firmware/src/TZX/tzx_cas.cpp
// 
// Формат .TAP:
// [2 bytes: length] [data...] [2 bytes: length] [data...] ...
// 
// Сигналы для ZX Spectrum 48K:
// - PILOT TONE: 8063 импульсов × 2168 t-states (header)
//               3223 импульса × 2168 t-states (data)
// - SYNC: 667 + 735 t-states
// - DATA: Bit 0 = 855 + 855 t-states
//         Bit 1 = 1710 + 1710 t-states
// - PAUSE: 1000 ms между блоками
// ═══════════════════════════════════════════════════════════

class TapeCas {
public:
  TapeCas() {}
  ~TapeCas() {}
  
  // Загрузить .TAP файл через tape emulation
  bool loadTap(TapeListener* listener, uint8_t* data, size_t fileSize);
  
private:
  // Обработка одного блока (pilot + sync + data + pause)
  void handleBlock(
    TapeListener* listener,
    const uint8_t* bytes,
    int pause,
    int dataSize,
    int pilot,
    int pilotLength,
    int sync1,
    int sync2,
    int bit0,
    int bit1,
    int bitsInLastByte
  );
  
  // Утилиты
  inline uint16_t getU16LE(const uint8_t* p) {
    return p[0] | (p[1] << 8);
  }
};

#endif // TAPE_CAS_H

