#ifndef Z80_LOADER_H
#define Z80_LOADER_H

#include "../arduino_compat.h"
#include "../arduino_compat.h"
#include "spectrum_mini.h"

// ═══════════════════════════════════════════════════════════
// 📂 .Z80 SNAPSHOT LOADER
// ═══════════════════════════════════════════════════════════
// 
// Поддержка форматов:
// - Version 1: 48K, несжатый/сжатый
// - Version 2: 48K/128K, сжатый
// - Version 3: 48K/128K, сжатый
// 
// RLE compression: ED ED xx yy
//   - ED ED 00 00 = конец блока
//   - ED ED xx yy = повторить yy байт xx раз
// 
// Референс:
// - https://worldofspectrum.org/faq/reference/z80format.htm
// ═══════════════════════════════════════════════════════════

class Z80Loader {
public:
  Z80Loader();
  ~Z80Loader();
  
  // Загрузка .Z80 файла
  bool loadZ80(const char* filename, ZXSpectrum* spectrum);
  
  // Получить последнюю ошибку
  const char* getLastError() const { return lastError; }
  
private:
  char lastError[128];
  
  // Чтение little-endian значений
  uint16_t readUInt16(uint8_t* data);
  
  // RLE декомпрессия
  bool decompressBlock(uint8_t* compressed, size_t compressedSize, 
                       uint8_t* output, size_t outputSize);
  
  // Загрузка версии 1 (48K)
  bool loadVersion1(File& file, ZXSpectrum* spectrum);
  
  // Загрузка версии 2/3 (48K/128K)
  bool loadVersion2or3(File& file, ZXSpectrum* spectrum, int version);
};

#endif // Z80_LOADER_H


