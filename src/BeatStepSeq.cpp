// =============================================================================
// BeatStepSeq.cpp — VCV Rack 2 Module: BeatStep Sequencer Sync
// Two-way sync with the Arturia BeatStep hardware sequencer over SysEx MIDI.
// =============================================================================

#include "plugin.hpp"
#include <rack.hpp>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <memory>
#include <algorithm>

using namespace rack;

// =============================================================================
// SysEx commands for the Arturia BeatStep (firmware 1.2.0).
// Verified against https://www.untergeek.de/2014/11/taming-arturias-beatstep-sysex-codes-for-programming-via-ipad/
//
// Per-step:   F0 00 20 6B 7F 42 <01=read|02=write> 00 <pp> <step> [<value>] F7
//   pp=0x52  step note   (step 0..15, value = MIDI note 0-127)
//   pp=0x53  step gate   (step 0..15, value = 0x7F on / 0x00 off)
//
// Global:     F0 00 20 6B 7F 42 <01=read|02=write> 00 50 <sub> [<value>] F7
//   sub=0x01  MIDI channel     (0-15, stored as channel-1)
//   sub=0x02  Transpose        (raw note value, base note C4 = 0x3C = 60)
//   sub=0x03  Scale            (0-7: Chromatic/Major/Minor/Dorian/Mixolydian/HarmMinor/Blues/User)
//   sub=0x04  Play mode        (0-3: Forward/Reverse/Alternating/Random)
//   sub=0x05  Step size        (0-3: 1/4, 1/8, 1/16, 1/32)
//   sub=0x06  Pattern length   (1-16 steps)
//   sub=0x07  Swing            (50-75, percent)
//   sub=0x08  Gate length      (50-99, percent)
//   sub=0x09  Legato           (0-2: Off/On/Reset)
//
// Response frames use the same layout as a write frame regardless of whether
// they answer a read or reflect a live edit on the hardware itself.
//
// Preset memory (separate, shorter frame -- no "00"/pp/cc breakdown):
//   Store:   F0 00 20 6B 7F 42 06 <bank 1-16> F7
//   Recall:  F0 00 20 6B 7F 42 05 <bank 1-16> F7
// Recall does not push a parameter dump -- the poller's next full pass is
// what actually re-reads note/gate/global state after a recall.
// =============================================================================
namespace BS {
    static const int NUM_STEPS = 16;

    enum GlobalParam : uint8_t {
        GP_CHANNEL   = 0x01,
        GP_TRANSPOSE = 0x02,
        GP_SCALE     = 0x03,
        GP_MODE      = 0x04,
        GP_STEP_SIZE = 0x05,
        GP_LENGTH    = 0x06,
        GP_SWING     = 0x07,
        GP_GATE_LEN  = 0x08,
        GP_LEGATO    = 0x09,
    };

    static inline std::vector<uint8_t> readStep(uint8_t pp, int step) {
        return {0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42, 0x01, 0x00, pp, (uint8_t)step, 0xF7};
    }
    static inline std::vector<uint8_t> writeStep(uint8_t pp, int step, uint8_t value) {
        return {0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42, 0x02, 0x00, pp, (uint8_t)step, value, 0xF7};
    }
    static inline std::vector<uint8_t> readNote(int step) { return readStep(0x52, step); }
    static inline std::vector<uint8_t> readGate(int step) { return readStep(0x53, step); }
    static inline std::vector<uint8_t> writeNote(int step, uint8_t note) { return writeStep(0x52, step, note & 0x7F); }
    static inline std::vector<uint8_t> writeGate(int step, bool on) { return writeStep(0x53, step, on ? 0x7F : 0x00); }

    static inline std::vector<uint8_t> readGlobal(uint8_t sub) {
        return {0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42, 0x01, 0x00, 0x50, sub, 0xF7};
    }
    static inline std::vector<uint8_t> writeGlobal(uint8_t sub, uint8_t value) {
        return {0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42, 0x02, 0x00, 0x50, sub, value, 0xF7};
    }

    // bank is 1-16, matching the documented mm range.
    static inline std::vector<uint8_t> storeBank(int bank) {
        return {0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42, 0x06, (uint8_t)clamp(bank, 1, 16), 0xF7};
    }
    static inline std::vector<uint8_t> recallBank(int bank) {
        return {0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42, 0x05, (uint8_t)clamp(bank, 1, 16), 0xF7};
    }

    // MIDI note 60 = "C4" (matches the hardware's own baseline, and the convention
    // most DAWs/keyboards use).
    static inline std::string noteName(uint8_t n) {
        static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        return std::string(names[n % 12]) + std::to_string((int)(n / 12) - 1);
    }

    static inline void sendMsg(midi::Output& out, const std::vector<uint8_t>& bytes) {
        midi::Message msg;
        msg.setSize(bytes.size());
        for (size_t i = 0; i < bytes.size(); i++)
            msg.bytes[i] = bytes[i];
        out.sendMessage(msg);
    }
}

// =============================================================================
// Global-parameter formatting/labels, shared by every knob's value readout.
// =============================================================================
static const char* SCALE_NAMES[8]  = {"Chromatic", "Major", "Minor", "Dorian", "Mixolydian", "HarmMinor", "Blues", "User"};
static const char* MODE_NAMES[4]   = {"Forward", "Reverse", "Alternat.", "Random"};
static const char* STEPSZ_NAMES[4] = {"1/4", "1/8", "1/16", "1/32"};
static const char* LEGATO_NAMES[3] = {"Off", "On", "Reset"};

// =============================================================================
// Sequencer + global-parameter state (thread-safe: audio/GUI/poller all touch it)
// =============================================================================
struct BSState {
    mutable std::mutex mtx;
    uint8_t notes[BS::NUM_STEPS];
    bool    gates[BS::NUM_STEPS];

    int patLen   = 16;
    int channel  = 0;    // 0-15
    int transpose = 60;  // 36-84 (C2-C6), base note C4 = 0x3C = 60
    int scale    = 0;    // 0-7
    int mode     = 0;    // 0-3
    int stepSize = 2;    // 0-3 (1/16 default)
    int swing    = 50;   // 50-75
    int gateLen  = 50;   // 50-99
    int legato   = 0;    // 0-2

    BSState() {
        for (int i = 0; i < BS::NUM_STEPS; i++) {
            notes[i] = 60;
            gates[i] = true;
        }
    }

    uint8_t getNote(int i) const { std::lock_guard<std::mutex> g(mtx); return notes[i]; }
    bool    getGate(int i) const { std::lock_guard<std::mutex> g(mtx); return gates[i]; }
    void setNote(int i, uint8_t v) { std::lock_guard<std::mutex> g(mtx); notes[i] = v; }
    void setGate(int i, bool v)    { std::lock_guard<std::mutex> g(mtx); gates[i] = v; }

    int getGlobal(BS::GlobalParam kind) const {
        std::lock_guard<std::mutex> g(mtx);
        switch (kind) {
            case BS::GP_CHANNEL:   return channel;
            case BS::GP_TRANSPOSE: return transpose;
            case BS::GP_SCALE:     return scale;
            case BS::GP_MODE:      return mode;
            case BS::GP_STEP_SIZE: return stepSize;
            case BS::GP_LENGTH:    return patLen;
            case BS::GP_SWING:     return swing;
            case BS::GP_GATE_LEN:  return gateLen;
            case BS::GP_LEGATO:    return legato;
        }
        return 0;
    }
    void setGlobal(BS::GlobalParam kind, int v) {
        std::lock_guard<std::mutex> g(mtx);
        switch (kind) {
            case BS::GP_CHANNEL:   channel   = clamp(v, 0, 15); break;
            case BS::GP_TRANSPOSE: transpose = clamp(v, 36, 84); break;
            case BS::GP_SCALE:     scale     = clamp(v, 0, 7); break;
            case BS::GP_MODE:      mode      = clamp(v, 0, 3); break;
            case BS::GP_STEP_SIZE: stepSize  = clamp(v, 0, 3); break;
            case BS::GP_LENGTH:    patLen    = clamp(v, 1, 16); break;
            case BS::GP_SWING:     swing     = clamp(v, 50, 75); break;
            case BS::GP_GATE_LEN:  gateLen   = clamp(v, 50, 99); break;
            case BS::GP_LEGATO:    legato    = clamp(v, 0, 2); break;
        }
    }
    int getLen() const { std::lock_guard<std::mutex> g(mtx); return patLen; }

    // Parse an incoming response frame (same layout as a write frame -- see header comment).
    bool applyResponse(const std::vector<uint8_t>& msg) {
        if (msg.size() < 11) return false;
        if (msg[0] != 0xF0) return false;
        if (msg[1] != 0x00 || msg[2] != 0x20 || msg[3] != 0x6B) return false;
        if (msg[5] != 0x42) return false;
        if (msg[6] != 0x02 || msg[7] != 0x00) return false;

        uint8_t pp = msg[8];
        uint8_t cc = msg[9];
        uint8_t vv = msg[10];

        if (pp == 0x52 && cc < BS::NUM_STEPS) { setNote(cc, vv & 0x7F); return true; }
        if (pp == 0x53 && cc < BS::NUM_STEPS) { setGate(cc, vv != 0x00); return true; }
        if (pp == 0x50) {
            switch (cc) {
                case BS::GP_CHANNEL:   setGlobal(BS::GP_CHANNEL,   vv); return true;
                case BS::GP_TRANSPOSE: setGlobal(BS::GP_TRANSPOSE, vv); return true;
                case BS::GP_SCALE:     setGlobal(BS::GP_SCALE,     vv); return true;
                case BS::GP_MODE:      setGlobal(BS::GP_MODE,      vv); return true;
                case BS::GP_STEP_SIZE: setGlobal(BS::GP_STEP_SIZE, vv); return true;
                case BS::GP_LENGTH:    setGlobal(BS::GP_LENGTH,    vv); return true;
                case BS::GP_SWING:     setGlobal(BS::GP_SWING,     vv); return true;
                case BS::GP_GATE_LEN:  setGlobal(BS::GP_GATE_LEN,  vv); return true;
                case BS::GP_LEGATO:    setGlobal(BS::GP_LEGATO,    vv); return true;
            }
        }
        return false;
    }
};

static std::string formatGlobalParam(BS::GlobalParam kind, int v) {
    switch (kind) {
        case BS::GP_CHANNEL:   return string::f("Ch %d", v + 1);
        case BS::GP_TRANSPOSE: return BS::noteName((uint8_t)v);
        case BS::GP_SCALE:     return SCALE_NAMES[clamp(v, 0, 7)];
        case BS::GP_MODE:      return MODE_NAMES[clamp(v, 0, 3)];
        case BS::GP_STEP_SIZE: return STEPSZ_NAMES[clamp(v, 0, 3)];
        case BS::GP_LENGTH:    return string::f("%d steps", v);
        case BS::GP_SWING:     return string::f("%d%%", v);
        case BS::GP_GATE_LEN:  return string::f("%d%%", v);
        case BS::GP_LEGATO:    return LEGATO_NAMES[clamp(v, 0, 2)];
    }
    return "";
}
static const char* globalParamLabel(BS::GlobalParam kind) {
    switch (kind) {
        case BS::GP_CHANNEL:   return "Channel";
        case BS::GP_TRANSPOSE: return "Transpose";
        case BS::GP_SCALE:     return "Scale";
        case BS::GP_MODE:      return "Mode";
        case BS::GP_STEP_SIZE: return "Step sz";
        case BS::GP_LENGTH:    return "Length";
        case BS::GP_SWING:     return "Swing";
        case BS::GP_GATE_LEN:  return "Gate len";
        case BS::GP_LEGATO:    return "Legato";
    }
    return "";
}

// =============================================================================
// Module
// =============================================================================
struct BeatStepSeqModule : Module {

    enum ParamIds {
        PATTERN_LENGTH_PARAM,
        CHANNEL_PARAM, TRANSPOSE_PARAM, SCALE_PARAM, MODE_PARAM,
        STEP_SIZE_PARAM, SWING_PARAM, GATE_LEN_PARAM, LEGATO_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        SLOT_CV_INPUT, RECALL_TRIG_INPUT,
        CHANNEL_CV_INPUT, TRANSPOSE_CV_INPUT, SCALE_CV_INPUT, MODE_CV_INPUT,
        STEP_SIZE_CV_INPUT, LENGTH_CV_INPUT, SWING_CV_INPUT, GATE_LEN_CV_INPUT, LEGATO_CV_INPUT,
        CLOCK_INPUT,
        NUM_INPUTS
    };
    enum OutputIds { NUM_OUTPUTS };
    enum LightIds {
        CONNECTED_LIGHT,
        ENUMS(GATE_LIGHT, 16),
        NUM_LIGHTS
    };

    midi::Output     midiOut;
    midi::InputQueue midiIn;
    std::mutex       midiOutMtx;

    BSState state;
    std::atomic<bool> stateChanged{false};
    std::atomic<int> selectedSaveSlot{0}; // 0-15, UI-selected target for "Save -> Slot" (Slot CV/Recall Trig use their own CV-derived slot)
    std::atomic<int> cvSlotHighlight{-1}; // live slot pointed to by Slot CV, -1 when not patched (drives SlotButton highlight)

    // Rnd Notes / Rnd Gates settings, adjustable via the module's right-click menu.
    int randNoteOctMin = 3; // C3
    int randNoteOctMax = 5; // C5
    int randGateDensityPct = 60;

    // Per-CV-binding rate-limit timers (index matches the cvBindings table in
    // process()) -- without this, a fast LFO/noise source patched into a
    // global-param CV input would fire a SysEx write every audio block
    // (potentially thousands/sec), flooding the USB-MIDI link to the hardware.
    float cvGlobalTimer[9] = {};

    // Countdown (seconds) per global param, indexed directly by BS::GlobalParam
    // (1-9, index 0 unused). Set whenever WE just wrote a new value (manual
    // knob turn or CV), so the poll thread's "push hardware state into the
    // knobs" step (below in process()) leaves that knob alone until the
    // write+read-back round trip has had a chance to catch up. Without this,
    // a knob being dragged gets yanked back to the last-confirmed (now stale)
    // hardware value every poll pass -- worse the faster the poller runs.
    float knobTouchGrace[10] = {};

    std::thread       pollThread;
    std::atomic<bool> pollRunning{false};
    std::atomic<bool> immediateRefresh{false};
    std::atomic<bool> pendingRecallSettle{false};
    // 0 = none, 1 = Rnd Notes, 2 = Rnd Gates. Handled by the poll thread itself
    // (not sent directly from the GUI button click) so the burst of writes
    // can't race against the poller's own concurrent reads of the same steps --
    // only one thread ever touches midiOut/midiIn at a time this way.
    std::atomic<int> pendingRandomize{0};

    std::mutex           rxMtx;
    std::vector<uint8_t> rxBuf;
    bool                 inSysex = false;

    dsp::SchmittTrigger recallTrigDetector;
    dsp::SchmittTrigger clockTrigDetector;

    BeatStepSeqModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PATTERN_LENGTH_PARAM, 1.f, 16.f, 16.f, "Pattern length", " steps");
        configParam(CHANNEL_PARAM,   0.f, 15.f, 0.f,  "MIDI channel");
        // Real hardware's usable transpose range is C2-C6 (MIDI 36-84), not the
        // full 0-127 -- confirmed against real BeatStep hardware.
        configParam(TRANSPOSE_PARAM, 36.f, 84.f, 60.f, "Transpose");
        configParam(SCALE_PARAM,     0.f, 7.f,  0.f,  "Scale");
        configParam(MODE_PARAM,      0.f, 3.f,  0.f,  "Play mode");
        configParam(STEP_SIZE_PARAM, 0.f, 3.f,  2.f,  "Step size");
        configParam(SWING_PARAM,     50.f, 75.f, 50.f, "Swing", "%");
        configParam(GATE_LEN_PARAM,  50.f, 99.f, 50.f, "Gate length", "%");
        configParam(LEGATO_PARAM,    0.f, 2.f,  0.f,  "Legato");
        configInput(SLOT_CV_INPUT, "Preset slot select (1V/slot, 0-15V)");
        configInput(RECALL_TRIG_INPUT, "Recall preset (rising edge)");
        configInput(CHANNEL_CV_INPUT,   "Channel CV (0-10V = ch 1-16)");
        configInput(TRANSPOSE_CV_INPUT, "Transpose CV (0-10V = C2-C6)");
        configInput(SCALE_CV_INPUT,     "Scale CV (0-10V = 8 scales)");
        configInput(MODE_CV_INPUT,      "Play mode CV (0-10V = Fwd/Rev/Alt/Random)");
        configInput(STEP_SIZE_CV_INPUT, "Step size CV (0-10V = 1/4-1/32)");
        configInput(LENGTH_CV_INPUT,    "Pattern length CV (0-10V = 1-16 steps)");
        configInput(SWING_CV_INPUT,     "Swing CV (0-10V = 50-75%)");
        configInput(GATE_LEN_CV_INPUT,  "Gate length CV (0-10V = 50-99%)");
        configInput(LEGATO_CV_INPUT,    "Legato CV (0-10V = Off/On/Reset)");
        configInput(CLOCK_INPUT, "Clock (rising edge = one MIDI Clock tick to the BeatStep; feed 24 PPQN)");
    }

    ~BeatStepSeqModule() {
        stopPoller();
    }

    void sendSysEx(const std::vector<uint8_t>& bytes) {
        std::lock_guard<std::mutex> g(midiOutMtx);
        BS::sendMsg(midiOut, bytes);
    }

    // Forwards a CV clock pulse as a single MIDI Clock (0xF8) byte, through the
    // same mutex-guarded midiOut as sendSysEx -- so a separate clock-generator
    // module no longer needs to share this module's MIDI OUT device to keep the
    // BeatStep in tempo, which was racing against this module's own SysEx
    // traffic at the shared MIDI port (two independent writers to one device is
    // outside anything this module can otherwise coordinate).
    void sendClockTick() {
        std::lock_guard<std::mutex> g(midiOutMtx);
        midi::Message msg;
        msg.setSize(1);
        msg.bytes[0] = 0xF8;
        midiOut.sendMessage(msg);
    }

    // Recall (unlike store) makes the hardware internally switch its active
    // pattern before its parameter reads reflect the new one -- if the poller's
    // very next request lands before that internal switch has actually finished,
    // the first few queried steps come back with the OLD pattern's data while
    // later ones already reflect the new one, i.e. a half-old/half-new mix that
    // looks exactly like "recall doesn't work right." pendingRecallSettle makes
    // the poller wait a bit before its next pass instead of racing it.
    void recallSlot(int slot0based) {
        sendSysEx(BS::recallBank(slot0based + 1));
        pendingRecallSettle = true;
        immediateRefresh = true;
    }

    // Drain queued MIDI input and reassemble SysEx frames. Returns true if at
    // least one complete frame was dispatched (used by waitForReply() below to
    // exit early instead of always waiting a fixed amount of time).
    bool drainInput() {
        bool any = false;
        midi::Message msg;
        while (midiIn.tryPop(&msg, INT64_MAX)) {
            std::vector<uint8_t> complete;
            {
                std::lock_guard<std::mutex> g(rxMtx);
                for (uint8_t b : msg.bytes) {
                    // System Real-Time bytes (Clock 0xF8, Start/Continue/Stop, Active
                    // Sensing, Reset -- the whole 0xF8-0xFF range) are, per the MIDI
                    // spec, allowed to appear injected in the middle of any other
                    // message, including a SysEx dump in progress -- a receiver must
                    // skip them transparently and keep reassembling whatever was
                    // already underway. Without this, a clock source sharing the same
                    // MIDI OUT as this module (e.g. a sequencer/transport clocking the
                    // BeatStep) injects a stray 0xF8 into rxBuf mid-frame, corrupting
                    // the reassembled SysEx and silently breaking sync.
                    if (b >= 0xF8) continue;
                    if (b == 0xF0) {
                        rxBuf.clear();
                        rxBuf.push_back(b);
                        inSysex = true;
                    } else if (inSysex) {
                        rxBuf.push_back(b);
                        if (b == 0xF7) {
                            complete = rxBuf;
                            rxBuf.clear();
                            inSysex = false;
                        }
                    }
                }
            }
            if (!complete.empty()) {
                dispatchSysEx(complete);
                any = true;
            }
        }
        return any;
    }

    void dispatchSysEx(const std::vector<uint8_t>& msg) {
        if (msg.size() < 8) return;
        if (msg[1] != 0x00 || msg[2] != 0x20 || msg[3] != 0x6B) return;
        if (msg[5] != 0x42) return;

        if (state.applyResponse(msg))
            stateChanged = true;
    }

    // Poll-thread only. USB-MIDI round-trip latency isn't instantaneous, so poll
    // for up to timeoutMs (checking every 2ms) instead of a single immediate
    // drainInput() call -- a reply landing a few ms late must still be caught
    // before the next request's leading F0 resets the in-progress reassembly.
    bool waitForReply(int timeoutMs = 60) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        do {
            if (drainInput()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (pollRunning && std::chrono::steady_clock::now() < deadline);
        return false;
    }

    void startPoller() {
        if (pollRunning) return;
        pollRunning = true;
        pollThread = std::thread([this]() {
            static const BS::GlobalParam globals[] = {
                BS::GP_CHANNEL, BS::GP_TRANSPOSE, BS::GP_SCALE, BS::GP_MODE,
                BS::GP_STEP_SIZE, BS::GP_LENGTH, BS::GP_SWING, BS::GP_GATE_LEN, BS::GP_LEGATO
            };
            while (pollRunning) {
                immediateRefresh.exchange(false);
                if (pendingRecallSettle.exchange(false)) {
                    for (int t = 0; t < 15 && pollRunning; t++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                int rj = pendingRandomize.exchange(0);
                if (rj != 0 && midiOut.getDeviceId() >= 0) {
                    std::mt19937 rng(std::random_device{}());
                    int len = state.getLen();
                    if (rj == 1) {
                        int lo = std::min(randNoteOctMin, randNoteOctMax);
                        int hi = std::max(randNoteOctMin, randNoteOctMax);
                        std::uniform_int_distribution<int> dist((lo + 1) * 12, (hi + 1) * 12);
                        for (int i = 0; i < len && pollRunning; i++) {
                            uint8_t n = (uint8_t)dist(rng);
                            state.setNote(i, n);
                            sendSysEx(BS::writeNote(i, n));
                            // Paced, not back-to-back: an unpaced burst of up to 16
                            // writes can outrun the hardware's own SysEx receive/
                            // processing rate, silently dropping some.
                            std::this_thread::sleep_for(std::chrono::milliseconds(8));
                        }
                    } else {
                        std::bernoulli_distribution dist(randGateDensityPct / 100.0);
                        for (int i = 0; i < len && pollRunning; i++) {
                            bool g = dist(rng);
                            state.setGate(i, g);
                            sendSysEx(BS::writeGate(i, g));
                            std::this_thread::sleep_for(std::chrono::milliseconds(8));
                        }
                    }
                    immediateRefresh = true;
                    continue;
                }

                if (midiOut.getDeviceId() >= 0 && midiIn.getDeviceId() >= 0) {
                    for (int i = 0; i < BS::NUM_STEPS && pollRunning; i++) {
                        sendSysEx(BS::readNote(i));
                        waitForReply();
                    }
                    for (int i = 0; i < BS::NUM_STEPS && pollRunning; i++) {
                        sendSysEx(BS::readGate(i));
                        waitForReply();
                    }
                    for (BS::GlobalParam gp : globals) {
                        if (!pollRunning) break;
                        sendSysEx(BS::readGlobal(gp));
                        waitForReply();
                    }
                }

                // Idle gap between full poll passes. A physical change on the
                // hardware (turning a knob, hitting a pad) is only noticed on the
                // *next* pass, so this gap is the dominant source of
                // hardware->software latency -- kept short, but nonzero so the
                // device gets brief breathing room between passes.
                for (int t = 0; t < 10 && pollRunning && !immediateRefresh; t++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void stopPoller() {
        pollRunning = false;
        if (pollThread.joinable()) pollThread.join();
    }

    void process(const ProcessArgs& args) override {
        bool conn = (midiOut.getDeviceId() >= 0);
        lights[CONNECTED_LIGHT].setBrightness(conn ? 1.f : 0.f);

        // Recall preset: CV selects the slot (1V/slot, 0-15V -> slot 1-16), a
        // rising trigger edge recalls it. Independent of selectedSaveSlot, which
        // is only ever used by the manual "Save -> Slot" button -- CV control of
        // a destructive save would be too easy to trigger by accident.
        int slotFromCv = (int) clamp(std::round(inputs[SLOT_CV_INPUT].getVoltage()), 0.f, 15.f);
        cvSlotHighlight = inputs[SLOT_CV_INPUT].isConnected() ? slotFromCv : -1;
        if (recallTrigDetector.process(inputs[RECALL_TRIG_INPUT].getVoltage())) {
            recallSlot(slotFromCv);
        }

        if (clockTrigDetector.process(inputs[CLOCK_INPUT].getVoltage())) {
            sendClockTick();
        }

        // CV control of all 9 global params: each CV, when patched, maps 0-10V
        // across that param's own range and overrides the knob; same
        // echo-avoidance pattern as GlobalParamKnob::onChange -- only writes/sends
        // when the mapped value actually differs from the already-known state.
        // Rate-limited to at most one SysEx write per ~20ms per binding (see
        // cvGlobalTimer) so a fast LFO/noise source patched into one of these
        // can't flood the hardware's USB-MIDI link.
        static const struct { BS::GlobalParam kind; int inputId; int paramId; float minV; float maxV; } cvBindings[9] = {
            {BS::GP_CHANNEL,   CHANNEL_CV_INPUT,   CHANNEL_PARAM,        0.f,  15.f},
            {BS::GP_TRANSPOSE, TRANSPOSE_CV_INPUT, TRANSPOSE_PARAM,      36.f, 84.f},
            {BS::GP_SCALE,     SCALE_CV_INPUT,     SCALE_PARAM,          0.f,  7.f},
            {BS::GP_MODE,      MODE_CV_INPUT,      MODE_PARAM,           0.f,  3.f},
            {BS::GP_STEP_SIZE, STEP_SIZE_CV_INPUT, STEP_SIZE_PARAM,      0.f,  3.f},
            {BS::GP_LENGTH,    LENGTH_CV_INPUT,    PATTERN_LENGTH_PARAM, 1.f,  16.f},
            {BS::GP_SWING,     SWING_CV_INPUT,     SWING_PARAM,          50.f, 75.f},
            {BS::GP_GATE_LEN,  GATE_LEN_CV_INPUT,  GATE_LEN_PARAM,       50.f, 99.f},
            {BS::GP_LEGATO,    LEGATO_CV_INPUT,    LEGATO_PARAM,         0.f,  2.f},
        };
        for (int bi = 0; bi < 9; bi++) {
            const auto& b = cvBindings[bi];
            cvGlobalTimer[bi] += args.sampleTime;
            if (!inputs[b.inputId].isConnected()) continue;
            if (cvGlobalTimer[bi] < 0.02f) continue;
            float volt = clamp(inputs[b.inputId].getVoltage(), 0.f, 10.f);
            // Equal-width quantization buckets across the param's discrete state
            // count (floor, not round) -- round() gives the two endpoint values
            // only half the voltage width of every interior value.
            int numStates = (int)(b.maxV - b.minV) + 1;
            int idx = (int) clamp(std::floor((volt / 10.f) * numStates), 0.f, (float)(numStates - 1));
            int v = (int) b.minV + idx;
            if (v == state.getGlobal(b.kind)) continue;
            cvGlobalTimer[bi] = 0.f;
            state.setGlobal(b.kind, v);
            sendSysEx(BS::writeGlobal(b.kind, (uint8_t)v));
            paramQuantities[b.paramId]->setValue(v);
            knobTouchGrace[(int)b.kind] = 0.5f;
        }

        for (int i = 1; i <= 9; i++)
            if (knobTouchGrace[i] > 0.f) knobTouchGrace[i] -= args.sampleTime;

        if (stateChanged.exchange(false)) {
            for (int i = 0; i < BS::NUM_STEPS; i++)
                lights[GATE_LIGHT + i].setBrightness(state.getGate(i) ? 1.f : 0.f);

            // Push synced values into the knobs so they track the hardware instead
            // of silently going stale; each knob's onChange compares against state
            // before re-sending, so this doesn't echo a write back out. Skipped
            // per-param for a short grace window after WE just wrote that param
            // (manual knob turn or CV) -- otherwise, with the poller now running
            // every ~100ms, this push can yank a knob back to the last-confirmed
            // (already stale) hardware value faster than a manual drag can move
            // it forward, before the write+read-back round trip has caught up.
            auto pushIfIdle = [&](BS::GlobalParam kind, int paramId) {
                if (knobTouchGrace[(int)kind] > 0.f) return;
                paramQuantities[paramId]->setValue(state.getGlobal(kind));
            };
            pushIfIdle(BS::GP_CHANNEL,   CHANNEL_PARAM);
            pushIfIdle(BS::GP_TRANSPOSE, TRANSPOSE_PARAM);
            pushIfIdle(BS::GP_SCALE,     SCALE_PARAM);
            pushIfIdle(BS::GP_MODE,      MODE_PARAM);
            pushIfIdle(BS::GP_STEP_SIZE, STEP_SIZE_PARAM);
            pushIfIdle(BS::GP_LENGTH,    PATTERN_LENGTH_PARAM);
            pushIfIdle(BS::GP_SWING,     SWING_PARAM);
            pushIfIdle(BS::GP_GATE_LEN,  GATE_LEN_PARAM);
            pushIfIdle(BS::GP_LEGATO,    LEGATO_PARAM);
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "midiOut", midiOut.toJson());
        json_object_set_new(root, "midiIn",  midiIn.toJson());
        json_object_set_new(root, "selectedSaveSlot", json_integer(selectedSaveSlot));
        json_object_set_new(root, "randNoteOctMin", json_integer(randNoteOctMin));
        json_object_set_new(root, "randNoteOctMax", json_integer(randNoteOctMax));
        json_object_set_new(root, "randGateDensityPct", json_integer(randGateDensityPct));
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* j;
        j = json_object_get(root, "midiOut");
        if (j) midiOut.fromJson(j);
        j = json_object_get(root, "midiIn");
        if (j) midiIn.fromJson(j);
        j = json_object_get(root, "selectedSaveSlot");
        if (j) selectedSaveSlot = clamp((int) json_integer_value(j), 0, 15);
        j = json_object_get(root, "randNoteOctMin");
        if (j) randNoteOctMin = clamp((int) json_integer_value(j), 0, 8);
        j = json_object_get(root, "randNoteOctMax");
        if (j) randNoteOctMax = clamp((int) json_integer_value(j), 0, 8);
        j = json_object_get(root, "randGateDensityPct");
        if (j) randGateDensityPct = clamp((int) json_integer_value(j), 5, 95);
    }
};

// =============================================================================
// Widgets
// =============================================================================

static const NVGcolor INK = nvgRGB(0x23, 0x26, 0x2a);

// Generic knob for a global parameter: writes SysEx only when the user actually
// turns it (compares against the already-known state value), so the periodic
// hardware->knob sync in process() doesn't bounce straight back out as a write.
struct GlobalParamKnob : RoundSmallBlackKnob {
    BeatStepSeqModule* bsModule = nullptr;
    BS::GlobalParam kind = BS::GP_LENGTH;
    void onChange(const ChangeEvent& e) override {
        RoundSmallBlackKnob::onChange(e);
        if (!bsModule) return;
        ParamQuantity* pq = getParamQuantity();
        if (!pq) return;
        int v = (int) std::round(pq->getValue());
        if (v == bsModule->state.getGlobal(kind)) return;
        bsModule->state.setGlobal(kind, v);
        bsModule->sendSysEx(BS::writeGlobal(kind, (uint8_t)v));
        bsModule->knobTouchGrace[(int)kind] = 0.5f;
    }
    // Rack's default scroll behavior scales the step by the param's whole range,
    // so a single wheel tick can jump many steps on a wide-range param (or barely
    // move at all on a narrow one). One tick = exactly one integer step instead,
    // regardless of range.
    void onHoverScroll(const HoverScrollEvent& e) override {
        ParamQuantity* pq = getParamQuantity();
        if (!pq) return;
        float delta = (e.scrollDelta.y > 0.f) ? 1.f : -1.f;
        pq->setValue(pq->getValue() + delta);
        e.consume(this);
    }
    // Same idea for drag: Rack's default also scales pixels-per-step by the
    // param's range. Step by a fixed pixel distance instead -- but that distance
    // is itself scaled so a full sweep of the range always takes a similar total
    // drag distance, regardless of how many states there are. Otherwise a
    // uniform fixed step (e.g. 8px) makes few-state params (Legato: 3, Mode: 4)
    // feel far too sensitive -- their whole range covers barely 1-2cm of drag.
    float dragAccum = 0.f;
    void onDragStart(const DragStartEvent& e) override {
        dragAccum = 0.f;
        RoundSmallBlackKnob::onDragStart(e);
    }
    void onDragMove(const DragMoveEvent& e) override {
        ParamQuantity* pq = getParamQuantity();
        if (pq) {
            float numSteps = std::max(1.f, pq->getRange());
            float pxPerStep = clamp(160.f / numSteps, 8.f, 40.f);
            dragAccum += -e.mouseDelta.y;
            while (dragAccum >= pxPerStep) {
                pq->setValue(pq->getValue() + 1.f);
                dragAccum -= pxPerStep;
            }
            while (dragAccum <= -pxPerStep) {
                pq->setValue(pq->getValue() - 1.f);
                dragAccum += pxPerStep;
            }
        }
        ParamWidget::onDragMove(e);
    }
};

// Centered name (above) / value (below) label for a knob -- one instance of each per
// knob, both horizontally centered on the knob's own x, so it's unambiguous which
// knob a label belongs to (unlike text placed beside the knob).
struct GlobalParamNameValue : TransparentWidget {
    BeatStepSeqModule* module = nullptr;
    BS::GlobalParam kind = BS::GP_LENGTH;
    bool showValue = false; // false = parameter name (sits above the knob), true = current value (below)
    void draw(const DrawArgs& args) override {
        nvgFontSize(args.vg, 6.3f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, INK);
        if (showValue) {
            int v = module ? module->state.getGlobal(kind) : 0;
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgText(args.vg, box.size.x * 0.5f, 0, formatGlobalParam(kind, v).c_str(), nullptr);
        } else {
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
            nvgText(args.vg, box.size.x * 0.5f, box.size.y, globalParamLabel(kind), nullptr);
        }
    }
};

struct StepWidget : OpaqueWidget {
    BeatStepSeqModule* module = nullptr;
    int stepIndex = 0;

    uint8_t dispNote = 60;
    bool    dispGate = true;
    bool    highlight = false;
    float   hlTimer = 0.f;

    void step() override {
        OpaqueWidget::step();
        if (!module) return;
        // Pad shows the effective (post-transpose) pitch, not the raw stored step note --
        // transpose is a base-note offset from reference 60 (0x3C), applied on top of every
        // step. The write commands still send the raw note (see onButton/onHoverScroll/menu):
        // transpose is the hardware's own live modifier, not something to bake into storage.
        uint8_t n = module->state.getNote(stepIndex);
        bool    g = module->state.getGate(stepIndex);
        int transpose = module->state.getGlobal(BS::GP_TRANSPOSE);
        uint8_t eff = (uint8_t) clamp((int)n + (transpose - 60), 0, 127);
        if (eff != dispNote || g != dispGate) {
            dispNote = eff; dispGate = g;
            highlight = true; hlTimer = 0.4f;
        }
        if (highlight) {
            hlTimer -= APP->window->getLastFrameDuration();
            if (hlTimer <= 0.f) highlight = false;
        }
    }

    void draw(const DrawArgs& args) override {
        float w = box.size.x, h = box.size.y;

        NVGcolor bg = dispGate
            ? (highlight ? nvgRGB(0x8a, 0xe0, 0x9a) : nvgRGB(0xc9, 0xe8, 0xcd))
            : (highlight ? nvgRGB(0xd8, 0xd8, 0xd8) : nvgRGB(0xc6, 0xc7, 0xc3));
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 1, 1, w - 2, h - 2, 3);
        nvgFillColor(args.vg, bg);
        nvgFill(args.vg);

        nvgStrokeColor(args.vg, highlight ? nvgRGB(0xff, 0x8a, 0x3c) : nvgRGBA(0, 0, 0, 0x30));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgFontSize(args.vg, 7.f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 0x90));
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(args.vg, 3, 2, string::f("%02d", stepIndex + 1).c_str(), nullptr);

        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, INK);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, w / 2.f, h / 2.f + 2, BS::noteName(dispNote).c_str(), nullptr);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, w - 6, h - 6, 3);
        nvgFillColor(args.vg, dispGate ? nvgRGB(0x2a, 0x80, 0x40) : nvgRGBA(0, 0, 0, 0x25));
        nvgFill(args.vg);
    }

    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (!module) { e.consume(this); return; }
            bool ng = !module->state.getGate(stepIndex);
            module->state.setGate(stepIndex, ng);
            module->sendSysEx(BS::writeGate(stepIndex, ng));
            e.consume(this);
        }
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
            createContextMenu();
            e.consume(this);
        }
    }

    void onHoverScroll(const HoverScrollEvent& e) override {
        if (!module) return;
        int n = (int)module->state.getNote(stepIndex);
        n = clamp(n + (e.scrollDelta.y > 0 ? 1 : -1), 0, 127);
        module->state.setNote(stepIndex, (uint8_t)n);
        module->sendSysEx(BS::writeNote(stepIndex, (uint8_t)n));
        e.consume(this);
    }

    void createContextMenu() {
        if (!module) return;
        ui::Menu* menu = createMenu();
        // Writes always target the raw stored note; the pad (and the hardware)
        // then sound it shifted by whatever the live Transpose knob currently is.
        // Both are shown here since they only match when Transpose is at its
        // center (C4) -- otherwise "raw C4" can sound as something else entirely.
        int transpose = module->state.getGlobal(BS::GP_TRANSPOSE);
        uint8_t rawNote = module->state.getNote(stepIndex);
        uint8_t soundsNote = (uint8_t) clamp((int)rawNote + (transpose - 60), 0, 127);
        menu->addChild(createMenuLabel(string::f("Step %02d - raw %s, sounds %s",
            stepIndex + 1, BS::noteName(rawNote).c_str(), BS::noteName(soundsNote).c_str())));

        BeatStepSeqModule* mod = module;
        int step = stepIndex;
        bool gate = module->state.getGate(stepIndex);
        menu->addChild(createMenuItem(
            gate ? "Gate ON  -> turn off" : "Gate OFF -> turn on", "",
            [mod, step, gate]() {
                bool ng = !gate;
                mod->state.setGate(step, ng);
                mod->sendSysEx(BS::writeGate(step, ng));
            }
        ));

        menu->addChild(new MenuSeparator());
        menu->addChild(createMenuLabel("Set raw note by octave (0 = Transpose center):"));
        for (int oct = 0; oct <= 8; oct++) {
            uint8_t noteForOct = (uint8_t)((oct + 1) * 12);
            int relOct = oct - 4; // oct 4 (raw 60) is the Transpose-neutral center
            uint8_t sounds = (uint8_t) clamp((int)noteForOct + (transpose - 60), 0, 127);
            std::string octLabel = relOct == 0 ? std::string("0") : string::f("%+d", relOct);
            menu->addChild(createMenuItem(
                string::f("Oct %s -> sounds %s", octLabel.c_str(), BS::noteName(sounds).c_str()), "",
                [mod, step, noteForOct]() {
                    mod->state.setNote(step, noteForOct);
                    mod->sendSysEx(BS::writeNote(step, noteForOct));
                }
            ));
        }
    }
};

struct ConnDot : TransparentWidget {
    BeatStepSeqModule* module = nullptr;
    void draw(const DrawArgs& args) override {
        bool ok = module && (module->midiOut.getDeviceId() >= 0);
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, 5, box.size.y / 2.f, 4.f);
        nvgFillColor(args.vg, ok ? nvgRGB(0x2a, 0x80, 0x40) : nvgRGB(0xc0, 0x30, 0x30));
        nvgFill(args.vg);
        nvgFontSize(args.vg, 8.f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, INK);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, 13, box.size.y / 2.f, ok ? "Connected" : "Disconnected", nullptr);
    }
};

struct TinyLabel : Widget {
    std::string text;
    NVGcolor color = INK;
    float fontSize = 9.f;
    int align = NVG_ALIGN_LEFT;
    void draw(const DrawArgs& args) override {
        nvgFontSize(args.vg, fontSize);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, color);
        float x = (align & NVG_ALIGN_CENTER) ? box.size.x * 0.5f : 0.f;
        nvgText(args.vg, x, box.size.y * 0.5f, text.c_str(), nullptr);
    }
};

struct TextButton : OpaqueWidget {
    std::string label;
    bool pressed = false;
    std::function<void()> action;

    void draw(const DrawArgs& args) override {
        float w = box.size.x, h = box.size.y;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.3f, 0.3f, w - 0.6f, h - 0.6f, 3);
        NVGpaint grad = nvgLinearGradient(args.vg, 0, 0, 0, h, nvgRGB(0x3b, 0x3f, 0x44), nvgRGB(0x1d, 0x20, 0x23));
        nvgFillPaint(args.vg, pressed ? nvgLinearGradient(args.vg, 0, 0, 0, h, nvgRGB(0x1d, 0x20, 0x23), nvgRGB(0x3b, 0x3f, 0x44)) : grad);
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0, 0, 0, 0x80));
        nvgStrokeWidth(args.vg, 0.6f);
        nvgStroke(args.vg);
        nvgFontSize(args.vg, 8.5f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, nvgRGB(0xe9, 0xeb, 0xed));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, w / 2.f, h / 2.f, label.c_str(), nullptr);
    }
    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            pressed = true;
            if (action) action();
            e.consume(this);
        }
        if (e.action == GLFW_RELEASE) pressed = false;
    }
};

// One preset-slot button (1-16): selects the slot as the target for "Save -> Slot".
struct SlotButton : OpaqueWidget {
    BeatStepSeqModule* module = nullptr;
    int slot = 0;
    void draw(const DrawArgs& args) override {
        bool selected = module && module->selectedSaveSlot == slot;
        bool cvActive = module && module->cvSlotHighlight.load() == slot;
        float w = box.size.x, h = box.size.y;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.5f, 0.5f, w - 1, h - 1, 2.5f);
        nvgFillColor(args.vg, cvActive ? nvgRGB(0x5a, 0xc8, 0xe6) : (selected ? nvgRGB(0xff, 0xd8, 0x66) : nvgRGB(0xd6, 0xd7, 0xd3)));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, cvActive ? nvgRGB(0x0a, 0x6a, 0x8c) : (selected ? nvgRGB(0xcc, 0x99, 0x00) : nvgRGBA(0, 0, 0, 0x30)));
        nvgStrokeWidth(args.vg, (cvActive || selected) ? 1.2f : 0.6f);
        nvgStroke(args.vg);
        nvgFontSize(args.vg, 9.f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, INK);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, w / 2.f, h / 2.f, string::f("%d", slot + 1).c_str(), nullptr);
    }
    void onButton(const ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (module) module->selectedSaveSlot = slot;
            e.consume(this);
        }
    }
};

struct SelectedSlotDisplay : TransparentWidget {
    BeatStepSeqModule* module = nullptr;
    void draw(const DrawArgs& args) override {
        int slot = module ? module->selectedSaveSlot.load() : 0;
        nvgFontSize(args.vg, 9.f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFillColor(args.vg, INK);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, 0, box.size.y * 0.5f, string::f("Selected: %02d", slot + 1).c_str(), nullptr);
    }
};

// Single-line MIDI port selector (driver+device combined menu) -- see the
// identical fix applied to SysExBank: LedDisplayChoice draws its text from
// drawLayer(layer 1), not draw(), and the dark backdrop needs a parent
// app::LedDisplay or the text floats unreadably on the light panel.
struct MidiPortChoice : app::LedDisplayChoice {
    midi::Port* port = nullptr;
    std::string prefix;
    void step() override {
        if (port) {
            int deviceId = port->getDeviceId();
            text = prefix + ": " + (deviceId < 0 ? std::string("click to select") : port->getDeviceName(deviceId));
        }
        app::LedDisplayChoice::step();
    }
    // Device names from the OS (esp. ALSA "client:client MIDI n c:p") routinely run
    // much longer than the panel is wide. Drawing the raw string just let it get
    // hard-clipped mid-word with no indication anything was cut off, and always lost
    // the trailing port number -- the one part that actually disambiguates devices.
    // Truncate from the front instead, keeping the tail, with a leading ellipsis.
    static std::string fitTail(NVGcontext* vg, const std::string& full, float maxWidth) {
        float bounds[4];
        nvgTextBounds(vg, 0, 0, full.c_str(), nullptr, bounds);
        if (bounds[2] - bounds[0] <= maxWidth) return full;
        for (size_t cut = 1; cut < full.size(); cut++) {
            std::string candidate = "..." + full.substr(cut);
            nvgTextBounds(vg, 0, 0, candidate.c_str(), nullptr, bounds);
            if (bounds[2] - bounds[0] <= maxWidth) return candidate;
        }
        return "...";
    }
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;
        nvgFontSize(args.vg, 11.f);
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, color);
        std::string shown = fitTail(args.vg, text, box.size.x - 10.f);
        nvgText(args.vg, 6.f, box.size.y * 0.5f, shown.c_str(), nullptr);
    }
    void onAction(const ActionEvent& e) override {
        if (!port) return;
        ui::Menu* menu = createMenu();
        menu->addChild(createMenuLabel(prefix));
        app::appendMidiMenu(menu, port);
    }
};

// =============================================================================
// Panel background: standard light brushed-aluminum look, matching the other
// Idleness modules -- drawn procedurally, no SVG assets needed.
// =============================================================================
struct BgPanel : Widget {
    void draw(const DrawArgs& args) override {
        NVGpaint grad = nvgLinearGradient(args.vg, 0, 0, 0, box.size.y,
            nvgRGB(0xe8, 0xe8, 0xe6), nvgRGB(0xcc, 0xcd, 0xc9));
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, grad);
        nvgFill(args.vg);
    }
};

// =============================================================================
// Widget layout
// =============================================================================
// Single-column layout: step pads, then global-param knobs (one row, name/value
// centered on each knob), then Rnd/Refresh buttons, then on-device presets --
// all stacked full-width instead of side-by-side columns.
static const float MODULE_HP   = 32.f;
static const float MODULE_W_MM = MODULE_HP * 5.08f; // 162.56 mm
static const float RACK_H_MM   = 380.f / (75.f / 25.4f); // ~128.69 mm
static const float MARGIN      = 3.f;
static const float GUTTER      = 3.f;
static const float CONTENT_W = MODULE_W_MM - 2.f * MARGIN;
static const float CONTENT_X = MARGIN;

struct BeatStepSeqWidget : ModuleWidget {

    BeatStepSeqWidget(BeatStepSeqModule* module) {
        setModule(module);
        box.size = Vec(mm2px(MODULE_W_MM), RACK_GRID_HEIGHT);

        {
            auto* bg = new BgPanel;
            bg->box.size = box.size;
            addChild(bg);
        }

        float y = 2.f;
        {
            auto* lbl = new TinyLabel;
            lbl->text = "BEATSTEP SYNC";
            lbl->fontSize = 10.5f;
            lbl->align = NVG_ALIGN_CENTER;
            lbl->box.pos = Vec(0, mm2px(y));
            lbl->box.size = Vec(box.size.x, mm2px(5.f));
            addChild(lbl);

            auto* dot = new ConnDot;
            dot->module = module;
            dot->box.pos = mm2px(Vec(MODULE_W_MM - 34.f, y));
            dot->box.size = mm2px(Vec(32.f, 5.f));
            addChild(dot);
        }
        y += 6.f;

        // --- MIDI IN / MIDI OUT: one line each, full-width dark display ---
        {
            float midiW = (MODULE_W_MM - 2.f * MARGIN - GUTTER) / 2.f;
            float midiH = 7.f;

            auto* dispIn = new app::LedDisplay;
            dispIn->box.pos = mm2px(Vec(MARGIN, y));
            dispIn->box.size = mm2px(Vec(midiW, midiH));
            addChild(dispIn);
            auto* inChoice = new MidiPortChoice;
            inChoice->prefix = "MIDI IN";
            inChoice->box.size = dispIn->box.size;
            inChoice->port = module ? &module->midiIn : nullptr;
            dispIn->addChild(inChoice);

            auto* dispOut = new app::LedDisplay;
            dispOut->box.pos = mm2px(Vec(MARGIN + midiW + GUTTER, y));
            dispOut->box.size = mm2px(Vec(midiW, midiH));
            addChild(dispOut);
            auto* outChoice = new MidiPortChoice;
            outChoice->prefix = "MIDI OUT";
            outChoice->box.size = dispOut->box.size;
            outChoice->port = module ? &module->midiOut : nullptr;
            dispOut->addChild(outChoice);
        }
        y += 9.f;

        float bodyTop = y;

        // --- Square step-note pads (2x8), full content width ---
        float gapX = 1.5f, gapY = 1.5f;
        float padSide = (CONTENT_W - 7.f * gapX) / 8.f;
        for (int i = 0; i < BS::NUM_STEPS; i++) {
            int row = i / 8, col = i % 8;
            float sx = CONTENT_X + col * (padSide + gapX);
            float sy = bodyTop + row * (padSide + gapY);
            auto* sw = new StepWidget;
            sw->box.pos  = mm2px(Vec(sx, sy));
            sw->box.size = mm2px(Vec(padSide, padSide));
            sw->module   = module;
            sw->stepIndex = i;
            addChild(sw);
        }
        float gridH = 2.f * padSide + gapY;

        // --- Rnd Notes / Rnd Gates / Refresh: directly under the step grid,
        // above the knob row -- these act on the grid, the knobs are one level
        // removed from it ---
        float btnY = bodyTop + gridH + 0.8f;
        float btnH = 6.f;
        float btnW = (CONTENT_W - 10.f) / 3.f;
        {
            auto* btn = new TextButton;
            btn->label = "Rnd Notes";
            btn->box.pos = mm2px(Vec(CONTENT_X, btnY));
            btn->box.size = mm2px(Vec(btnW, btnH));
            // Handed off to the poll thread (see pendingRandomize in startPoller())
            // rather than sent here directly: writing 16 steps one by one would
            // otherwise both block the GUI thread for its whole duration and race
            // the poller's own concurrent reads of the same steps.
            btn->action = [module]() { if (module) module->pendingRandomize = 1; };
            addChild(btn);
        }
        {
            auto* btn = new TextButton;
            btn->label = "Rnd Gates";
            btn->box.pos = mm2px(Vec(CONTENT_X + btnW + 5.f, btnY));
            btn->box.size = mm2px(Vec(btnW, btnH));
            btn->action = [module]() { if (module) module->pendingRandomize = 2; };
            addChild(btn);
        }
        {
            auto* btn = new TextButton;
            btn->label = "Refresh";
            btn->box.pos = mm2px(Vec(CONTENT_X + (btnW + 5.f) * 2.f, btnY));
            btn->box.size = mm2px(Vec(btnW, btnH));
            btn->action = [module]() { if (module) module->immediateRefresh = true; };
            addChild(btn);
        }

        // --- All 9 global-param knobs in a single row, name centered above /
        // value centered below each knob -- unambiguous which knob is which. ---
        static const BS::GlobalParam order[9] = {
            BS::GP_CHANNEL, BS::GP_TRANSPOSE, BS::GP_SCALE,
            BS::GP_MODE, BS::GP_STEP_SIZE, BS::GP_LENGTH,
            BS::GP_SWING, BS::GP_GATE_LEN, BS::GP_LEGATO
        };
        static const int paramIds[9] = {
            BeatStepSeqModule::CHANNEL_PARAM, BeatStepSeqModule::TRANSPOSE_PARAM, BeatStepSeqModule::SCALE_PARAM,
            BeatStepSeqModule::MODE_PARAM, BeatStepSeqModule::STEP_SIZE_PARAM, BeatStepSeqModule::PATTERN_LENGTH_PARAM,
            BeatStepSeqModule::SWING_PARAM, BeatStepSeqModule::GATE_LEN_PARAM, BeatStepSeqModule::LEGATO_PARAM
        };
        float knobsY = btnY + btnH + 0.8f;
        float knobColW = CONTENT_W / 9.f;
        float knobLabelH = 3.2f, knobValueH = 3.2f;
        float knobGap = 0.8f; // breathing room between label text and the knob itself
        float knobRowH = knobLabelH + knobGap + 7.f + knobGap + knobValueH;
        for (int i = 0; i < 9; i++) {
            float cx = CONTENT_X + (i + 0.5f) * knobColW;

            auto* nameLbl = new GlobalParamNameValue;
            nameLbl->module = module;
            nameLbl->kind = order[i];
            nameLbl->showValue = false;
            nameLbl->box.pos  = mm2px(Vec(cx - knobColW * 0.5f, knobsY));
            nameLbl->box.size = mm2px(Vec(knobColW, knobLabelH));
            addChild(nameLbl);

            auto* knob = createParamCentered<GlobalParamKnob>(
                mm2px(Vec(cx, knobsY + knobLabelH + knobGap + 3.5f)), module, paramIds[i]);
            knob->bsModule = module;
            knob->kind = order[i];
            knob->snap = true;
            addChild(knob);

            auto* valLbl = new GlobalParamNameValue;
            valLbl->module = module;
            valLbl->kind = order[i];
            valLbl->showValue = true;
            valLbl->box.pos  = mm2px(Vec(cx - knobColW * 0.5f, knobsY + knobLabelH + knobGap + 7.f + knobGap));
            valLbl->box.size = mm2px(Vec(knobColW, knobValueH));
            addChild(valLbl);
        }

        // --- CV inputs for all 9 global params, directly below their own knob
        // column. No separate text label here -- the knob's own name above
        // already identifies the column, so the jack just sits right under it.
        static const int cvInputIds[9] = {
            BeatStepSeqModule::CHANNEL_CV_INPUT, BeatStepSeqModule::TRANSPOSE_CV_INPUT, BeatStepSeqModule::SCALE_CV_INPUT,
            BeatStepSeqModule::MODE_CV_INPUT, BeatStepSeqModule::STEP_SIZE_CV_INPUT, BeatStepSeqModule::LENGTH_CV_INPUT,
            BeatStepSeqModule::SWING_CV_INPUT, BeatStepSeqModule::GATE_LEN_CV_INPUT, BeatStepSeqModule::LEGATO_CV_INPUT
        };
        float cvRowY = knobsY + knobRowH + 0.5f;
        float cvRowH = 8.4f;
        for (int i = 0; i < 9; i++) {
            float cx = CONTENT_X + (i + 0.5f) * knobColW;
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx, cvRowY + cvRowH * 0.5f)), module, cvInputIds[i]));
        }
        float afterCvRow = cvRowY + cvRowH;

        // --- On-device preset memory (store/recall), at the bottom, full width --
        // 8x2 grid (same column count as the step grid above, visibly smaller
        // cells so it reads as secondary to the step pads).
        {
            float titleY = afterCvRow;
            auto* title = new TinyLabel;
            title->text = "BEATSTEP PRESETS (device memory)";
            title->fontSize = 6.f;
            title->align = NVG_ALIGN_CENTER;
            title->box.pos = mm2px(Vec(0, titleY));
            title->box.size = mm2px(Vec(MODULE_W_MM, 3.2f));
            addChild(title);

            float gy = titleY + 3.2f + 0.8f;
            float pGapX = 1.f, pGapY = 0.8f;
            float cellW = (CONTENT_W - 7.f * pGapX) / 8.f;
            float cellH = 4.2f; // deliberately smaller than the step pads (padSide) above
            for (int i = 0; i < 16; i++) {
                int row = i / 8, col = i % 8;
                auto* sb = new SlotButton;
                sb->module = module;
                sb->slot = i;
                sb->box.pos = mm2px(Vec(CONTENT_X + col * (cellW + pGapX), gy + row * (cellH + pGapY)));
                sb->box.size = mm2px(Vec(cellW, cellH));
                addChild(sb);
            }

            float saveY = gy + 2.f * (cellH + pGapY) + 0.8f;
            float halfW = (CONTENT_W - 2.f) / 2.f;
            {
                auto* btn = new TextButton;
                btn->label = "Save -> Slot";
                btn->box.pos = mm2px(Vec(CONTENT_X, saveY));
                btn->box.size = mm2px(Vec(halfW, 6.f));
                btn->action = [module]() {
                    if (!module) return;
                    module->sendSysEx(BS::storeBank(module->selectedSaveSlot + 1));
                };
                addChild(btn);
            }
            {
                // Manual duplicate of the CV+Trig recall path, using the same
                // selected slot as "Save -> Slot" -- recall doesn't need to be
                // CV-only, unlike save (which is deliberately never CV-triggered).
                auto* btn = new TextButton;
                btn->label = "Recall <- Slot";
                btn->box.pos = mm2px(Vec(CONTENT_X + halfW + 2.f, saveY));
                btn->box.size = mm2px(Vec(halfW, 6.f));
                btn->action = [module]() {
                    if (!module) return;
                    module->recallSlot(module->selectedSaveSlot);
                };
                addChild(btn);
            }

            float selY = saveY + 6.f + 0.6f;
            {
                auto* disp = new SelectedSlotDisplay;
                disp->module = module;
                disp->box.pos = mm2px(Vec(CONTENT_X, selY));
                disp->box.size = mm2px(Vec(CONTENT_W, 3.5f));
                addChild(disp);
            }

            float jackY = selY + 3.5f + 0.6f;
            {
                static const char* jackLabels[3] = {"Slot CV", "Recall Trig", "Clock"};
                static const int jackIds[3] = {
                    BeatStepSeqModule::SLOT_CV_INPUT, BeatStepSeqModule::RECALL_TRIG_INPUT, BeatStepSeqModule::CLOCK_INPUT
                };
                float colW = CONTENT_W / 3.f;
                for (int i = 0; i < 3; i++) {
                    auto* lbl = new TinyLabel;
                    lbl->text = jackLabels[i];
                    lbl->fontSize = 7.f;
                    lbl->align = NVG_ALIGN_CENTER;
                    lbl->box.pos = mm2px(Vec(CONTENT_X + i * colW, jackY));
                    lbl->box.size = mm2px(Vec(colW, 3.5f));
                    addChild(lbl);

                    addInput(createInputCentered<PJ301MPort>(
                        mm2px(Vec(CONTENT_X + (i + 0.5f) * colW, jackY + 7.2f)), module, jackIds[i]));
                }
            }

            auto* note = new TinyLabel;
            note->text = "Slot CV 1V/slot 0-15V; Recall on Trig edge; Clock: 1 edge = 1 MIDI tick (24 PPQN)";
            note->fontSize = 5.f;
            note->align = NVG_ALIGN_CENTER;
            note->color = nvgRGBA(0x23, 0x26, 0x2a, 0xa0);
            note->box.pos = mm2px(Vec(0, jackY + 12.2f));
            note->box.size = mm2px(Vec(MODULE_W_MM, 3.2f));
            addChild(note);
        }

        if (module) module->startPoller();
    }

    void appendContextMenu(Menu* menu) override {
        auto* m = dynamic_cast<BeatStepSeqModule*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator());
        menu->addChild(createMenuLabel("BeatStep Sync"));
        menu->addChild(createMenuItem("Force refresh", "",
            [m]() { m->immediateRefresh = true; }));
        menu->addChild(createMenuItem("Restart poller", "",
            [m]() { m->stopPoller(); m->startPoller(); }));

        menu->addChild(new MenuSeparator());
        menu->addChild(createMenuLabel("Randomization settings"));

        // Rnd Notes writes raw notes, same as the step pad's octave picker --
        // so its C0..C8 range only sounds like those names when Transpose is at
        // its center (C4). Show what it actually sounds like given the live
        // Transpose value, same as the step pad menu.
        int transpose = m->state.getGlobal(BS::GP_TRANSPOSE);
        auto soundsForOct = [transpose](int oct) -> uint8_t {
            int raw = (oct + 1) * 12;
            return (uint8_t) clamp(raw + (transpose - 60), 0, 127);
        };
        int loOct = std::min(m->randNoteOctMin, m->randNoteOctMax);
        int hiOct = std::max(m->randNoteOctMin, m->randNoteOctMax);
        menu->addChild(createMenuLabel(string::f("Rnd Notes currently sounds %s - %s (Transpose %s)",
            BS::noteName(soundsForOct(loOct)).c_str(), BS::noteName(soundsForOct(hiOct)).c_str(),
            BS::noteName((uint8_t)transpose).c_str())));

        menu->addChild(createSubmenuItem("Rnd Notes: min octave",
            string::f("C%d (sounds %s)", m->randNoteOctMin, BS::noteName(soundsForOct(m->randNoteOctMin)).c_str()),
            [m, soundsForOct](Menu* subMenu) {
                for (int oct = 0; oct <= 8; oct++) {
                    subMenu->addChild(createCheckMenuItem(
                        string::f("C%d  (sounds %s)", oct, BS::noteName(soundsForOct(oct)).c_str()), "",
                        [m, oct]() { return m->randNoteOctMin == oct; },
                        [m, oct]() { m->randNoteOctMin = oct; }
                    ));
                }
            }));

        menu->addChild(createSubmenuItem("Rnd Notes: max octave",
            string::f("C%d (sounds %s)", m->randNoteOctMax, BS::noteName(soundsForOct(m->randNoteOctMax)).c_str()),
            [m, soundsForOct](Menu* subMenu) {
                for (int oct = 0; oct <= 8; oct++) {
                    subMenu->addChild(createCheckMenuItem(
                        string::f("C%d  (sounds %s)", oct, BS::noteName(soundsForOct(oct)).c_str()), "",
                        [m, oct]() { return m->randNoteOctMax == oct; },
                        [m, oct]() { m->randNoteOctMax = oct; }
                    ));
                }
            }));

        menu->addChild(createSubmenuItem("Rnd Gates: density", string::f("%d%%", m->randGateDensityPct),
            [m](Menu* subMenu) {
                for (int pct = 10; pct <= 90; pct += 10) {
                    subMenu->addChild(createCheckMenuItem(
                        string::f("%d%%", pct), "",
                        [m, pct]() { return m->randGateDensityPct == pct; },
                        [m, pct]() { m->randGateDensityPct = pct; }
                    ));
                }
            }));
    }
};

// =============================================================================
Model* modelBeatStepSeq = createModel<BeatStepSeqModule, BeatStepSeqWidget>("BeatStepSeq");
