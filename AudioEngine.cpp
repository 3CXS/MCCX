#include "AudioEngine.h"
#include "Sequencer.h"

namespace AudioEngine {
    
    Voice       voices[NUM_VOICES];
    SynthEngine engines[MAX_ENGINES];

    // ------------------ AUDIO OBJECTS ------------------

    AudioMixer4             mixMain;
    AudioMixer4             mixMetro;

    AudioOutputI2S          i2s1;
    AudioControlSGTL5000    sgtl5000_1;

    // SYNTH ENGINE
    AudioConnection* patchCords[NUM_VOICES * 4 + 4];

    // METRO
    AudioSynthWaveform      metroOsc;
    AudioEffectEnvelope     metroEnv;
    AudioConnection         patchMetro1(metroOsc, metroEnv);
    AudioConnection         patchMetro2(metroEnv, 0, mixMetro, 0);
    AudioConnection         patchMetro3(mixMetro, 0, mixMain, 2);   // MAIN CH 3    

    // OUTPUT
    AudioConnection         patchMainL(mixMain, 0, i2s1, 0);
    AudioConnection         patchMainR(mixMain, 0, i2s1, 1);   

    // ------------------ PARAMETERS  ------------------
    void setMainParam(EncParam param, float value) {
        switch (param) {
            case EncParam::MAIN_VOL: {
                mixMain.gain(0, value);
                mixMain.gain(1, value);
                break;
            }
            default:
                break;
        }
    }

    void setSynthParam(EncParam param, float value) {
        switch (param) {

            case EncParam::FILTER_CUTOFF:
                engines[0].filter.frequency(value);
                break;

            case EncParam::FILTER_RESONANCE:
                engines[0].filter.resonance(value);
                break;

            case EncParam::BITCRUSH_BITS:
                engines[0].crusher.bits((int)value);
                break;

            case EncParam::OSC1_PULSE: {
                static const float dutyTable[] = {
                    0.125f, 0.25f, 0.5f, 0.75f
                };
                int idx = constrain((int)value, 0, 3);
                for (int i = 0; i < NUM_VOICES; i++) {
                    voices[i].oscA.pulseWidth(dutyTable[idx]);
                    voices[i].oscB.pulseWidth(dutyTable[idx]);
                }
                break;
            }

            case EncParam::ENV_ATT:
                for (int i = 0; i < NUM_VOICES; i++)
                    voices[i].env.attack(value);
                break;

            case EncParam::ENV_DEC:
                for (int i = 0; i < NUM_VOICES; i++)
                    voices[i].env.decay(value);
                break;

            case EncParam::ENV_SUS:
                for (int i = 0; i < NUM_VOICES; i++)
                    voices[i].env.sustain(value);
                break;

            case EncParam::ENV_REL:
                for (int i = 0; i < NUM_VOICES; i++)
                    voices[i].env.release(value);
                break;
            
            default:
                break;

        }
    }

    // ------------------ PENDING BUFFER ------------------
    volatile uint8_t pend_note[64];
    volatile uint8_t pend_vel[64];
    volatile uint8_t pend_track[64];

    volatile uint8_t pend_w = 0;
    volatile uint8_t pend_r = 0;

    void pushPending(uint8_t trackId, uint8_t note, uint8_t vel) {
        uint8_t next = (pend_w + 1) & PEND_MASK;
        if (next == pend_r) return; // buffer full

        pend_track[pend_w] = trackId;
        pend_note[pend_w]  = note;
        pend_vel[pend_w]   = vel;
        pend_w = next;
    }

    bool popPending(uint8_t &trackId, uint8_t &note, uint8_t &vel) {
        noInterrupts();
        if (pend_r == pend_w) {
            interrupts();
            return false;
        }

        trackId = pend_track[pend_r];
        note    = pend_note[pend_r];
        vel     = pend_vel[pend_r];
        pend_r  = (pend_r + 1) & PEND_MASK;

        interrupts();
        return true;
    }

    void processAudio() {
        uint8_t trackId, note, vel;

        while (popPending(trackId, note, vel)) {
            if (vel > 0)
                noteOn(trackId, note, vel);
            else
                noteOff(trackId, note);
        }
    }

    // ------------------ VOICE MANAGEMENT ------------------
    int8_t trackVoiceMap[MAX_TRACKS][128];

    void initTrackVoiceMap() {
        for (int t = 0; t < MAX_TRACKS; t++)
            for (int n = 0; n < 128; n++)
                trackVoiceMap[t][n] = -1;
    }

    float midiToFreq(uint8_t note) {return 440.0f * pow(2.0f, float(note - 69) / 12.0f);}

    int findFreeVoice() {
        for (int i = 0; i < NUM_VOICES; i++) {
            if (!voices[i].env.isActive())
                return i;
        }
        return 0; // simple voice steal
    }


    void noteOn(uint8_t trackId, uint8_t note, uint8_t vel) {

        if (trackId >= MAX_TRACKS) return;

        int v = findFreeVoice();
        Voice& voice = voices[v];

        voice.note   = note;
        voice.active = true;
        voice.trackId  = trackId;

        float f = midiToFreq(note);
        float amp = vel / 127.0f;
        voice.oscA.frequency(f);
        voice.oscB.frequency(f * 2.0f);   // octave up (or detune later)
        voice.oscA.amplitude(amp);
        voice.oscB.amplitude(amp);

        voice.env.noteOn();
        trackVoiceMap[trackId][note] = v;
    }

    void noteOff(uint8_t trackId, uint8_t note)
    {
        if (trackId >= MAX_TRACKS) return;

        int v = trackVoiceMap[trackId][note];
        if (v >= 0) {
            Voice& voice = voices[v];
            voice.env.noteOff();
            voice.note   = 255;
            voice.active = false;

            trackVoiceMap[trackId][note] = -1;
        }
    }

    void muteTrack(uint8_t trackId)
    {
        if (trackId >= MAX_TRACKS) return;

        for (int note = 0; note < 128; note++) {
            noteOff(trackId, note);
        }
    }

    void allNotesOff() {
        metroEnv.noteOff();
        for (int t = 0; t < MAX_TRACKS; t++)
            muteTrack(t);

        // Clear pending buffer
        noInterrupts();
        pend_w = pend_r = 0;
        interrupts();
    }

    // ------------------ METRO ------------------
    const float METRO_VOLUME = 1.0;

    void Metro(uint32_t tick) {
        if (tick % PPQN != 0) return;   // only quarter notes
        bool downbeat = ((tick / PPQN) % BEATS_PER_BAR) == 0;

        if (downbeat) {
            metroOsc.frequency(1500);
            metroOsc.amplitude(1.0f);
        } else {
            metroOsc.frequency(800);
            metroOsc.amplitude(0.7f);
        }

        metroEnv.noteOn();
    }

    // ------------------ INITIALIZATION ------------------

    void init() {

        // ------------------ AUDIO MEMORY & BOARD ------------------
        AudioMemory(60);                     // allocate enough blocks
        sgtl5000_1.enable();                // enable codec
        sgtl5000_1.volume(0.5f);            // default volume

        // ------------------ MAIN MIXER ------------------
        mixMain.gain(0, 0.2f);              // synth
        mixMain.gain(1, 0.4f);              // metro or other
        mixMain.gain(2, 0.0f);              // reserved

        // ------------------ METRO ------------------
        metroOsc.begin(WAVEFORM_SQUARE);
        metroOsc.frequency(2000);
        metroOsc.amplitude(0.8f);

        metroEnv.attack(0);
        metroEnv.hold(1);
        metroEnv.decay(15);
        metroEnv.sustain(0);
        metroEnv.release(5);

        mixMetro.gain(0, 1.0f);

        // Patch metro chain
        patchCords[0] = new AudioConnection(metroOsc, metroEnv);
        patchCords[1] = new AudioConnection(metroEnv, 0, mixMetro, 0);
        patchCords[2] = new AudioConnection(mixMetro, 0, mixMain, 1);

        // ------------------ SYNTH ENGINE ------------------
        // Engine FX: one filter + one bitcrusher
        engines[0].crusher.bits(24);
        engines[0].crusher.sampleRate(16000);
        engines[0].filter.frequency(3000);
        engines[0].filter.resonance(0.1f);

        // ------------------ SYNTH VOICES ------------------
        int pc = 3;  // next free patchCords index
        for (int i = 0; i < NUM_VOICES; i++) {

            Voice& voice = voices[i];
            voice.note = 255;
            voice.active = false;
            voice.engineId = 0;

            // Oscillators
            voice.oscA.begin(WAVEFORM_PULSE);
            voice.oscA.pulseWidth(0.5f);
            voice.oscB.begin(WAVEFORM_PULSE);
            voice.oscB.pulseWidth(0.7f);

            // Envelope
            voice.env.attack(0);
            voice.env.decay(10);
            voice.env.sustain(0.5);
            voice.env.release(20);

            // OscMix for this voice
            voice.oscMix.gain(0, 0.5);
            voice.oscMix.gain(1, 0.5);

            // ------------------ PATCHING VOICE ------------------
            patchCords[pc++] = new AudioConnection(voice.oscA, 0, voice.oscMix, 0);
            patchCords[pc++] = new AudioConnection(voice.oscB, 0, voice.oscMix, 1);
            patchCords[pc++] = new AudioConnection(voice.oscMix, 0, voice.env, 0);
            patchCords[pc++] = new AudioConnection(voice.env, 0, engines[0].mix, i % 4);
        }

        // Engine FX → main mix
        patchCords[pc++] = new AudioConnection(engines[0].mix, 0, engines[0].crusher, 0);
        patchCords[pc++] = new AudioConnection(engines[0].crusher, 0, engines[0].filter, 0);
        patchCords[pc++] = new AudioConnection(engines[0].filter, 0, mixMain, 0);

        // ------------------ OUTPUT ------------------
        patchMainL = AudioConnection(mixMain, 0, i2s1, 0);
        patchMainR = AudioConnection(mixMain, 0, i2s1, 1);
    }


} // namespace AudioEngine