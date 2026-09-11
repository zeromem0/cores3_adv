#include "tape_cas.h"

// ═══════════════════════════════════════════════════════════
// LOAD TAP FILE (порт из ESP32-Rainbow)
// ═══════════════════════════════════════════════════════════

bool TapeCas::loadTap(TapeListener* listener, uint8_t* casdata, size_t caslen) {
  Serial.println("\n📼 Starting TAP emulation...");
  
  const uint8_t* p = casdata;
  int blockNum = 0;
  
  while (p < casdata + caslen) {
    // Читаем длину блока (2 байта little-endian)
    if (p + 2 > casdata + caslen) {
      Serial.println("⚠️  Incomplete block header");
      break;
    }
    
    int dataSize = getU16LE(p);
    p += 2;
    
    if (dataSize == 0 || p + dataSize > casdata + caslen) {
      Serial.printf("⚠️  Invalid block size: %d\n", dataSize);
      break;
    }
    
    // Определяем тип блока по флагу (0x00 = header, 0xFF = data)
    uint8_t flag = p[0];
    bool isHeader = (flag == 0x00);
    
    // Для header: длинный pilot (8063), для data: короткий (3223)
    int pilotLength = isHeader ? 8063 : 3223;
    
    Serial.printf("📦 Block %d: %s (%d bytes)\n", 
                  blockNum++, 
                  isHeader ? "HEADER" : "DATA",
                  dataSize);
    
    // Обрабатываем блок:
    // - pilot: 2168 t-states × pilotLength
    // - sync: 667 + 735 t-states
    // - data: bit0=855+855, bit1=1710+1710 t-states
    // - pause: 1000 ms
    handleBlock(
      listener,
      p,              // данные
      1000,           // pause (ms)
      dataSize,       // размер данных
      2168,           // pilot pulse length (t-states)
      pilotLength,    // pilot repetitions
      667,            // sync1 (t-states)
      735,            // sync2 (t-states)
      855,            // bit 0 pulse (t-states)
      1710,           // bit 1 pulse (t-states)
      8               // bits in last byte
    );
    
    p += dataSize;
  }
  
  Serial.println("✅ TAP emulation complete!");
  return true;
}

// ═══════════════════════════════════════════════════════════
// HANDLE BLOCK - генерация сигналов для одного блока
// ═══════════════════════════════════════════════════════════

void TapeCas::handleBlock(
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
) {
  // ═══ 1. PILOT TONE ═══
  // Длинный тон для синхронизации
  // Serial.printf("  🎵 Pilot: %d × %d t-states\n", pilotLength, pilot);
  
  // Border = CYAN во время pilot (как на реальном Spectrum!)
  ZXSpectrum* spectrum = listener->spectrum;
  uint8_t oldBorder = spectrum->borderColor;
  spectrum->borderColor = 5;  // CYAN
  
  for (int i = 0; i < pilotLength; i++) {
    listener->runForTicks(pilot);
    listener->toggleMic();
    
    // Рендерим каждые 500 импульсов (не слишком часто)
    if (i % 500 == 0) {
      listener->tryRender(1);  // throttle=1 → рендерим сразу
    }
  }
  
  // ═══ 2. SYNC PULSES ═══
  // Два коротких импульса для синхронизации начала данных
  // Serial.println("  🔄 Sync");
  if (sync1 > 0) {
    listener->runForTicks(sync1);
    listener->toggleMic();
  }
  if (sync2 > 0) {
    listener->runForTicks(sync2);
    listener->toggleMic();
  }
  
  // ═══ 3. DATA BITS ═══
  // Каждый бит = 2 импульса (положительный + отрицательный)
  // Bit 0: короткий импульс (855 + 855 t-states)
  // Bit 1: длинный импульс (1710 + 1710 t-states)
  // Serial.printf("  📊 Data: %d bytes\n", dataSize);
  
  // Border = YELLOW/RED во время data (как на реальном Spectrum!)
  spectrum->borderColor = 6;  // YELLOW
  
  for (int dataIndex = 0; dataIndex < dataSize; dataIndex++) {
    uint8_t byte = bytes[dataIndex];
    int bitsToGo = (dataIndex == dataSize - 1) ? bitsInLastByte : 8;
    
    for (int bit = 0; bit < bitsToGo; bit++) {
      // Читаем бит (старший бит первым)
      bool bitValue = (byte & 0x80);
      byte <<= 1;
      
      // Генерируем импульс соответствующей длины
      int bitLength = bitValue ? bit1 : bit0;
      
      // Положительный импульс
      listener->runForTicks(bitLength);
      listener->toggleMic();
      
      // Отрицательный импульс
      listener->runForTicks(bitLength);
      listener->toggleMic();
    }
    
    // 🎨 РЕНДЕРИМ КАЖДЫЕ 32 БАЙТА (4 СТРОКИ ZX Spectrum!)
    // 32 байта = ~4 строки экрана
    // Для SCREEN$ (6912 байт) = 216 обновлений (БЫСТРО!)
    // ⚡⚡ УСКОРЕННАЯ ЗАГРУЗКА - эффект всё ещё виден!
    if (dataIndex % 32 == 0) {
      listener->tryRender(1);
    }
  }
  
  // Принудительный рендеринг после завершения блока
  listener->forceRender();
  
  // ═══ 4. PAUSE ═══
  // Пауза между блоками (обычно 1 секунда)
  if (pause > 0) {
    // Serial.printf("  ⏸️  Pause: %d ms\n", pause);
    listener->pause1Millis();
    listener->setMicLow();
    for (int i = 0; i < pause - 1; i++) {
      listener->pause1Millis();
    }
  }
  
  // Восстанавливаем border цвет
  spectrum->borderColor = oldBorder;
}

