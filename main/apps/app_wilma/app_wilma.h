/*
 * Spoken phrase recognition, of the only kind that fits here.
 *
 * Not speech recognition: this board has no PSRAM, and Espressif's
 * WakeNet wants megabytes of it plus a model partition to match. What
 * does fit is speaker-dependent template matching -- you record a
 * phrase a few times, and it recognises that phrase, in your voice,
 * roughly where you recorded it. A different speaker, a noisy room or a
 * different intonation will all hurt it.
 *
 * How it works, in three steps:
 *
 *  - Frames of 16ms at 16kHz, and for each one the energy in a handful
 *    of frequency bands. Those come from Goertzel filters rather than an
 *    FFT: one filter per band costs a multiply-accumulate per sample,
 *    which for twelve bands is a few hundred thousand operations a
 *    second, and it saves carrying a transform and its tables.
 *
 *  - Log of each band, then the frame's mean subtracted from it. That
 *    last step is what makes the match survive you speaking louder or
 *    standing further away: it throws away the overall level and keeps
 *    the shape of the spectrum.
 *
 *  - Dynamic time warping against the stored takes, which is what
 *    allows a phrase said slowly to match one said quickly. The best
 *    distance across all takes of all phrases wins, if it beats a
 *    threshold.
 *
 * Templates live in RAM. This is a test bench: they are recorded at the
 * start of a session and lost on reboot, which keeps the storage
 * question out of the way until the recognition itself is worth
 * keeping.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>
#include <vector>

class AppWilma : public mooncake::AppAbility {
public:
    AppWilma();
    ~AppWilma();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static constexpr int kPhrases     = 3;
    static constexpr int kTakes       = 3;
    static constexpr int kBands       = 10;
    static constexpr int kFrameLen    = 256;  // 32ms at 8kHz
    static constexpr int kHop         = 128;  // half a frame
    static constexpr int kMaxFrames   = 120;  // just under two seconds
    static constexpr int kSampleRate  = 8000;

    /*
     * The whole phrase is recorded in one go, and this is how long that
     * window is. Asking the microphone for a frame at a time lost the
     * audio between requests and could block for ever inside record();
     * one recording of 48 KB costs memory instead, which this board can
     * spare more easily than it can spare a hang.
     */
    static constexpr int kWindowMs   = 1500;
    static constexpr int kMaxSamples = kSampleRate * kWindowMs / 1000;

    static constexpr int kChunkSamples = 1024;  // 128ms at 8kHz
    static constexpr int kRingFrames   = 160;   // about 2.5s of history
    static constexpr int kRunUpFrames  = 4;     // kept from before speech starts
    enum Screen_t : std::uint8_t {
        SCREEN_MENU,
        SCREEN_RECORD,
        SCREEN_LISTEN,
    };

    /* One take: a sequence of band-energy frames, quantised to a byte
     * each. Twelve bytes a frame and at most 120 frames is 1.4 KB, so
     * nine takes cost about 13 KB -- affordable on a board where the
     * emulator wants 48 KB in one piece. */
    struct Template_t {
        std::uint8_t frames[kMaxFrames][kBands];
        int length = 0;
    };

    /*
     * The takes and the scratch, held only while the application is
     * open.
     *
     * As plain members they were about 15 KB carried from boot to
     * shutdown, because the launcher constructs every application
     * object at startup whether or not anyone opens it -- and that was
     * enough to stop the Record application allocating its buffer,
     * which rebooted the board the moment it was opened. An application
     * that is not running has no business holding memory.
     */
    struct Store_t {
        Template_t templates[kPhrases][kTakes];
        Template_t scratch;

        /* The rolling history the stream writes into. A phrase is
         * recognised after it has been said, so its frames have to
         * still be here when the end of it is detected. */
        std::uint8_t ring[kRingFrames][kBands];
        std::uint16_t ring_level[kRingFrames];
        int ring_head  = 0;
        int ring_count = 0;
    };
    Store_t* _store = nullptr;

    int _take_count[kPhrases] = {0, 0, 0};

    /*
     * Where the next take of each phrase goes, counted without a
     * ceiling.
     *
     * The slot used to come from _take_count, which stops at kTakes --
     * so once three had been recorded the modulo froze at zero and
     * every later attempt overwrote the same one. The other two were
     * kept for ever and could not be replaced, which is the opposite of
     * the rotation intended. This counter keeps going, so recording
     * really does keep the most recent three.
     */
    int _take_next[kPhrases] = {0, 0, 0};

    std::uint8_t _screen = SCREEN_MENU;

    /*
     * What is actually on the display, as against what _screen says
     * ought to be. Esc changes the screen from inside a keyboard
     * callback and nothing redrew afterwards, so leaving the listening
     * screen left its picture up: the menu was live underneath, and
     * pressing Esc again looked like it did nothing at all. onRunning
     * reconciles the two.
     */
    std::uint8_t _drawn_screen = 0xFF;

    /* Whether the next draw starts from a blank panel. Set when the
     * screen changes, cleared by whoever wipes it. */
    bool _needs_clear    = true;
    int _selected        = 0;
    int _key_slot        = -1;
    int _key_raw_slot    = -1;
    bool _close_requested = false;
    bool _mic_ready       = false;

    /*
     * Whether a throwaway reading has been taken since the microphone
     * was started. The first record() after begin() comes back at once
     * off DMA buffers that hold nothing yet, so without this the first
     * take of a session was always lost -- it looked like the button
     * had missed the press.
     */
    bool _mic_primed      = false;

    /*
     * Set by Esc to break out of a capture in progress.
     *
     * Capturing blocks the main loop, and the main loop is the only
     * thing that polls the keyboard, so a capture that runs to its own
     * end is a capture nobody can interrupt -- listening could only be
     * left by resetting the board. capture() now pumps the keyboard
     * itself and watches this.
     */
    bool _abort = false;

    /* A record asked for by the keyboard callback and carried out by
     * onRunning, so the capture never runs on the callback's stack. */
    bool _want_record = false;

    /* What the last listen decided, kept for the display. */
    int _last_phrase   = -1;
    int _last_distance = 0;
    int _last_level    = 0;

    /*
     * The phrases that have to be said, in this order, and how far
     * along we are. Anything else heard is ignored rather than treated
     * as a mistake: a stray word in the room should not undo progress.
     */
    static constexpr int kSequence[] = {2, 0, 1};  // Yes Master, Wilma, I'm home
    static constexpr int kSequenceLen = 3;

    /* How long a part-finished sequence stays valid. */
    static constexpr std::uint32_t kSequenceTimeoutMs = 15000;

    int _seq_step             = 0;
    bool _seq_done            = false;
    std::uint32_t _seq_last_ms = 0;

    /*
     * How close a match has to be before it counts, learned from the
     * training rather than picked.
     *
     * Guessing an absolute number is the mistake already made twice
     * here with the energy gate. The takes of one phrase differ from
     * each other by some amount; that amount is the natural scale of
     * "the same thing said twice", so the limit is derived from it.
     */
    int _accept_limit = 0;

    /* Measured at the start of every capture, and the two thresholds
     * derived from it. Members rather than constants because the right
     * value depends on the room and on the microphone gain, which is
     * exactly what a fixed number got wrong. */
    int _noise   = 0;
    int _gate    = 0;
    int _silence = 0;

    std::int16_t* _audio = nullptr;

    void draw();
    void draw_menu();
    void draw_record();
    void draw_listen();

    /** @brief Wipe the panel, but only where a whole screen changed. */
    void clear_if_needed();

    /** @brief The live meter shown while a capture waits for speech. */
    void draw_capture(bool armed);

    /** @brief Record a window, find the phrase in it, and frame it. */
    int capture(Template_t& out);

    /** @brief Distance between a fresh capture and one stored take. */
    int compare(const Template_t& a, const Template_t& b) const;

    /*
     * Continuous listening.
     *
     * Recording a window at a time meant deaf gaps between windows, so
     * a phrase only registered if it was said after the prompt
     * appeared -- which is no use to someone walking into a room. Two
     * chunks are kept queued at the microphone at all times (its queue
     * holds exactly two), so audio never stops arriving; each chunk is
     * cut into frames as it lands, and speech is found in the stream by
     * its own energy rather than by a window someone has to wait for.
     */
    int _queued = 0;  // chunks currently with the microphone
    int _oldest = 0;  // which of the two completes next
    int _phase  = 0;  // where the next frame starts in the carried samples

    bool _streaming = false;
    bool _speech    = false;
    int _quiet_run  = 0;
    int _seg_len    = 0;

    /** @brief Start and stop the continuous stream. */
    bool stream_start();
    void stream_stop();

    /** @brief Take whatever the microphone has ready; returns at once. */
    void stream_poll();

    /** @brief One frame of the stream: level, bands, and the speech detector. */
    void stream_frame(const std::int16_t* frame);

    /** @brief A finished utterance, matched against the takes. */
    void segment_done();

    /** @brief One turn of the main loop from inside a wait; true to stop. */
    bool pump();

    /** @brief Work out how close a match must be, from the takes themselves. */
    void compute_accept_limit();

    /*
     * The takes, on the SD card rather than in NVS.
     *
     * A trained set is a few kilobytes, and this board's NVS partition
     * is 16 KB shared with the WiFi credentials, the jobs state and the
     * irrigation settings. Crowding it to save training data would risk
     * the things that matter more.
     */
    bool save_takes();
    bool load_takes();

    /* What the last save or load did, for the menu to show. Training
     * that is not being kept is worth saying out loud, since the whole
     * point of it is not having to do it again. */
    const char* _store_note = "";

    void do_record(int phrase);

    /** @brief Judge an utterance and advance the sequence; true if accepted. */
    bool decide(Template_t& heard);

    /** @brief Judge one stretch of the ring; true if it was accepted. */
    bool try_segment(int start, int len);
    void handle_char(char ch);
    void show(std::uint8_t screen);
};
