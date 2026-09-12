/*
 * SPDX-License-Identifier: MIT
 */
#include "app_wilma.h"

#include "assets/wilma_big.h"
#include "assets/wilma_small.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal.h>
#include <mooncake_log.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

using namespace mooncake;

namespace {

const char* const kPhraseNames[] = {"Wilma", "I'm home", "Yes Master"};

/*
 * Band centres, in hertz, spread the way hearing is: close together low
 * down where vowels differ, wider apart high up. Twelve of them is
 * enough to tell these three phrases apart without carrying a whole
 * spectrum around.
 */
const float kBandHz[] = {300, 420, 570, 760, 1000, 1300, 1650, 2100, 2650, 3300};

/* Frames of silence that end a phrase. Twelve of them at 8ms of hop is
 * about a tenth of a second, short enough not to feel laggy and long
 * enough to sit through the stop in "Yes Master". */
constexpr int kTailFrames = 12;

/* Anything at or above this came back from a comparison that found no
 * path at all, rather than one that found a poor one. */
constexpr int kNoMatch = 1000000;

/*
 * One Goertzel filter, run over a frame, returning the band's power.
 *
 * The cheapest way to ask "how much energy is at this frequency" when
 * only a few frequencies are wanted: two multiply-accumulates a sample
 * and no transform, no tables, no buffer of complex numbers.
 */
float goertzel(const std::int16_t* samples, int len, float hz, int rate)
{
    const float k     = 2.0f * (float)M_PI * hz / (float)rate;
    const float coeff = 2.0f * cosf(k);

    float s1 = 0.0f;
    float s2 = 0.0f;
    for (int i = 0; i < len; i++) {
        /* A Hann window, so the band does not smear into its
         * neighbours at the frame edges. */
        const float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(len - 1));
        const float s = (float)samples[i] * w + coeff * s1 - s2;
        s2            = s1;
        s1            = s;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

}  // namespace

AppWilma::AppWilma()
{
    setAppInfo().name     = "Wilma";
    setAppInfo().userData = new AppIcon_t(image_data_wilma_big, image_data_wilma_small);
}

AppWilma::~AppWilma()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

/* -------------------------------------------------------------------------- */
/*                                  Listening                                  */
/* -------------------------------------------------------------------------- */

/*
 * One recording, whole, and the analysis afterwards.
 *
 * The first version asked the microphone for one 16ms frame at a time,
 * a hundred and twenty times a phrase. That was wrong twice over. It
 * dropped whatever was said between two requests, so the phrase arrived
 * with holes in it -- and M5Unified's record() waits, with no timeout
 * and no way out, for the previous buffer to drain:
 *
 *     while (_rec_info[_rec_flip].length) { xSemaphoreTake(...); }
 *
 * which froze the screen with the keyboard unread. Putting a deadline
 * on the *other* wait, isRecording(), changed nothing, because that was
 * never the wait that blocked.
 *
 * So: ask once for the whole window, wait for that single recording
 * with a deadline, then do every bit of the work on a buffer that is
 * already in hand and cannot stall. No gaps, no blocking, and the
 * thresholds are chosen knowing the whole phrase rather than guessed
 * from the frames seen so far.
 */
int AppWilma::capture(Template_t& out)
{
    out.length  = 0;
    _abort      = false;
    _last_level = 0;

    if (_audio == nullptr || _store == nullptr) {
        return 0;
    }

    /*
     * A microphone that failed to start once stayed failed for the life
     * of the application, and every capture then returned instantly
     * with nothing said about why. Retry it here instead: the usual
     * reason is that the speaker still held the shared I2S peripheral,
     * which is a state that passes.
     */
    if (!_mic_ready) {
        GetHAL().speaker.end();
        _mic_ready = GetHAL().mic.begin();
    _mic_primed = false;
        mclog::tagWarn(getAppInfo().name, "microphone was not ready, restart {}",
                       _mic_ready ? "succeeded" : "failed");
        if (!_mic_ready) {
            return 0;
        }
    }

    /*
     * A throwaway reading first, if the microphone has just started.
     *
     * The first record() after begin() returns almost at once, off DMA
     * buffers that have not been filled yet, so the first take of every
     * session came back empty -- which read as the button having missed
     * the press rather than as a recording that was never made. Spend a
     * tenth of a second on nothing, once, and the take that follows is
     * a real one.
     */
    if (!_mic_primed) {
        if (GetHAL().mic.record(_audio, kSampleRate / 10, kSampleRate)) {
            const std::int64_t warm = esp_timer_get_time() + 500000;
            while (GetHAL().mic.isRecording() && esp_timer_get_time() < warm) {
                GetHAL().delay(2);
            }
        }
        _mic_primed = true;
    }

    draw_capture(false);

    /*
     * Never ask while one is still running. record() waits for the
     * previous buffer to drain with no timeout and no way out, so
     * calling it on a busy microphone is how the screen froze; this
     * turns that into a skipped attempt instead of a dead board.
     */
    if (GetHAL().mic.isRecording()) {
        mclog::tagWarn(getAppInfo().name, "microphone still busy, skipping");
        return 0;
    }

    if (!GetHAL().mic.record(_audio, kMaxSamples, kSampleRate)) {
        mclog::tagWarn(getAppInfo().name, "microphone refused the recording");
        return 0;
    }

    /* The window is kWindowMs long; well past double that, something is
     * wrong with the microphone rather than with the speaker. */
    const std::int64_t deadline = esp_timer_get_time() + (kWindowMs * 3000);
    while (GetHAL().mic.isRecording()) {
        /* The main loop polls the keyboard, and this function is
         * standing on it, so it does that job itself while it waits --
         * otherwise Esc is never seen. */
        if (pump()) {
            GetHAL().mic.end();
            _mic_ready = GetHAL().mic.begin();
    _mic_primed = false;
            return 0;
        }
        if (esp_timer_get_time() > deadline) {
            mclog::tagError(getAppInfo().name, "recording never finished");
            return 0;
        }
        GetHAL().delay(2);
    }

    draw_capture(true);

    /*
     * Levels first, one per hop, so the whole phrase can be looked at
     * before deciding anything about it.
     */
    const int hops = (kMaxSamples - kFrameLen) / kHop;
    static std::uint16_t levels[(kMaxSamples - kFrameLen) / kHop + 1];

    int quietest = INT32_MAX;
    int loudest  = 0;
    for (int h = 0; h < hops; h++) {
        const std::int16_t* frame = _audio + (h * kHop);
        int sum                   = 0;
        for (int i = 0; i < kFrameLen; i++) {
            sum += std::abs(frame[i]);
        }
        const int level = sum / kFrameLen;
        levels[h]       = (std::uint16_t)(level > 65535 ? 65535 : level);

        if (level < quietest) quietest = level;
        if (level > loudest) loudest = level;
    }

    /*
     * The quietest moment in the window is the room; the thresholds sit
     * above it. Taking the floor from the recording itself, rather than
     * from a separate sample beforehand, means it cannot be measured
     * while someone is already talking.
     */
    _noise   = (quietest == INT32_MAX) ? 0 : quietest;
    _gate    = _noise * 3 + 120;
    _silence = _noise * 2 + 60;

    _last_level = loudest;

    /* Where the speech starts and where it stops. */
    int first = -1;
    int last  = -1;
    for (int h = 0; h < hops; h++) {
        if (levels[h] >= _gate) {
            if (first < 0) first = h;
            last = h;
        }
    }

    mclog::tagInfo(getAppInfo().name, "noise {}, peak {}, gate {}, speech {}..{} of {}", _noise,
                   loudest, _gate, first, last, hops);

    if (first < 0) {
        return 0;
    }

    /* A little of the run-up, because the start of a word carries much
     * of what separates one phrase from another, and the gate opens a
     * fraction late by construction. */
    first -= 2;
    if (first < 0) first = 0;

    /* And a little of the tail, for the stop in "Yes Master". */
    last += 2;
    if (last >= hops) last = hops - 1;

    for (int h = first; h <= last && out.length < kMaxFrames; h++) {
        const std::int16_t* frame = _audio + (h * kHop);

        /* Bands, logged, then flattened by removing the frame's own
         * mean -- which is what makes a quiet "Wilma" match a loud one,
         * since only the shape survives. */
        float logs[kBands];
        float mean = 0.0f;
        for (int b = 0; b < kBands; b++) {
            const float p = goertzel(frame, kFrameLen, kBandHz[b], kSampleRate);
            logs[b]       = logf(p + 1.0f);
            mean += logs[b];
        }
        mean /= (float)kBands;

        for (int b = 0; b < kBands; b++) {
            /* Scaled into a byte: the useful spread after mean removal
             * is a handful of log units either way. */
            float v = (logs[b] - mean) * 16.0f + 128.0f;
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            out.frames[out.length][b] = (std::uint8_t)v;
        }
        out.length++;

        /* Long phrases are subsampled rather than truncated: losing the
         * end of "Yes Master" would matter more than losing detail. */
        if (out.length == kMaxFrames && h < last) {
            mclog::tagWarn(getAppInfo().name, "phrase longer than the template, truncated");
        }
    }

    return out.length;
}

/*
 * Dynamic time warping, with a band around the diagonal.
 *
 * Straight frame-by-frame comparison fails the moment a phrase is said
 * a little faster, which is every time. Warping lets one sequence
 * stretch against the other; the band stops it stretching absurdly and
 * keeps the cost down to a few thousand operations.
 */
int AppWilma::compare(const Template_t& a, const Template_t& b) const
{
    if (a.length == 0 || b.length == 0) {
        return INT32_MAX;
    }

    static std::uint32_t prev[kMaxFrames + 1];
    static std::uint32_t curr[kMaxFrames + 1];

    /*
     * The band has to be wide enough to reach the far corner.
     *
     * A fixed 24 could not: with takes of 86 and 39 frames, no path
     * within 24 of the diagonal reaches the end, so the whole
     * comparison returned its "no path" sentinel of about twelve
     * million. That sentinel then went into the acceptance limit as the
     * worst pair and made the limit meaningless -- which left the
     * margin rule deciding everything on its own, and it rejected
     * nearly half of what was said correctly. The band must never be
     * narrower than the difference in length, plus a little to warp
     * within.
     */
    const int diff = (a.length > b.length) ? (a.length - b.length) : (b.length - a.length);
    const int band = (diff + 8 > 24) ? (diff + 8) : 24;

    for (int j = 0; j <= b.length; j++) {
        prev[j] = UINT32_MAX / 4;
    }
    prev[0] = 0;

    for (int i = 1; i <= a.length; i++) {
        for (int j = 0; j <= b.length; j++) {
            curr[j] = UINT32_MAX / 4;
        }

        int lo = i - band;
        int hi = i + band;
        if (lo < 1) lo = 1;
        if (hi > b.length) hi = b.length;

        for (int j = lo; j <= hi; j++) {
            std::uint32_t cost = 0;
            for (int k = 0; k < kBands; k++) {
                const int d = (int)a.frames[i - 1][k] - (int)b.frames[j - 1][k];
                cost += (std::uint32_t)(d < 0 ? -d : d);
            }

            std::uint32_t best = prev[j];
            if (curr[j - 1] < best) best = curr[j - 1];
            if (prev[j - 1] < best) best = prev[j - 1];
            curr[j] = cost + best;
        }
        std::memcpy(prev, curr, sizeof(std::uint32_t) * (b.length + 1));
    }

    /* Divided by the path length, so a long phrase is not penalised for
     * being long. */
    return (int)(prev[b.length] / (std::uint32_t)(a.length + b.length));
}

void AppWilma::do_record(int phrase)
{
    show(SCREEN_RECORD);
    _selected = phrase;
    draw();

    if (capture(_store->scratch) < 8) {
        if (!_abort) {
            mclog::tagWarn(getAppInfo().name, "nothing captured, loudest frame {} (gate {})",
                           _last_level, _gate);
        }
        show(SCREEN_MENU);
        return;
    }

    const int slot = _take_next[phrase] % kTakes;
    _store->templates[phrase][slot] = _store->scratch;
    _take_next[phrase]++;
    if (_take_count[phrase] < kTakes) {
        _take_count[phrase]++;
    }

    mclog::tagInfo(getAppInfo().name, "\"{}\" take {}: {} frames", kPhraseNames[phrase],
                   _take_count[phrase], _store->scratch.length);

    /* Saved as each take is made, not on the way out: a reset or a flat
     * battery in the middle of a session should not cost the training
     * that has already been done. */
    compute_accept_limit();
    save_takes();

    show(SCREEN_MENU);
}

/* -------------------------------------------------------------------------- */
/*                             Continuous listening                            */
/* -------------------------------------------------------------------------- */

/*
 * The three regions of the recording buffer, which is far larger than
 * the stream needs: two chunks the microphone fills in turn, and the
 * carry that lets a frame straddle the join between them.
 */
#define CHUNK(i) (_audio + (i) * kChunkSamples)
#define CARRY    (_audio + 2 * kChunkSamples)

bool AppWilma::stream_start()
{
    if (_audio == nullptr || _store == nullptr) {
        return false;
    }
    if (_streaming) {
        return true;
    }

    if (!_mic_ready) {
        GetHAL().speaker.end();
        _mic_ready = GetHAL().mic.begin();
        _mic_primed = false;
        if (!_mic_ready) {
            _store_note = "the microphone would not start";
            return false;
        }
    }

    /* Everything the detector carries from frame to frame starts
     * clean, or the tail of the last session shows up as speech. */
    _queued     = 0;
    _oldest     = 0;
    _phase      = 0;
    _speech     = false;
    _quiet_run  = 0;
    _seg_len    = 0;
    _noise      = 0;
    _store->ring_head  = 0;
    _store->ring_count = 0;
    std::memset(CARRY, 0, kFrameLen * sizeof(std::int16_t));

    while (GetHAL().mic.isRecording()) {
        GetHAL().delay(1);
    }

    /* Both slots filled straight away: the queue holds two, and
     * keeping it full is what removes the gaps. */
    for (int i = 0; i < 2; i++) {
        if (!GetHAL().mic.record(CHUNK(i), kChunkSamples, kSampleRate)) {
            _store_note = "the microphone refused to stream";
            return false;
        }
        _queued++;
    }

    _streaming = true;
    mclog::tagInfo(getAppInfo().name, "listening continuously");
    return true;
}

void AppWilma::stream_stop()
{
    if (!_streaming) {
        return;
    }
    _streaming = false;

    /* Let the queued chunks finish rather than tearing the driver
     * down underneath them. */
    const std::int64_t until = esp_timer_get_time() + 500000;
    while (GetHAL().mic.isRecording() && esp_timer_get_time() < until) {
        GetHAL().delay(2);
    }
    _queued = 0;
}

/*
 * Whatever has arrived, cut into frames, and the queue topped back up.
 *
 * This must return quickly: it is called from onRunning, and the whole
 * point of the rewrite is that nothing here blocks the main loop.
 */
void AppWilma::stream_poll()
{
    if (!_streaming || _store == nullptr) {
        return;
    }

    /* isRecording() counts what is still outstanding, so anything
     * missing from our own count has been filled. */
    while ((int)GetHAL().mic.isRecording() < _queued) {
        const std::int16_t* chunk = CHUNK(_oldest);

        /*
         * Frames are cut across the join, not within one chunk: a
         * phrase does not pause at a buffer boundary. The carry holds
         * the last frame's worth of the previous chunk, and _phase
         * remembers where the next frame begins.
         */
        for (int s = _phase; s + kFrameLen <= kFrameLen + kChunkSamples; s += kHop) {
            if (s + kFrameLen <= kFrameLen) {
                stream_frame(CARRY + s);
            } else if (s >= kFrameLen) {
                stream_frame(chunk + (s - kFrameLen));
            } else {
                /* Straddling: assembled once into the tail of the
                 * carry, which is about to be overwritten anyway. */
                static std::int16_t joined[kFrameLen];
                const int from_carry = kFrameLen - s;
                std::memcpy(joined, CARRY + s, from_carry * sizeof(std::int16_t));
                std::memcpy(joined + from_carry, chunk,
                            (kFrameLen - from_carry) * sizeof(std::int16_t));
                stream_frame(joined);
            }
        }

        /* Carry the last frame's worth forward, and move the phase
         * into the new coordinates. */
        int s = _phase;
        while (s + kFrameLen <= kFrameLen + kChunkSamples) {
            s += kHop;
        }
        _phase = s - kChunkSamples;
        if (_phase < 0) _phase = 0;

        std::memcpy(CARRY, chunk + kChunkSamples - kFrameLen,
                    kFrameLen * sizeof(std::int16_t));

        _oldest = (_oldest + 1) & 1;
        _queued--;
    }

    while (_queued < 2) {
        const int slot = (_oldest + _queued) & 1;
        if (!GetHAL().mic.record(CHUNK(slot), kChunkSamples, kSampleRate)) {
            break;
        }
        _queued++;
    }
}

void AppWilma::stream_frame(const std::int16_t* frame)
{
    int sum = 0;
    for (int i = 0; i < kFrameLen; i++) {
        sum += std::abs(frame[i]);
    }
    const int level = sum / kFrameLen;
    _last_level     = level;

    /*
     * The noise floor follows the room down at once and back up only
     * slowly, and never while someone is talking -- otherwise a long
     * phrase would raise the floor above itself and cut its own end
     * off.
     */
    if (_noise == 0 || level < _noise) {
        _noise = level;
    } else if (!_speech) {
        _noise += (level - _noise) / 64 + 1;
    }

    _gate    = _noise * 3 + 120;
    _silence = _noise * 2 + 60;

    /* Bands into the ring, whether or not this is speech: when the
     * start of a phrase is detected the run-up is already behind us. */
    float logs[kBands];
    float mean = 0.0f;
    for (int b = 0; b < kBands; b++) {
        const float p = goertzel(frame, kFrameLen, kBandHz[b], kSampleRate);
        logs[b]       = logf(p + 1.0f);
        mean += logs[b];
    }
    mean /= (float)kBands;

    const int slot = _store->ring_head;
    for (int b = 0; b < kBands; b++) {
        float v = (logs[b] - mean) * 16.0f + 128.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 255.0f) v = 255.0f;
        _store->ring[slot][b] = (std::uint8_t)v;
    }
    _store->ring_level[slot] = (std::uint16_t)(level > 65535 ? 65535 : level);

    _store->ring_head = (slot + 1) % kRingFrames;
    if (_store->ring_count < kRingFrames) {
        _store->ring_count++;
    }

    if (!_speech) {
        if (level >= _gate) {
            _speech    = true;
            _quiet_run = 0;
            /* Counted back from before the trigger, because the gate
             * opens a fraction into the first sound. */
            _seg_len = kRunUpFrames;
        }
        return;
    }

    _seg_len++;
    _quiet_run = (level < _silence) ? (_quiet_run + 1) : 0;

    if (_quiet_run >= kTailFrames || _seg_len >= kMaxFrames) {
        segment_done();
        _speech    = false;
        _quiet_run = 0;
        _seg_len   = 0;
    }
}

/*
 * One stretch of the ring, copied out and judged. True if it was
 * accepted as one of the phrases.
 */
bool AppWilma::try_segment(int start, int len)
{
    if (len < 8) {
        return false;
    }
    if (len > kMaxFrames) {
        len = kMaxFrames;
    }

    for (int i = 0; i < len; i++) {
        const int slot = (start + i) % kRingFrames;
        std::memcpy(_store->scratch.frames[i], _store->ring[slot], kBands);
    }
    _store->scratch.length = len;

    return decide(_store->scratch);
}

/*
 * An utterance has ended: judge it, and if it looks like two phrases
 * run together, judge the halves.
 *
 * An utterance ends after about two tenths of a second of quiet, which
 * forces a pause between phrases that nobody speaking naturally would
 * make -- run two together and the segment contains both, matches
 * nothing, and is thrown away. Lengthening the silence needed would
 * only make that worse. So a segment far longer than any recorded take
 * is cut at its quietest interior moment, which is where the join
 * between two phrases is, and each half is offered on its own.
 */
void AppWilma::segment_done()
{
    /* The trailing silence that ended it is not part of it. */
    int len = _seg_len - kTailFrames;
    if (len < 8) {
        return;
    }
    if (len > kMaxFrames) {
        len = kMaxFrames;
    }

    const int start = (_store->ring_head - _seg_len + kRingFrames * 2) % kRingFrames;

    if (try_segment(start, len)) {
        return;
    }

    /* The longest phrase ever recorded sets what "too long" means:
     * anything half again beyond it is more than one thing said. */
    int longest = 0;
    for (int p = 0; p < kPhrases; p++) {
        for (int t = 0; t < _take_count[p]; t++) {
            if (_store->templates[p][t].length > longest) {
                longest = _store->templates[p][t].length;
            }
        }
    }
    if (longest == 0 || len < longest * 3 / 2) {
        return;
    }

    /* The quietest frame in the middle of it, kept away from both ends
     * so the split cannot produce a scrap. */
    int cut  = -1;
    int best = INT32_MAX;
    for (int i = len / 4; i < (len * 3) / 4; i++) {
        const int slot = (start + i) % kRingFrames;
        if (_store->ring_level[slot] < best) {
            best = _store->ring_level[slot];
            cut  = i;
        }
    }
    if (cut < 8 || len - cut < 8) {
        return;
    }

    mclog::tagInfo(getAppInfo().name, "segment of {} frames split at {}", len, cut);

    /* In the order spoken, so a sequence survives being run together. */
    try_segment(start, cut);
    try_segment((start + cut) % kRingFrames, len - cut);
}

/*
 * The acceptance limit, measured rather than chosen.
 *
 * Two takes of the same phrase, by the same person, are as close as
 * this method ever gets -- so the spread among them is the scale of
 * "the same thing said twice". Half again on top of the worst of them
 * leaves room for a third saying without opening the door to any word
 * at all, which is what a picked constant did.
 *
 * With only one take of everything there is nothing to measure, and the
 * limit stays zero: the margin rule in do_listen then carries the
 * decision on its own.
 */
void AppWilma::compute_accept_limit()
{
    if (_store == nullptr) {
        return;
    }

    int worst = 0;
    int pairs = 0;

    for (int p = 0; p < kPhrases; p++) {
        for (int a = 0; a < _take_count[p]; a++) {
            for (int b = a + 1; b < _take_count[p]; b++) {
                const int d = compare(_store->templates[p][a], _store->templates[p][b]);

                /* A comparison that found no path says nothing about
                 * how alike two takes are, and must not be allowed to
                 * set the limit. With the band fixed this should no
                 * longer happen; the guard stays because when it did
                 * happen the symptom was a limit of twelve million and
                 * no sign of anything wrong. */
                if (d >= kNoMatch) {
                    mclog::tagWarn(getAppInfo().name, "takes {} of \"{}\" would not align", a,
                                   kPhraseNames[p]);
                    continue;
                }

                if (d > worst) {
                    worst = d;
                }
                pairs++;
            }
        }
    }

    _accept_limit = (pairs > 0) ? (worst * 3 / 2) : 0;
    mclog::tagInfo(getAppInfo().name, "accept limit {} from {} pairs", _accept_limit, pairs);
}

/*
 * One turn of the main loop's work, done from inside a blocking wait,
 * and the answer to "should I stop?".
 *
 * Everything that waits here goes through this. The home button is why:
 * wasClicked() is latched for exactly the one update in which the click
 * happened, and capturing calls update() hundreds of times, so a click
 * was being swallowed inside these loops and never reached the check in
 * onRunning. Pressing home during listening did nothing at all.
 */
bool AppWilma::pump()
{
    GetHAL().update();

    if (GetHAL().homeButton.wasClicked()) {
        _close_requested = true;
        _abort           = true;
    }

    /* The corner of the band, polled here for the same reason the home
     * button is: recording and listening each run a loop of their own,
     * and a way out that worked only between them would not work while
     * either was going. */
    if (app_header::back_pressed(GetHAL().display.width(), 0, 0)) {
        _close_requested = true;
        _abort           = true;
    }
    return _abort;
}

/* -------------------------------------------------------------------------- */
/*                                   Storage                                   */
/* -------------------------------------------------------------------------- */

namespace {

const char* const kTakesPath = "/sdcard/wilma.dat";

/* Bumped if the layout below changes, so an old file is ignored rather
 * than read as nonsense. The band count and the phrase count are
 * written out with it: both change the shape of a take, and a file
 * recorded under different ones cannot be compared against fresh
 * captures. */
constexpr std::uint8_t kFileMagic[4] = {'W', 'I', 'L', '1'};

}  // namespace

/*
 * Only the frames that were actually used are written, not the whole
 * fixed-size template: a spoken phrase runs to about fifty frames of
 * the hundred and twenty reserved, so this keeps the file to a couple
 * of kilobytes instead of eleven.
 */
bool AppWilma::save_takes()
{
    if (_store == nullptr) {
        return false;
    }
    if (!GetHAL().sdCardProbe().is_mounted) {
        _store_note = "no card, takes are lost on exit";
        return false;
    }

    FILE* f = std::fopen(kTakesPath, "wb");
    if (f == nullptr) {
        _store_note = "could not write to the card";
        mclog::tagError(getAppInfo().name, "cannot open {} for writing", kTakesPath);
        return false;
    }

    bool ok = std::fwrite(kFileMagic, 1, sizeof(kFileMagic), f) == sizeof(kFileMagic);

    const std::uint8_t header[2] = {(std::uint8_t)kPhrases, (std::uint8_t)kBands};
    ok = ok && std::fwrite(header, 1, sizeof(header), f) == sizeof(header);

    for (int p = 0; ok && p < kPhrases; p++) {
        const std::uint8_t n = (std::uint8_t)_take_count[p];
        ok                   = std::fwrite(&n, 1, 1, f) == 1;

        for (int t = 0; ok && t < _take_count[p]; t++) {
            const Template_t& take = _store->templates[p][t];
            const std::uint16_t len =
                (std::uint16_t)(take.length < 0 ? 0 : (take.length > kMaxFrames ? kMaxFrames
                                                                                : take.length));
            ok = std::fwrite(&len, sizeof(len), 1, f) == 1;
            ok = ok && (len == 0 ||
                        std::fwrite(take.frames, kBands, len, f) == (std::size_t)len);
        }
    }

    std::fclose(f);

    _store_note = ok ? "takes saved to the card" : "the card write failed";
    mclog::tagInfo(getAppInfo().name, "save: {}", _store_note);
    return ok;
}

bool AppWilma::load_takes()
{
    if (_store == nullptr) {
        return false;
    }
    if (!GetHAL().sdCardProbe().is_mounted) {
        _store_note = "no card, takes are lost on exit";
        return false;
    }

    FILE* f = std::fopen(kTakesPath, "rb");
    if (f == nullptr) {
        _store_note = "no takes on the card yet";
        return false;
    }

    std::uint8_t magic[4];
    std::uint8_t header[2];
    bool ok = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
              std::memcmp(magic, kFileMagic, sizeof(magic)) == 0 &&
              std::fread(header, 1, sizeof(header), f) == sizeof(header) &&
              header[0] == kPhrases && header[1] == kBands;

    if (!ok) {
        std::fclose(f);
        _store_note = "the saved takes are from another version";
        mclog::tagWarn(getAppInfo().name, "{} does not match this build", kTakesPath);
        return false;
    }

    int loaded = 0;
    for (int p = 0; ok && p < kPhrases; p++) {
        std::uint8_t n = 0;
        ok             = std::fread(&n, 1, 1, f) == 1 && n <= kTakes;
        _take_count[p] = 0;

        for (int t = 0; ok && t < (int)n; t++) {
            Template_t& take  = _store->templates[p][t];
            std::uint16_t len = 0;

            ok = std::fread(&len, sizeof(len), 1, f) == 1 && len <= kMaxFrames;
            ok = ok && (len == 0 ||
                        std::fread(take.frames, kBands, len, f) == (std::size_t)len);
            if (ok) {
                take.length = len;
                _take_count[p]++;
                _take_next[p] = _take_count[p];
                loaded++;
            }
        }
    }

    std::fclose(f);

    if (!ok) {
        /* A half-read set is worse than none: it would be matched
         * against as though it were real. */
        for (int p = 0; p < kPhrases; p++) {
            _take_count[p] = 0;
        }
        _store_note = "the saved takes are damaged";
        mclog::tagError(getAppInfo().name, "{} is truncated or corrupt", kTakesPath);
        return false;
    }

    _store_note = "takes loaded from the card";
    mclog::tagInfo(getAppInfo().name, "loaded {} takes from {}", loaded, kTakesPath);
    return true;
}

bool AppWilma::decide(Template_t& heard)
{
    (void)heard;

    /* The closest take of each phrase, kept separately: the runner-up
     * has to be a *different* phrase to say anything about confidence,
     * and the second-closest take of the same phrase would always be
     * near and would make every match look ambiguous. */
    int per_phrase[kPhrases];
    for (int p = 0; p < kPhrases; p++) {
        per_phrase[p] = INT32_MAX;
        for (int t = 0; t < _take_count[p]; t++) {
            const int d = compare(_store->scratch, _store->templates[p][t]);
            if (d < per_phrase[p]) {
                per_phrase[p] = d;
            }
        }
    }

    int best_phrase = -1;
    int best        = INT32_MAX;
    int runner_up   = INT32_MAX;

    for (int p = 0; p < kPhrases; p++) {
        if (per_phrase[p] < best) {
            runner_up   = best;
            best        = per_phrase[p];
            best_phrase = p;
        } else if (per_phrase[p] < runner_up) {
            runner_up = per_phrase[p];
        }
    }

    /*
     * Two conditions, and both have to hold.
     *
     * Close enough on its own is not sufficient -- ordinary speech
     * lands somewhere near everything, which is what was producing
     * false readings. It also has to be clearly closer to one phrase
     * than to the others: a quarter better than the runner-up. A word
     * that is not in the set tends to sit an equal distance from all of
     * them and fails that test even when it passes the first.
     */
    /*
     * A part-finished sequence goes stale before anything is judged,
     * not after: what the application is waiting for now decides how
     * strictly this utterance is treated, so the waiting has to be up
     * to date first.
     */
    if (_seq_step > 0 && (GetHAL().millis() - _seq_last_ms) > kSequenceTimeoutMs) {
        _seq_step = 0;
    }

    /*
     * The order is evidence, and it was being thrown away.
     *
     * Only one phrase can advance the sequence; anything else is
     * ignored however well it matches. So the phrase being waited for
     * can be judged more leniently than the rest without letting
     * anything through: a false completion would need three wrong
     * readings, of the right phrases, in the right order, inside the
     * timeout.
     *
     * This is what the measurements asked for. Every miss in the two
     * minute test was "Wilma" -- the shortest and least distinctive of
     * the three -- sitting between 0.90 and 0.95, just outside a margin
     * that could not simply be widened: ordinary speech was being
     * stopped a hair short of both thresholds, so loosening them for
     * everything would have started letting the room in.
     */
    const int expected     = _seq_done ? -1 : kSequence[_seq_step];
    const bool is_expected = (best_phrase == expected);

    const int limit = (_accept_limit > 0 && is_expected) ? (_accept_limit * 23 / 20)
                                                         : _accept_limit;

    const bool close_enough = (limit <= 0) || (best <= limit);

    /*
     * Nine tenths, not four fifths.
     *
     * Measured rather than guessed: over nineteen correctly spoken
     * phrases the ratio of the winner to the nearest other phrase ran
     * from 0.69 to 0.94, and the old four fifths cut straight through
     * the middle of them -- it threw away nearly half of what was said
     * properly. A word from outside the set sits at roughly equal
     * distance from all three, so it lands near 1.0 and still fails.
     */
    /* Nineteen twentieths for the phrase being waited for, eighteen for
     * the others -- 0.95 against 0.90. The stricter figure is the one
     * the measurements supported for a phrase judged on its own. */
    const int margin       = is_expected ? 19 : 18;
    const bool unambiguous = (runner_up == INT32_MAX) || (best * 20 <= runner_up * margin);
    const bool accepted     = (best_phrase >= 0) && close_enough && unambiguous;

    _last_phrase   = accepted ? best_phrase : -1;
    _last_distance = best;

    if (accepted) {
        const std::uint32_t now = GetHAL().millis();

        if (_seq_done) {
            _seq_done = false;
            _seq_step = 0;
        }

        if (best_phrase == kSequence[_seq_step]) {
            _seq_step++;
            _seq_last_ms = now;

            if (_seq_step >= kSequenceLen) {
                _seq_done = true;
                _seq_step = 0;
                mclog::tagInfo(getAppInfo().name, "sequence complete");
            }
        }
        /* Anything else recognised is simply not the next thing wanted,
         * and is left alone rather than resetting the progress. */
    }

    mclog::tagInfo(getAppInfo().name, "best \"{}\" at {}, runner-up {}, limit {}{} -> {}",
                   best_phrase >= 0 ? kPhraseNames[best_phrase] : "-", best,
                   runner_up == INT32_MAX ? -1 : runner_up, limit,
                   is_expected ? " (awaited)" : "", accepted ? "accepted" : "ignored");

    /* The verdict changed, so the screen is out of date -- but drawing
     * it is onRunning's job, not this one's. Doing it here would put a
     * screen transfer in the middle of the stream. */
    _drawn_screen = 0xFF;

    return accepted;
}

/* -------------------------------------------------------------------------- */
/*                                   Drawing                                   */
/* -------------------------------------------------------------------------- */

void AppWilma::draw_menu()
{
    auto& canvas = GetHAL().display;
    clear_if_needed();
    app_header::draw(canvas, "Wilma");
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);

    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString(_mic_ready ? "Record a phrase" : "No microphone", 4, app_header::height() + 4);

    for (int p = 0; p < kPhrases; p++) {
        const int y = app_header::height() + 20 + p * 14;
        const bool sel = (p == _selected);

        /* Both states painted, not just the highlighted one: without a
         * canvas to redraw from scratch, the row the selection has just
         * left keeps its green until something covers it. */
        canvas.fillRect(0, y - 3, canvas.width(), 13, sel ? TFT_DARKGREEN : THEME_COLOR_BG);
        canvas.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? TFT_DARKGREEN : THEME_COLOR_BG);

        char line[40];
        std::snprintf(line, sizeof(line), "%-12s %d/%d takes", kPhraseNames[p], _take_count[p],
                      kTakes);
        canvas.drawString(line, 6, y);
    }

    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    canvas.drawString(_store_note, 4, canvas.height() - 30);
    canvas.drawString("Enter: record  L: listen  C: clear", 4, canvas.height() - 20);
    canvas.drawString("Esc: back", 4, canvas.height() - 10);
}

void AppWilma::draw_record()
{
    auto& canvas = GetHAL().display;
    clear_if_needed();
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString("Say it now", 4, app_header::height() + 8);

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.drawString(kPhraseNames[_selected], 4, app_header::height() + 38);
}

void AppWilma::draw_listen()
{
    auto& canvas = GetHAL().display;
    clear_if_needed();
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString("Listening", 4, app_header::height() + 4);

    canvas.setTextSize(2);
    if (_seq_done) {
        canvas.setTextColor(TFT_GREENYELLOW, THEME_COLOR_BG);
        canvas.drawString("OK", 4, app_header::height() + 20);
    } else {
        canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
        canvas.drawString(_last_phrase >= 0 ? kPhraseNames[_last_phrase] : "...", 4, app_header::height() + 20);
    }

    /*
     * The sequence, with what has been said already ticked off and what
     * is wanted next picked out. Without this the application knows
     * where it is in the order and the person in front of it does not.
     */
    canvas.setTextSize(1);
    for (int i = 0; i < kSequenceLen; i++) {
        const bool done = _seq_done || (i < _seq_step);
        const bool next = !_seq_done && (i == _seq_step);

        canvas.setTextColor(done ? TFT_GREENYELLOW : (next ? TFT_ORANGE : TFT_DARKGREY),
                            THEME_COLOR_BG);

        char step[32];
        std::snprintf(step, sizeof(step), "%c %s", done ? '*' : (next ? '>' : ' '),
                      kPhraseNames[kSequence[i]]);
        canvas.drawString(step, 4, 50 + i * 10);
    }

    canvas.setTextColor(TFT_LIGHTGREY, THEME_COLOR_BG);
    char line[48];
    std::snprintf(line, sizeof(line), "dist %d  limit %d", _last_distance, _accept_limit);
    canvas.drawString(line, 4, 84);

    /* The level meter is here so a silent microphone is obvious. Every
     * other symptom of a dead capture looks like bad recognition. */

    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    canvas.drawString("Esc: back", 4, canvas.height() - 10);
}

/*
 * What is missing without this: the capture arms silently, so there is
 * no way to tell whether it is waiting for you, already taking the
 * phrase, or hearing nothing at all. The bar is the honest part -- it
 * moves with your voice, and the mark on it is the level at which the
 * capture will trigger, so a microphone that never reaches the mark is
 * visibly a microphone that never reaches the mark.
 */
void AppWilma::draw_capture(bool armed)
{
    auto& canvas = GetHAL().display;

    /* Always cleared, whatever the flag says: "Got it" is shorter than
     * "Speak now", and text painted over its own background leaves the
     * tail of the longer word behind. */
    canvas.fillScreen(THEME_COLOR_BG);
    _needs_clear = false;
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    canvas.drawString(_screen == SCREEN_LISTEN ? "Listening" : "Recording", 4, 2);

    canvas.setTextSize(2);
    canvas.setTextColor(armed ? TFT_GREENYELLOW : TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString(armed ? "Got it" : "Speak now", 4, 24);

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    canvas.drawString(kPhraseNames[_selected], 4, app_header::height() + 38);

    const int bar_y = 70;
    const int bar_h = 10;
    const int full  = canvas.width() - 8;

    canvas.drawRect(4, bar_y, full, bar_h, TFT_DARKGREY);

    /* Scaled so the gate sits a third of the way along, whatever the
     * room turned out to be. A fixed full scale would put the mark off
     * the end of the bar in a noisy room and against the left edge in a
     * quiet one. */
    const int scale = (_gate > 0) ? (_gate * 3) : 4000;

    int w = _last_level * full / scale;
    if (w > full) w = full;
    if (w > 0) {
        canvas.fillRect(4, bar_y, w, bar_h, armed ? TFT_GREENYELLOW : TFT_DARKGREEN);
    }

    /* Where the gate sits, so the bar can be read against it. */
    const int mark = 4 + _gate * full / scale;
    canvas.drawFastVLine(mark, bar_y - 3, bar_h + 6, TFT_RED);

    char line[40];
    std::snprintf(line, sizeof(line), "level %d  gate %d  noise %d", _last_level, _gate, _noise);
    canvas.setTextColor(TFT_LIGHTGREY, THEME_COLOR_BG);
    canvas.drawString(line, 4, bar_y + bar_h + 6);

    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    canvas.drawString("Esc: stop", 4, canvas.height() - 10);
}

/*
 * The panel is drawn on directly now, so clearing it is a decision
 * rather than a free side effect of building a fresh sprite. A whole
 * screen is wiped when the screen changes; moving the selection within
 * one repaints only what it owns, which is what keeps the menu from
 * blinking under the arrow keys.
 */
void AppWilma::clear_if_needed()
{
    if (!_needs_clear) {
        return;
    }
    GetHAL().display.fillScreen(THEME_COLOR_BG);
    _needs_clear = false;
}

void AppWilma::draw()
{
    switch (_screen) {
        case SCREEN_RECORD: draw_record(); break;
        case SCREEN_LISTEN: draw_listen(); break;
        default:            draw_menu(); break;
    }
}

void AppWilma::show(std::uint8_t screen)
{
    _needs_clear  = true;
    _screen       = screen;
    _drawn_screen = screen;
    draw();
}

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                  */
/* -------------------------------------------------------------------------- */

void AppWilma::handle_char(char ch)
{
    if (ch == 'l' || ch == 'L') {
        show(_screen == SCREEN_LISTEN ? SCREEN_MENU : SCREEN_LISTEN);
        return;
    }

    /*
     * Clearing one phrase, because rotating through three takes only
     * helps if the three are all worth keeping. A bad one recorded by
     * accident would otherwise stay in the set until it happened to be
     * overwritten, dragging the acceptance limit out with it.
     */
    if ((ch == 'c' || ch == 'C') && _screen == SCREEN_MENU) {
        _take_count[_selected] = 0;
        _take_next[_selected]  = 0;

        mclog::tagInfo(getAppInfo().name, "cleared \"{}\"", kPhraseNames[_selected]);

        compute_accept_limit();
        save_takes();
        show(SCREEN_MENU);
    }
}

void AppWilma::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    app_header::reset();

    /*
     * Worth having on the record: free memory on this board falls in
     * the seconds after boot, as WiFi and the web server come up, and
     * the Record application can only be opened while it is still high.
     * If this one ever fails to open, these two numbers say why.
     */
    /*
     * The panel, taken whole -- and taken before anything is allocated.
     *
     * The launcher's two sprites are 64 KB, of which the application
     * canvas alone is a single 53 KB block, and they are held for as
     * long as the launcher is alive. This screen carries no system bar
     * of its own and draws straight onto the glass, so for as long as it
     * is open they would only be holding memory. Handing them back here,
     * ahead of the takes and the audio window, is what turns "no room
     * for a 1500 ms window" into a window that fits: the free block the
     * canvas leaves behind is more than twice what this needs.
     */
    GetHAL().setFullScreenApp(true);

    mclog::tagInfo(getAppInfo().name, "free heap {}, largest block {}",
                   heap_caps_get_free_size(MALLOC_CAP_8BIT),
                   heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    _store = new (std::nothrow) Store_t();
    if (_store == nullptr) {
        mclog::tagError(getAppInfo().name, "no room for the takes ({} bytes)", sizeof(Store_t));
    }

    /* Whatever was trained in an earlier session, so the training does
     * not have to be repeated every time the application is opened. */
    load_takes();
    compute_accept_limit();

    _audio = (std::int16_t*)heap_caps_malloc(kMaxSamples * sizeof(std::int16_t), MALLOC_CAP_8BIT);
    if (_audio == nullptr) {
        mclog::tagError(getAppInfo().name, "no room for a {} ms window ({} bytes), largest free {}",
                        kWindowMs, kMaxSamples * sizeof(std::int16_t),
                        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    /*
     * The keyboard clicks through the speaker, and the speaker and the
     * microphone are the same I2S peripheral on the same pins. Leaving
     * the clicks on meant that pressing enter to record handed I2S
     * straight back to the speaker, and the microphone then found its
     * own pins taken -- "GPIO 43 is not usable, maybe conflict with
     * others", and every capture came back empty. Silence first, then
     * take the peripheral.
     */
    audio::set_keyboard_sfx_enable(false);

    GetHAL().speaker.end();

    /*
     * Gain, because the built-in microphone is quiet -- but far less of
     * it than the Record application uses.
     *
     * At its 128 the empty room measured 2535, which put the gate at
     * 7725 and made it unreachable: loud speech clips against the top
     * of a 16-bit sample long before it gets three times above a floor
     * that high, so shouting could not open a gate that quiet speech
     * could not open either. Record wants volume for playback; this
     * wants the shape of the spectrum intact, so it keeps headroom.
     */
    auto cfg               = GetHAL().mic.config();
    cfg.magnification      = 16;
    cfg.noise_filter_level = 2;
    GetHAL().mic.config(cfg);

    _mic_ready = GetHAL().mic.begin();
    _mic_primed = false;
    if (!_mic_ready) {
        mclog::tagError(getAppInfo().name, "microphone would not start");
    }

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (!e.state || e.isModifier) {
            return;
        }
        if (e.keyCode == KEY_ESC) {
            if (_screen == SCREEN_MENU) {
                _close_requested = true;
            } else {
                /* Order matters: the flag is what a capture running
                 * underneath this callback will see when it returns to
                 * its loop, and it must be set before the screen
                 * changes or the capture would carry on regardless. */
                _abort  = true;
                _screen = SCREEN_MENU;
            }
            return;
        }
        if (e.keyName != nullptr && std::strlen(e.keyName) == 1) {
            handle_char(e.keyName[0]);
        }
    });

    /* Raw positions as well as key codes, because that is how every
     * other screen here is driven -- ; and . move, enter chooses. */
    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& k) {
        if (!k.state) {
            return;
        }

        /*
         * Back, by matrix position rather than by key code.
         *
         * The key printed "esc" is row 0 column 0, and its KEY_ESC only
         * exists on the Fn layer -- unshifted it sends a backtick. So a
         * screen listening for KEY_ESC answers to Fn+` and to nothing
         * else, which is indistinguishable from a dead key. The
         * launcher has always switched on position for exactly this
         * reason.
         */
        if (k.row == 0 && k.col == 0) {
            if (_screen == SCREEN_MENU) {
                _close_requested = true;
            } else {
                _abort  = true;
                _screen = SCREEN_MENU;
            }
            return;
        }

        if (_screen != SCREEN_MENU) {
            return;
        }
        if (k.row == 2 && k.col == 11) {
            _selected = (_selected + kPhrases - 1) % kPhrases;
            draw();
        } else if (k.row == 3 && k.col == 11) {
            _selected = (_selected + 1) % kPhrases;
            draw();
        } else if (k.row == 2 && k.col == 13) {
            /*
             * Only a request. Capturing from inside this callback meant
             * capturing from inside keyboard.update(), and capture()
             * calls that again to keep Esc alive -- keyboard.update()
             * re-entering itself, which wedged the keyboard exactly
             * when it was most needed. onRunning picks this up on the
             * next pass, off the callback stack.
             */
            _want_record = true;
        }
    });

    show(SCREEN_MENU);
}

void AppWilma::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        close();
        return;
    }

    if (GetHAL().homeButton.wasClicked()) {
        close();
        return;
    }

    /* Whatever changed the screen -- a key handler, an aborted capture,
     * a finished recording -- this is what puts it on the display. */
    if (_screen != _drawn_screen) {
        _drawn_screen = _screen;
        _needs_clear  = true;
        draw();
    }

    if (_want_record) {
        _want_record = false;
        do_record(_selected);
        return;
    }

    /* Listening is a stream now, not a series of recordings: this
     * takes whatever has arrived and returns immediately, so the
     * keyboard and the rest of the loop keep running throughout. */
    if (_screen == SCREEN_LISTEN) {
        if (!_streaming) {
            stream_start();
        }
        stream_poll();
    } else if (_streaming) {
        stream_stop();
    }
}

void AppWilma::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    if (_key_raw_slot >= 0) {
        GetHAL().keyboard.onKeyEventRaw.disconnect(_key_raw_slot);
        _key_raw_slot = -1;
    }

    /* Hand the peripheral back the way it was found, or the rest of the
     * firmware leaves here mute. */
    while (GetHAL().mic.isRecording()) {
        GetHAL().delay(1);
    }
    GetHAL().mic.end();
    GetHAL().speaker.begin();
    GetHAL().speaker.setVolume(audio::DEFAULT_VOLUME);
    audio::set_keyboard_sfx_enable(true);

    delete _store;
    _store = nullptr;

    free(_audio);
    _audio = nullptr;

    /* The launcher gets its sprites back, and redraws itself from
     * scratch, so nothing is lost by having thrown the pixels away. */
    GetHAL().setFullScreenApp(false);
}
