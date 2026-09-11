#include "spectrum_mini.h"
#include "48k_rom.h"

// ZX Spectrum 16-color palette in RGB565 format
const uint16_t specpal565[16] = {
    0x0000, 0x1B00, 0x00B8, 0x17B8, 0xE005, 0xF705, 0xE0BD, 0x18C6,
    0x0000, 0x1F00, 0x00F8, 0x1FF8, 0xE007, 0xFF07, 0xE0FF, 0xFFFF
};

// Keyboard state (8 rows, 5 bits each)
// row0: SHIFT, Z, X, C, V
// row1: A, S, D, F, G
// row2: Q, W, E, R, T
// row3: 1, 2, 3, 4, 5
// row4: 0, 9, 8, 7, 6
// row5: P, O, I, U, Y
// row6: ENTER, L, K, J, H
// row7: SPACE, SYMB, M, N, B
uint8_t speckey[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Mapping from SpecKeys to keyboard matrix
const int key2specy[2][41] = {
    {0, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4,
     2, 2, 2, 2, 2, 5, 5, 5, 5, 5,
     1, 1, 1, 1, 1, 6, 6, 6, 6, 6,
     0, 0, 0, 0, 0, 7, 7, 7, 7, 7},
    {0, 0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE,
     0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE,
     0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE,
     0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE}
};

ZXSpectrum::ZXSpectrum() {
  z80Regs = (Z80Regs *)malloc(sizeof(Z80Regs));
  if (!z80Regs) {
    Serial.println("Failed to allocate Z80Regs");
  }
  z80Regs->userInfo = this;
}

// HUD snapshot (taken at mid-frame, not during INT!)
static uint16_t hud_pc = 0;
static uint8_t hud_im = 0;
static uint8_t hud_iff1 = 0;
static uint16_t hud_sp = 0;

void ZXSpectrum::reset() {
  Z80Reset(z80Regs);
  Z80FlagTables();
}

// Run emulation for one frame (312 lines × 224 tstates = 69888 tstates)
// ТАКЖЕ генерирует audioBuffer для звука!
int ZXSpectrum::runForFrame(uint16_t *accumOut) {
  static int totalFrames = 0;
  static bool im1Detected = false;
  
  const int LINES_PER_FRAME = 312;
  const int TSTATES_PER_LINE = 224;
  int cyclesExecuted = 0;
  
  // ═══ ЭМУЛЯЦИЯ КАДРА ПО ЛИНИЯМ (как ESP32-Rainbow) ═══
  for (int line = 0; line < LINES_PER_FRAME; line++) {
    // Сброс звукового аккумулятора перед каждой линией
    soundAccumulator = 0;
    
    // Запускаем Z80 на 224 t-states (1 линия)
    int used = runForCycles(TSTATES_PER_LINE);
    cyclesExecuted += used;
    
    // ═══ V3.111: СОХРАНЕНИЕ RAW ACCUMULATOR для ChatGPT beeper ═══
    // soundAccumulator = сколько t-states beeper был включен (0-224)
    if (accumOut) {
      accumOut[line] = soundAccumulator;  // ✅ Передаем RAW значение (0-224)!
    }
    
    // Snapshot в середине кадра (линия 156 из 312)
    if (line == 156) {
      hud_pc = z80Regs->PC.W;
      hud_im = z80Regs->IM;
      hud_iff1 = z80Regs->IFF1;
      hud_sp = z80Regs->SP.W;
    }
  }
  
  // ═══ ПРЕРЫВАНИЕ В КОНЦЕ КАДРА ═══
  // Every interrupt mode, not just IM 1: games routinely switch to IM 2
  // for their own handler, and one that never fires leaves them with a
  // running main loop and a keyboard nothing ever reads. Z80Interrupt
  // checks IFF1 itself and knows all three modes.
  if (z80Regs->IFF1) {
    interrupt();
  }
  
  // Проверяем переход на IM=1 (загрузка ROM завершена)
  if (!im1Detected && z80Regs->IM == 1) {
    im1Detected = true;
    Serial.printf("✅ Boot complete! IM=1 detected (frame #%d, %.2fs)\n", 
                  totalFrames, totalFrames / 50.0);
  }
  
  totalFrames++;
  return cyclesExecuted;
}

void ZXSpectrum::interrupt() {
  Z80Interrupt(z80Regs, 0x38);  // IM1: RST 38h
}

void ZXSpectrum::updateKey(SpecKeys key, uint8_t state) {
  if (key < SPECKEY_MAX_NORMAL) {
    if (state == 1) {
      // Key pressed: clear bit
      speckey[key2specy[0][key]] &= key2specy[1][key];
    } else {
      // Key released: set bit
      speckey[key2specy[0][key]] |= ((key2specy[1][key]) ^ 0xFF);
    }
  }
}

bool ZXSpectrum::init_48k() {
  // Without the 48K the machine has nothing to run in, and every peek and
  // poke past this point would go through a null pointer.
  if (!mem.isReady()) {
    Serial.println("No RAM: refusing to start");
    return false;
  }

  // ULA config for 48K
  hwopt.emulate_FF = 1;
  hwopt.ts_lebo = 24;    // left border t states
  hwopt.ts_grap = 128;   // graphic zone t states
  hwopt.ts_ribo = 24;    // right border t states
  hwopt.ts_hore = 48;    // horizontal retrace t states
  hwopt.ts_line = 224;   // total tstates per line
  hwopt.line_poin = 16;  // lines in retrace post interrupt
  hwopt.line_upbo = 48;  // lines of upper border
  hwopt.line_grap = 192; // lines of graphic zone
  hwopt.line_bobo = 56;  // lines of bottom border
  hwopt.line_retr = 8;   // lines of retrace
  hwopt.hw_model = SPECMDL_48K;
  hwopt.BorderColor = 7;
  hwopt.portFF = 0xFF;
  hwopt.SoundBits = 0;

  // Load 48K ROM
  mem.loadRom(gb_rom_0_sinclair_48k, 16384);

  // CRITICAL: Verify ROM signature
  uint8_t b0 = mem.peek(0x0000);
  uint8_t b1 = mem.peek(0x0001);
  uint8_t b38 = mem.peek(0x0038);
  uint8_t b39 = mem.peek(0x0039);
  uint8_t b3A = mem.peek(0x003A);
  
  // Check ROM start (must be DI; XOR A)
  if (b0 != 0xF3 || b1 != 0xAF) {
    Serial.println("🔴 FATAL: ROM signature mismatch!");
    Serial.printf("   Expected F3 AF @ 0x0000, got %02X %02X\n", b0, b1);
    return false;
  }
  
  // Check IM1 vector @ 0x0038 (two known variants)
  bool rom_im1_is_jp = (b38 == 0xC3 && b39 == 0xCB && b3A == 0x15);    // JP 0x15CB
  bool rom_im1_is_push = (b38 == 0xF5 && b39 == 0xE5 && b3A == 0x2A);  // PUSH AF; PUSH HL; LD HL,...
  
  const char* variant = "unknown";
  if (rom_im1_is_jp) variant = "classic";
  else if (rom_im1_is_push) variant = "inline";
  
  Serial.printf("✅ ROM: ZX Spectrum 48K (%s)\n", variant);

  return true;
}

void ZXSpectrum::reset_spectrum() {
  Z80Reset(z80Regs);
}

// HUD getters (return mid-frame snapshot, not INT state!)
uint16_t ZXSpectrum::getHudPC() const { return hud_pc; }
uint8_t ZXSpectrum::getHudIM() const { return hud_im; }
uint8_t ZXSpectrum::getHudIFF1() const { return hud_iff1; }
uint16_t ZXSpectrum::getHudSP() const { return hud_sp; }

