#include "z80_loader.h"
#include "../arduino_compat.h"

Z80Loader::Z80Loader() {
  lastError[0] = '\0';
}

Z80Loader::~Z80Loader() {
}

// ═══════════════════════════════════════════════════════════
// Чтение little-endian uint16
// ═══════════════════════════════════════════════════════════
uint16_t Z80Loader::readUInt16(uint8_t* data) {
  return data[0] | (data[1] << 8);
}

// ═══════════════════════════════════════════════════════════
// RLE декомпрессия (ED ED xx yy)
// ═══════════════════════════════════════════════════════════
bool Z80Loader::decompressBlock(uint8_t* compressed, size_t compressedSize, 
                                 uint8_t* output, size_t outputSize) {
  size_t inPos = 0;
  size_t outPos = 0;
  
  while (inPos < compressedSize && outPos < outputSize) {
    uint8_t byte = compressed[inPos++];
    
    // Проверяем на RLE маркер (ED ED)
    if (byte == 0xED && inPos < compressedSize && compressed[inPos] == 0xED) {
      inPos++; // Пропускаем второй ED
      
      if (inPos + 1 >= compressedSize) {
        snprintf(lastError, sizeof(lastError), "RLE: unexpected end");
        return false;
      }
      
      uint8_t count = compressed[inPos++];
      uint8_t value = compressed[inPos++];
      
      // ED ED 00 00 = конец блока (для версии 1)
      if (count == 0 && value == 0) {
        break;
      }
      
      // Повторяем value count раз
      for (int i = 0; i < count && outPos < outputSize; i++) {
        output[outPos++] = value;
      }
    } else {
      // Обычный байт
      output[outPos++] = byte;
    }
  }
  
  return true;
}

// ═══════════════════════════════════════════════════════════
// Загрузка .Z80 Version 1 (48K)
// ═══════════════════════════════════════════════════════════
bool Z80Loader::loadVersion1(File& file, ZXSpectrum* spectrum) {
  Serial.println("📂 Z80 Version 1 (48K)");
  
  // Header уже прочитан (30 байт)
  // Перечитываем для удобства
  file.seek(0);
  
  uint8_t header[30];
  if (file.read(header, 30) != 30) {
    snprintf(lastError, sizeof(lastError), "Failed to read v1 header");
    return false;
  }
  
  // ═══ РЕГИСТРЫ ═══
  uint8_t A = header[0];
  uint8_t F = header[1];
  uint16_t BC = readUInt16(&header[2]);
  uint16_t HL = readUInt16(&header[4]);
  uint16_t PC = readUInt16(&header[6]);  // В v1 PC != 0
  uint16_t SP = readUInt16(&header[8]);
  uint8_t I = header[10];
  uint8_t R = header[11] & 0x7F;  // Bit 7 отдельно
  
  uint8_t flags1 = header[12];
  bool compressed = (flags1 != 255);  // 255 = несжатый
  
  uint16_t DE = readUInt16(&header[13]);
  uint16_t BC_ = readUInt16(&header[15]);
  uint16_t DE_ = readUInt16(&header[17]);
  uint16_t HL_ = readUInt16(&header[19]);
  uint8_t A_ = header[21];
  uint8_t F_ = header[22];
  uint16_t IY = readUInt16(&header[23]);
  uint16_t IX = readUInt16(&header[25]);
  
  uint8_t IFF1 = header[27];
  uint8_t IFF2 = header[28];
  uint8_t IM = header[29] & 0x03;  // Bits 0-1
  
  Serial.printf("   PC=0x%04X, SP=0x%04X, IM=%d, IFF1=%d\n", PC, SP, IM, IFF1);
  Serial.printf("   Compressed: %s\n", compressed ? "YES" : "NO");
  
  // ═══ ПАМЯТЬ ═══
  size_t remainingSize = file.size() - 30;
  uint8_t* memData = (uint8_t*)malloc(remainingSize);
  if (!memData) {
    snprintf(lastError, sizeof(lastError), "Out of memory");
    return false;
  }
  
  if (file.read(memData, remainingSize) != remainingSize) {
    free(memData);
    snprintf(lastError, sizeof(lastError), "Failed to read memory");
    return false;
  }
  
  // Декомпрессия (если нужно)
  if (compressed) {
    Serial.println("   Decompressing memory...");
    uint8_t* decompressed = (uint8_t*)malloc(49152);  // 48K
    if (!decompressed) {
      free(memData);
      snprintf(lastError, sizeof(lastError), "Out of memory for decompression");
      return false;
    }
    
    if (!decompressBlock(memData, remainingSize, decompressed, 49152)) {
      free(memData);
      free(decompressed);
      return false;
    }
    
    // Копируем в эмулятор (16384-65535 = RAM 48K)
    spectrum->mem.writeRam(0x4000, decompressed, 49152);
    free(decompressed);
  } else {
    // Несжатый - копируем напрямую
    size_t copySize = (remainingSize < 49152) ? remainingSize : 49152;
    spectrum->mem.writeRam(0x4000, memData, copySize);
  }
  
  free(memData);
  
  // ═══ ЗАГРУЗКА РЕГИСТРОВ В ЭМУЛЯТОР ═══
  // Используем .B.h и .B.l как в ESP32 Rainbow!
  spectrum->z80Regs->AF.B.h = A;
  spectrum->z80Regs->AF.B.l = F;
  spectrum->z80Regs->BC.B.l = BC & 0xFF;
  spectrum->z80Regs->BC.B.h = (BC >> 8) & 0xFF;
  spectrum->z80Regs->DE.B.l = DE & 0xFF;
  spectrum->z80Regs->DE.B.h = (DE >> 8) & 0xFF;
  spectrum->z80Regs->HL.B.l = HL & 0xFF;
  spectrum->z80Regs->HL.B.h = (HL >> 8) & 0xFF;
  spectrum->z80Regs->AFs.B.h = A_;
  spectrum->z80Regs->AFs.B.l = F_;
  spectrum->z80Regs->BCs.B.l = BC_ & 0xFF;
  spectrum->z80Regs->BCs.B.h = (BC_ >> 8) & 0xFF;
  spectrum->z80Regs->DEs.B.l = DE_ & 0xFF;
  spectrum->z80Regs->DEs.B.h = (DE_ >> 8) & 0xFF;
  spectrum->z80Regs->HLs.B.l = HL_ & 0xFF;
  spectrum->z80Regs->HLs.B.h = (HL_ >> 8) & 0xFF;
  spectrum->z80Regs->IX.B.l = IX & 0xFF;
  spectrum->z80Regs->IX.B.h = (IX >> 8) & 0xFF;
  spectrum->z80Regs->IY.B.l = IY & 0xFF;
  spectrum->z80Regs->IY.B.h = (IY >> 8) & 0xFF;
  spectrum->z80Regs->SP.B.l = SP & 0xFF;
  spectrum->z80Regs->SP.B.h = (SP >> 8) & 0xFF;
  spectrum->z80Regs->PC.B.l = PC & 0xFF;
  spectrum->z80Regs->PC.B.h = (PC >> 8) & 0xFF;
  spectrum->z80Regs->I = I;
  spectrum->z80Regs->R.B.l = R;  // R - младший байт
  spectrum->z80Regs->R.B.h = 0;  // R.h всегда 0
  spectrum->z80Regs->IFF1 = IFF1 & 0x01;
  spectrum->z80Regs->IFF2 = IFF2 & 0x01;
  spectrum->z80Regs->IM = IM & 0x03;
  
  // Border color (bits 1-3 of flags1)
  if (flags1 != 255) {
    spectrum->borderColor = (flags1 >> 1) & 0x07;
  }
  
  Serial.println("✅ Z80 v1 loaded successfully!");
  return true;
}

// ═══════════════════════════════════════════════════════════
// Загрузка .Z80 Version 2/3 (48K/128K)
// ═══════════════════════════════════════════════════════════
bool Z80Loader::loadVersion2or3(File& file, ZXSpectrum* spectrum, int version) {
  Serial.printf("📂 Z80 Version %d (48K/128K)\n", version);
  
  // Читаем основной header (30 байт)
  file.seek(0);
  uint8_t header[30];
  if (file.read(header, 30) != 30) {
    snprintf(lastError, sizeof(lastError), "Failed to read header");
    return false;
  }
  
  // PC в v2/v3 хранится в расширенном header
  // В основном header PC = 0
  
  // ═══ РЕГИСТРЫ из основного header ═══
  uint8_t A = header[0];
  uint8_t F = header[1];
  uint16_t BC = readUInt16(&header[2]);
  uint16_t HL = readUInt16(&header[4]);
  // PC пропускаем (будет в расширенном header)
  uint16_t SP = readUInt16(&header[8]);
  uint8_t I = header[10];
  uint8_t R = header[11] & 0x7F;
  
  uint8_t flags1 = header[12];
  
  uint16_t DE = readUInt16(&header[13]);
  uint16_t BC_ = readUInt16(&header[15]);
  uint16_t DE_ = readUInt16(&header[17]);
  uint16_t HL_ = readUInt16(&header[19]);
  uint8_t A_ = header[21];
  uint8_t F_ = header[22];
  uint16_t IY = readUInt16(&header[23]);
  uint16_t IX = readUInt16(&header[25]);
  
  uint8_t IFF1 = header[27];
  uint8_t IFF2 = header[28];
  uint8_t IM = header[29] & 0x03;
  
  // ═══ РАСШИРЕННЫЙ HEADER ═══
  uint8_t extHeader[54];  // Максимум 54 байта для v3
  uint16_t extHeaderLen = 0;
  
  if (file.read((uint8_t*)&extHeaderLen, 2) != 2) {
    snprintf(lastError, sizeof(lastError), "Failed to read ext header length");
    return false;
  }
  
  Serial.printf("   Extended header length: %d bytes\n", extHeaderLen);
  
  if (extHeaderLen > 54) {
    snprintf(lastError, sizeof(lastError), "Extended header too large: %d", extHeaderLen);
    return false;
  }
  
  if (file.read(extHeader, extHeaderLen) != extHeaderLen) {
    snprintf(lastError, sizeof(lastError), "Failed to read ext header");
    return false;
  }
  
  // PC из расширенного header
  uint16_t PC = readUInt16(&extHeader[0]);
  
  // Hardware mode (byte 2)
  uint8_t hwMode = extHeader[2];
  
  Serial.printf("   PC=0x%04X, SP=0x%04X, IM=%d, IFF1=%d\n", PC, SP, IM, IFF1);
  Serial.printf("   Hardware mode: %d\n", hwMode);
  
  // Проверяем, что это 48K режим (0=48K, 1=48K+IF1, 3=48K+MGT)
  if (hwMode != 0 && hwMode != 1 && hwMode != 3) {
    Serial.printf("⚠️  WARNING: Hardware mode %d (not 48K), trying anyway...\n", hwMode);
  }
  
  // ═══ MEMORY BLOCKS ═══
  // Формат: [length:2][page:1][data:length]
  // Page для 48K:
  //   4 = 0x8000-0xBFFF (32768-49151)
  //   5 = 0xC000-0xFFFF (49152-65535)
  //   8 = 0x4000-0x7FFF (16384-32767)
  
  while (file.available()) {
    uint16_t blockLen;
    uint8_t page;
    
    if (file.read((uint8_t*)&blockLen, 2) != 2) break;
    if (file.read(&page, 1) != 1) break;
    
    Serial.printf("   Block: page=%d, length=%d\n", page, blockLen);
    
    // Определяем адрес назначения
    uint16_t destAddr = 0;
    if (page == 4) destAddr = 0x8000;  // 32768
    else if (page == 5) destAddr = 0xC000;  // 49152
    else if (page == 8) destAddr = 0x4000;  // 16384
    else {
      Serial.printf("⚠️  WARNING: Unknown page %d, skipping\n", page);
      file.seek(file.position() + blockLen);
      continue;
    }
    
    // Читаем сжатые данные
    uint8_t* compressed = (uint8_t*)malloc(blockLen);
    if (!compressed) {
      snprintf(lastError, sizeof(lastError), "Out of memory for block");
      return false;
    }
    
    if (file.read(compressed, blockLen) != blockLen) {
      free(compressed);
      snprintf(lastError, sizeof(lastError), "Failed to read block");
      return false;
    }
    
    // Декомпрессия
    uint8_t* decompressed = (uint8_t*)malloc(16384);  // Каждый блок = 16K
    if (!decompressed) {
      free(compressed);
      snprintf(lastError, sizeof(lastError), "Out of memory for decompression");
      return false;
    }
    
    if (!decompressBlock(compressed, blockLen, decompressed, 16384)) {
      free(compressed);
      free(decompressed);
      return false;
    }
    
    // Копируем в память эмулятора
    spectrum->mem.writeRam(destAddr, decompressed, 16384);
    
    free(compressed);
    free(decompressed);
  }
  
  // ═══ ЗАГРУЗКА РЕГИСТРОВ ═══
  // Используем .B.h и .B.l как в ESP32 Rainbow!
  spectrum->z80Regs->AF.B.h = A;
  spectrum->z80Regs->AF.B.l = F;
  spectrum->z80Regs->BC.B.l = BC & 0xFF;
  spectrum->z80Regs->BC.B.h = (BC >> 8) & 0xFF;
  spectrum->z80Regs->DE.B.l = DE & 0xFF;
  spectrum->z80Regs->DE.B.h = (DE >> 8) & 0xFF;
  spectrum->z80Regs->HL.B.l = HL & 0xFF;
  spectrum->z80Regs->HL.B.h = (HL >> 8) & 0xFF;
  spectrum->z80Regs->AFs.B.h = A_;
  spectrum->z80Regs->AFs.B.l = F_;
  spectrum->z80Regs->BCs.B.l = BC_ & 0xFF;
  spectrum->z80Regs->BCs.B.h = (BC_ >> 8) & 0xFF;
  spectrum->z80Regs->DEs.B.l = DE_ & 0xFF;
  spectrum->z80Regs->DEs.B.h = (DE_ >> 8) & 0xFF;
  spectrum->z80Regs->HLs.B.l = HL_ & 0xFF;
  spectrum->z80Regs->HLs.B.h = (HL_ >> 8) & 0xFF;
  spectrum->z80Regs->IX.B.l = IX & 0xFF;
  spectrum->z80Regs->IX.B.h = (IX >> 8) & 0xFF;
  spectrum->z80Regs->IY.B.l = IY & 0xFF;
  spectrum->z80Regs->IY.B.h = (IY >> 8) & 0xFF;
  spectrum->z80Regs->SP.B.l = SP & 0xFF;
  spectrum->z80Regs->SP.B.h = (SP >> 8) & 0xFF;
  spectrum->z80Regs->PC.B.l = PC & 0xFF;
  spectrum->z80Regs->PC.B.h = (PC >> 8) & 0xFF;
  spectrum->z80Regs->I = I;
  spectrum->z80Regs->R.B.l = R;
  spectrum->z80Regs->R.B.h = 0;
  spectrum->z80Regs->IFF1 = IFF1 & 0x01;
  spectrum->z80Regs->IFF2 = IFF2 & 0x01;
  spectrum->z80Regs->IM = IM & 0x03;
  
  // Border
  if (flags1 != 255) {
    spectrum->borderColor = (flags1 >> 1) & 0x07;
  }
  
  Serial.printf("✅ Z80 v%d loaded successfully!\n", version);
  return true;
}

// ═══════════════════════════════════════════════════════════
// Главная функция загрузки
// ═══════════════════════════════════════════════════════════
bool Z80Loader::loadZ80(const char* filename, ZXSpectrum* spectrum) {
  Serial.println("\n═══════════════════════════════════════════");
  Serial.printf("📂 LOADING .Z80 FILE: %s\n", filename);
  Serial.println("═══════════════════════════════════════════");
  
  // Открываем файл
  File file = SD.open(filename, FILE_READ);
  if (!file) {
    snprintf(lastError, sizeof(lastError), "File not found: %s", filename);
    Serial.printf("❌ ERROR: %s\n", lastError);
    return false;
  }
  
  // Проверяем размер
  size_t fileSize = file.size();
  Serial.printf("File size: %d bytes\n", fileSize);
  
  if (fileSize < 30) {
    file.close();
    snprintf(lastError, sizeof(lastError), "File too small");
    Serial.printf("❌ ERROR: %s\n", lastError);
    return false;
  }
  
  // Читаем header для определения версии
  uint8_t header[30];
  if (file.read(header, 30) != 30) {
    file.close();
    snprintf(lastError, sizeof(lastError), "Failed to read header");
    Serial.printf("❌ ERROR: %s\n", lastError);
    return false;
  }
  
  // Определяем версию по PC (byte 6-7)
  uint16_t PC = readUInt16(&header[6]);
  
  bool success = false;
  
  if (PC != 0) {
    // Version 1 (PC != 0)
    success = loadVersion1(file, spectrum);
  } else {
    // Version 2 или 3 (PC == 0, читаем расширенный header)
    // Определяем версию по длине расширенного header
    file.seek(30);
    uint16_t extHeaderLen = 0;
    file.read((uint8_t*)&extHeaderLen, 2);
    
    int version = (extHeaderLen == 23) ? 2 : 3;
    
    success = loadVersion2or3(file, spectrum, version);
  }
  
  file.close();
  
  if (success) {
    Serial.println("═══════════════════════════════════════════");
    Serial.println("✅ .Z80 FILE LOADED SUCCESSFULLY!");
    Serial.println("═══════════════════════════════════════════\n");
  } else {
    Serial.println("═══════════════════════════════════════════");
    Serial.printf("❌ FAILED TO LOAD .Z80: %s\n", lastError);
    Serial.println("═══════════════════════════════════════════\n");
  }
  
  return success;
}

