#include "tap_loader.h"
#include "tape_listener.h"
#include "tape_cas.h"
#include "../arduino_compat.h"

TAPLoader::TAPLoader() {
  blocks = nullptr;
  blockCount = 0;
  maxBlocks = 0;
  lastError[0] = '\0';
}

TAPLoader::~TAPLoader() {
  if (blocks) {
    // Освобождаем данные блоков
    for (int i = 0; i < blockCount; i++) {
      if (blocks[i].data) {
        free(blocks[i].data);
      }
    }
    free(blocks);
  }
}

// ═══════════════════════════════════════════════════════════
// LOAD TAP - TAPE EMULATION (правильный способ!)
// ═══════════════════════════════════════════════════════════

bool TAPLoader::loadTAP(const char* filename, ZXSpectrum* spectrum, RenderCallback renderCallback) {
  Serial.println("\n═══════════════════════════════════════════");
  Serial.printf("📼 LOADING TAP (TAPE EMULATION): %s\n", filename);
  Serial.println("═══════════════════════════════════════════");
  
  // ═══ 1. ОТКРЫВАЕМ ФАЙЛ ═══
  File file = SD.open(filename);
  if (!file) {
    snprintf(lastError, sizeof(lastError), "Failed to open file");
    Serial.println("❌ Failed to open TAP file");
    return false;
  }
  
  size_t fileSize = file.size();
  Serial.printf("📊 File size: %d bytes\n", fileSize);
  
  // ═══ 2. ЧИТАЕМ ФАЙЛ В ПАМЯТЬ ═══
  uint8_t* tapData = (uint8_t*)malloc(fileSize);
  if (!tapData) {
    snprintf(lastError, sizeof(lastError), "Out of memory");
    file.close();
    Serial.println("❌ Out of memory");
    return false;
  }
  
  file.read(tapData, fileSize);
  file.close();

  bool ok = playTape(tapData, fileSize, spectrum, renderCallback);
  free(tapData);
  return ok;
}

// A tape already in memory: one built into the firmware is read straight
// out of flash, so nothing is copied and no heap is needed for it.
bool TAPLoader::loadTAPFromMemory(const uint8_t* data, size_t size, ZXSpectrum* spectrum,
                                  RenderCallback renderCallback) {
  if (data == nullptr || size == 0) {
    snprintf(lastError, sizeof(lastError), "Empty tape");
    return false;
  }
  Serial.printf("📼 LOADING TAP FROM FLASH: %d bytes\n", (int)size);
  return playTape(data, size, spectrum, renderCallback);
}

// Bring the machine up and put LOAD "" on its command line. Both loading
// paths start here: what differs is only where the bytes come from after
// the ROM asks for them.
void TAPLoader::bootAndTypeLoad(ZXSpectrum* spectrum, bool pressEnter) {
  // ═══ 3. RESET SPECTRUM ═══
  Serial.println("\n🔄 Resetting Spectrum...");
  spectrum->reset();

  // ═══ 4. ЖДЁМ ИНИЦИАЛИЗАЦИЮ ROM (~200 кадров = 4 секунды) ═══
  Serial.println("⏳ Waiting for ROM initialization...");
  for (int i = 0; i < 200; i++) {
    spectrum->runForFrame(nullptr);  // Без звука во время загрузки
  }

  // ═══ 5. СИМУЛИРУЕМ НАЖАТИЕ "LOAD ""  ═══
  Serial.println("\n⌨️  Typing: LOAD \"\"");

  // J = LOAD
  spectrum->updateKey(SPECKEY_J, 1);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
  spectrum->updateKey(SPECKEY_J, 0);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);

  // SYMBOL SHIFT + P = "
  spectrum->updateKey(SPECKEY_SYMB, 1);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
  spectrum->updateKey(SPECKEY_P, 1);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
  spectrum->updateKey(SPECKEY_P, 0);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);

  // SYMBOL SHIFT + P = " (снова)
  spectrum->updateKey(SPECKEY_P, 1);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
  spectrum->updateKey(SPECKEY_P, 0);
  for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
  spectrum->updateKey(SPECKEY_SYMB, 0);
  
  // ENTER. The caller that traps the loader presses this itself: running
  // whole frames here would carry the machine straight past the entry
  // point it is waiting to catch.
  if (pressEnter) {
    spectrum->updateKey(SPECKEY_ENTER, 1);
    for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
    spectrum->updateKey(SPECKEY_ENTER, 0);
    for (int i = 0; i < 10; i++) spectrum->runForFrame(nullptr);
  }

  Serial.println("✅ LOAD \"\" entered!");
}

bool TAPLoader::playTape(const uint8_t* tapData, size_t fileSize, ZXSpectrum* spectrum,
                         RenderCallback renderCallback) {
  bootAndTypeLoad(spectrum);

  // ═══ 6. ЗАПУСКАЕМ TAPE EMULATION ═══
  Serial.println("\n📼 Starting tape emulation...");
  Serial.println("🎨 Loading screen будет построчно!");
  Serial.println("═══════════════════════════════════════════");
  
  TapeListener* listener = new TapeListener(spectrum, renderCallback);
  TapeCas tapCas;

  // The tape is only ever read, so a buffer in flash is as good as one
  // in RAM; the cast is what lets the built-in tape stay where it is.
  bool success = tapCas.loadTap(listener, const_cast<uint8_t*>(tapData), fileSize);

  delete listener;

  if (!success) {
    snprintf(lastError, sizeof(lastError), "Tape emulation failed");
    Serial.println("\n❌ TAPE EMULATION FAILED!");
    return false;
  }
  
  Serial.println("\n═══════════════════════════════════════════");
  Serial.println("✅ TAP FILE LOADED SUCCESSFULLY!");
  Serial.println("═══════════════════════════════════════════");
  
  return true;
}

// ═══════════════════════════════════════════════════════════
// INSTANT LOAD - ROM LOADER TRAP
// ═══════════════════════════════════════════════════════════
//
// The 48K ROM loads every tape block through one routine, LD_BYTES at
// 0x0556. Catching the machine there and handing over a whole block at
// once does in a moment what the tape does in a minute.
//
// On entry the ROM has set:
//   A  = the flag byte it expects, 0x00 for a header, 0xFF for data
//   IX = where the block goes
//   DE = how many bytes it wants
//   carry = set to load, clear to verify
// On return carry set means the block arrived intact.

#define ROM_LD_BYTES 0x0556

bool TAPLoader::loadTAPInstant(const char* filename, ZXSpectrum* spectrum,
                               RenderCallback renderCallback) {
  File file = SD.open(filename);
  if (!file) {
    snprintf(lastError, sizeof(lastError), "Failed to open file");
    return false;
  }

  size_t fileSize = file.size();
  uint8_t* tapData = (uint8_t*)malloc(fileSize);
  if (!tapData) {
    snprintf(lastError, sizeof(lastError), "Out of memory");
    file.close();
    return false;
  }

  file.read(tapData, fileSize);
  file.close();

  bool ok = trapTape(tapData, fileSize, spectrum, renderCallback);
  free(tapData);
  return ok;
}

bool TAPLoader::loadTAPInstantFromMemory(const uint8_t* data, size_t size, ZXSpectrum* spectrum,
                                         RenderCallback renderCallback) {
  if (data == nullptr || size == 0) {
    snprintf(lastError, sizeof(lastError), "Empty tape");
    return false;
  }
  return trapTape(data, size, spectrum, renderCallback);
}

bool TAPLoader::trapTape(const uint8_t* tapData, size_t fileSize, ZXSpectrum* spectrum,
                         RenderCallback renderCallback) {
  Serial.printf("📼 INSTANT LOAD: %d bytes\n", (int)fileSize);

  bootAndTypeLoad(spectrum, false);

  // Enter is pressed here and let go from inside the stepping loop, so
  // the machine is already being watched instruction by instruction when
  // it reaches the loader.
  spectrum->updateKey(SPECKEY_ENTER, 1);
  bool enterHeld = true;

  size_t tapePos = 0;
  int blocksLoaded = 0;
  int cyclesSinceInt = 0;
  int frames = 0;

  // Stepping one instruction at a time is the only way to catch the entry
  // point exactly. It lasts only until the tape runs out, and the frame
  // count caps it in case a tape never asks the ROM for its blocks.
  const int frameLimit = 3000;

  while (tapePos + 2 <= fileSize && frames < frameLimit) {
    if (spectrum->z80Regs->PC.W == ROM_LD_BYTES) {
      const uint16_t blockLen = tapData[tapePos] | (tapData[tapePos + 1] << 8);
      if (blockLen < 2 || tapePos + 2 + blockLen > fileSize) {
        snprintf(lastError, sizeof(lastError), "Truncated block");
        return false;
      }
      const uint8_t* block = tapData + tapePos + 2;
      tapePos += 2 + blockLen;

      const uint8_t wantedFlag = spectrum->z80Regs->AF.B.h;
      const uint16_t wanted = spectrum->z80Regs->DE.W;
      uint16_t dest = spectrum->z80Regs->IX.W;

      // The ROM asks for one kind of block at a time and skips the rest,
      // which is how "LOAD name" finds its own file on a tape of many.
      if (block[0] != wantedFlag) {
        Serial.printf("  skipping block flag 0x%02X, wanted 0x%02X\n", block[0], wantedFlag);
        continue;
      }

      const uint16_t available = blockLen - 2;  // less flag and checksum
      const uint16_t count = wanted < available ? wanted : available;
      for (uint16_t i = 0; i < count; i++) {
        spectrum->z80_poke(dest++, block[1 + i]);
      }

      spectrum->z80Regs->IX.W = dest;
      spectrum->z80Regs->DE.W = wanted - count;
      spectrum->z80Regs->AF.B.l |= C_FLAG;   // carry set: block accepted
      spectrum->z80Regs->AF.B.h = 0;

      // Return to whoever called the loader.
      spectrum->z80Regs->PC.B.l = spectrum->z80_peek(spectrum->z80Regs->SP.W);
      spectrum->z80Regs->PC.B.h = spectrum->z80_peek(spectrum->z80Regs->SP.W + 1);
      spectrum->z80Regs->SP.W += 2;

      blocksLoaded++;
      Serial.printf("  block %d: %d bytes to 0x%04X\n", blocksLoaded, count, dest - count);
      continue;
    }

    cyclesSinceInt += spectrum->runForCycles(1);

    // The frame interrupt still has to arrive on time or the BASIC that
    // runs between the blocks never gets anywhere.
    if (cyclesSinceInt >= 312 * 224) {
      cyclesSinceInt -= 312 * 224;
      frames++;
      if (spectrum->z80Regs->IFF1) spectrum->interrupt();

      // Long enough for the ROM's key scan to have seen it.
      if (enterHeld && frames >= 4) {
        spectrum->updateKey(SPECKEY_ENTER, 0);
        enterHeld = false;
      }

      // Instant is not instant enough to skip this: stepping instruction
      // by instruction takes seconds, and nothing else runs meanwhile.
      if (renderCallback && (frames % 25) == 0) {
        renderCallback();
      }
    }
  }

  if (blocksLoaded == 0) {
    snprintf(lastError, sizeof(lastError), "ROM loader never ran (PC 0x%04X)", spectrum->z80Regs->PC.W);
    return false;
  }

  Serial.printf("✅ INSTANT LOAD: %d blocks\n", blocksLoaded);
  return true;
}

// ═══════════════════════════════════════════════════════════
// СТАРЫЕ ФУНКЦИИ (не используются при tape emulation)
// ═══════════════════════════════════════════════════════════

bool TAPLoader::parseTAP(uint8_t* data, size_t fileSize) {
  // Не используется при tape emulation
  return false;
}

bool TAPLoader::loadBlock(TAPBlock& block, ZXSpectrum* spectrum) {
  // Не используется при tape emulation
  return false;
}
