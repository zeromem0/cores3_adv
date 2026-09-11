#ifndef TAP_LOADER_H
#define TAP_LOADER_H

#include "../arduino_compat.h"
#include <functional>
#include "spectrum_mini.h"

// Callback для рендеринга loading screen
using RenderCallback = std::function<void()>;

// ═══════════════════════════════════════════════════════════
// 📼 INSTANT TAP LOADER (простая загрузка без эмуляции ленты)
// ═══════════════════════════════════════════════════════════
// 
// Формат .TAP:
// [2 bytes: length] [data...] [2 bytes: length] [data...] ...
//
// Блоки:
// - Header (0x00): [тип] [имя 10 байт] [length] [addr] [param2] [checksum]
// - Data (0xFF): [данные...] [checksum]
//
// Типы:
// 0x00 = Program (BASIC)
// 0x01 = Number array
// 0x02 = Character array
// 0x03 = Code (machine code) ← нам нужен этот!
// ═══════════════════════════════════════════════════════════

struct TAPBlock {
  uint8_t flag;        // 0x00 = header, 0xFF = data
  uint8_t type;        // Тип блока (для header)
  char name[11];       // Имя файла (для header)
  uint16_t length;     // Длина данных
  uint16_t addr;       // Адрес загрузки (для CODE)
  uint16_t param2;     // Дополнительный параметр
  uint8_t* data;       // Указатель на данные (для data блока)
  bool isHeader;       // true = header, false = data
};

class TAPLoader {
public:
  TAPLoader();
  ~TAPLoader();
  
  // Загрузить .TAP файл (tape emulation с loading screen!)
  bool loadTAP(const char* filename, ZXSpectrum* spectrum, RenderCallback renderCallback = nullptr);

  // The same, for a tape that is already in memory -- a tape built into
  // the firmware plays straight out of flash.
  bool loadTAPFromMemory(const uint8_t* data, size_t size, ZXSpectrum* spectrum,
                         RenderCallback renderCallback = nullptr);

  // Instant load: instead of emulating the tape, the ROM's own load
  // routine is caught at its entry point and each block is handed over
  // whole. Seconds instead of a minute, at the cost of the loading
  // screen, and it only works for tapes that load through the ROM --
  // anything with its own turbo or protected loader needs the tape.
  bool loadTAPInstant(const char* filename, ZXSpectrum* spectrum, RenderCallback renderCallback = nullptr);
  bool loadTAPInstantFromMemory(const uint8_t* data, size_t size, ZXSpectrum* spectrum,
                                RenderCallback renderCallback = nullptr);
  
  // Информация о последней загрузке
  const char* getLastError() { return lastError; }
  int getBlockCount() { return blockCount; }
  
private:
  // Bring the machine up and type LOAD "": both paths start the same way.
  void bootAndTypeLoad(ZXSpectrum* spectrum, bool pressEnter = true);

  bool playTape(const uint8_t* data, size_t size, ZXSpectrum* spectrum, RenderCallback renderCallback);

  // The same, with the ROM's loader trapped rather than the tape played.
  bool trapTape(const uint8_t* data, size_t size, ZXSpectrum* spectrum, RenderCallback renderCallback);

  // Парсинг TAP файла
  bool parseTAP(uint8_t* data, size_t fileSize);
  
  // Загрузка блока в память
  bool loadBlock(TAPBlock& block, ZXSpectrum* spectrum);
  
  // Утилиты
  uint16_t readU16LE(uint8_t* p) {
    return p[0] | (p[1] << 8);
  }
  
  TAPBlock* blocks;
  int blockCount;
  int maxBlocks;
  char lastError[128];
};

#endif // TAP_LOADER_H

