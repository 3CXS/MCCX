#include "AudioEngine.h"
#include "Sequencer.h"

namespace AudioEngine {
    
    // ------------------ AUDIO OBJECTS ------------------
    AudioMixer4             mixMain;

    AudioOutputI2S          i2s1;
    AudioControlSGTL5000    sgtl5000_1;

    // SYNTH ENGINE
    AudioConnection*        patchCordsSynth[NUM_VOICES * 4 + 4];
    AudioConnection*        patchCordsSampler[NUM_SAMPLER_VOICES * 2];

    // METRO
    AudioSynthWaveform      metroOsc;
    AudioEffectEnvelope     metroEnv;
    AudioConnection         patchMetro1(metroOsc, metroEnv);
    AudioConnection         patchMetro2(metroEnv, 0, mixMain, 2);   // MAIN CH 2 

    // OUTPUT
    AudioConnection         patchMainL(mixMain, 0, i2s1, 0);
    AudioConnection         patchMainR(mixMain, 0, i2s1, 1);   

    // ------------------ SAMPLER ------------------

    SamplerVoice samplerVoices[NUM_SAMPLER_VOICES];
    Sample samplePool[MAX_SAMPLES];

    int8_t samplerTrackVoiceMap[MAX_TRACKS][128];  // -1 = free

    void initSamplerVoiceMap() {
        for (int t = 0; t < MAX_TRACKS; t++)
            for (int n = 0; n < 128; n++)
                samplerTrackVoiceMap[t][n] = -1;
    }

    int findFreeSamplerVoice() {
        for (int i = 0; i < NUM_SAMPLER_VOICES; i++) {
            if (!samplerVoices[i].active)
                return i;
        }
        return 0; // voice steal
    }

    void loadAndAssignPad(const char* filename, uint8_t padId) {
        //if (padId >= MAX_SAMPLES) return;

        samplePool[padId].filename = filename;

        Serial.print("Assigned ");
        Serial.print(filename);
        Serial.print(" to pad ");
        Serial.println(padId);
    }

    void samplerNoteOn(uint8_t trackId, uint8_t note, uint8_t vel) {
        int sampleIdx = note-47;
        const char* fn = samplePool[sampleIdx].filename;

        if (sampleIdx < 0 || sampleIdx >= MAX_SAMPLES || !fn) {
            //Serial.printf("No sample mapped for pad %d\n", padId);
            return; 
        }
        int v = findFreeSamplerVoice();
        SamplerVoice &voice = samplerVoices[v];

        if (voice.player.isPlaying())
            voice.player.stop();

        voice.trackId = trackId;
        voice.note    = note;
        voice.sampleIndex = sampleIdx;
        voice.active  = true;

        samplerTrackVoiceMap[trackId][note] = v;

        //Serial.printf("Track %d triggered pad %d → sample %d\n", trackId, padId, sampleIdx);
        voice.player.play(samplePool[sampleIdx].filename);
    }

    void samplerNoteOff(uint8_t trackId, uint8_t padId) {
        int v = samplerTrackVoiceMap[trackId][padId];
        if (v >= 0) {
            SamplerVoice &voice = samplerVoices[v];
            if (voice.trackId == trackId) {
                voice.player.stop();
                voice.active = false;
                samplerTrackVoiceMap[trackId][padId] = -1;
                Serial.printf("Track %d released pad %d\n", trackId, padId);
            }
        }
    }

    void muteSamplerTrack(uint8_t trackId) {
        if (trackId >= MAX_TRACKS) return;

        for (int note = 0; note < 128; note++) {
            samplerNoteOff(trackId, note);
        }
    }

    // ------------------ SYNTH  ------------------
    Voice voices[NUM_VOICES];
    SynthEngine engines[MAX_ENGINES];

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

    void noteOff(uint8_t trackId, uint8_t note) {
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

    void muteTrack(uint8_t trackId) {
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

    // ------------------ ENGINES UNIFY ------------------
    void trackNoteOn(uint8_t trackId, uint8_t note, uint8_t vel) {
        if (trackId >= MAX_TRACKS) return;

         Sequencer::Track &trk = Sequencer::curSeq().tracks[trackId];

        if (!trk.active || trk.mute) return;

        switch ((Sequencer::TrackType)trk.type) {
            case Sequencer::TrackType::SYNTH:
                noteOn(trackId, note, vel);
                break;

            case Sequencer::TrackType::SAMPLER:
                samplerNoteOn(trackId, note, vel);
                break;

            default:
                break;
        }
    }

    void trackNoteOff(uint8_t trackId, uint8_t note) {
        if (trackId >= MAX_TRACKS) return;

        Sequencer::Track &trk = Sequencer::curSeq().tracks[trackId];  // ✅ FIX

        if (!trk.active) return;

        switch ((Sequencer::TrackType)trk.type) {
            case Sequencer::TrackType::SYNTH:
                noteOff(trackId, note);
                break;

            case Sequencer::TrackType::SAMPLER:
                samplerNoteOff(trackId, note);
                break;

            default:
                break;
        }
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
             Serial.printf("Pending note: track=%d pad=%d vel=%d\n", trackId, note, vel);

            if (vel > 0) {trackNoteOn(trackId, note, vel);} 
            else {trackNoteOff(trackId, note);}

        }
    }

    // ------------------ INITIALIZATION ------------------
    void init() {
        // SD-CARD
        if (!(SD.begin(SDCARD_CS_PIN))) {
            while (1) {
            Serial.println("Unable to access the SD card");
            delay(500);
            }
        }
        // ------------ AUDIO MEMORY & BOARD ---------------
        AudioMemory(160); 
        sgtl5000_1.enable(); 
        sgtl5000_1.volume(0.5f);

        // ------------------ MAIN MIXER ------------------
        mixMain.gain(0, 0.2f);              // synth
        mixMain.gain(1, 0.5f);              // sampler
        mixMain.gain(2, 0.4f);              // metro

        // ------------------ METRO ------------------
        metroOsc.begin(WAVEFORM_SQUARE);
        metroOsc.frequency(2000);
        metroOsc.amplitude(0.8f);

        metroEnv.attack(0);
        metroEnv.hold(1);
        metroEnv.decay(15);
        metroEnv.sustain(0);
        metroEnv.release(5);

        // ------------------ SAMPLER ENGINE ------------------
        initSamplerVoiceMap();

        int pcsamp = 0;  // next free patchCords index
        for (int i = 0; i < NUM_SAMPLER_VOICES; i++) {
            SamplerVoice &voice = samplerVoices[i];
            voice.active = false;
            voice.mixSample.gain(0, 1.0f); // set mixer gain
            voice.mixSample.gain(1, 0.0f);
            voice.mixSample.gain(2, 0.0f);
            voice.mixSample.gain(3, 0.0f);

            // Player -> per-voice mixer
            patchCordsSampler[pcsamp++] = new AudioConnection(voice.player, 0, voice.mixSample, 0);
            // Mixer -> main mix (use channel 0 so it goes to output)
            patchCordsSampler[pcsamp++] = new AudioConnection(voice.mixSample, 0, mixMain, 1);
        }

        // ------------------ SYNTH ENGINE ------------------
        initTrackVoiceMap();

        engines[0].crusher.bits(24);
        engines[0].crusher.sampleRate(16000);
        engines[0].filter.frequency(3000);
        engines[0].filter.resonance(0.1f);

        // ------------------ SYNTH VOICES ------------------
        int pcsynth = 0;
        for (int i = 0; i < NUM_VOICES; i++) {

            Voice& voice = voices[i];
            voice.note = 255;
            voice.active = false;
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
            patchCordsSynth[pcsynth++] = new AudioConnection(voice.oscA, 0, voice.oscMix, 0);
            patchCordsSynth[pcsynth++] = new AudioConnection(voice.oscB, 0, voice.oscMix, 1);
            patchCordsSynth[pcsynth++] = new AudioConnection(voice.oscMix, 0, voice.env, 0);
            patchCordsSynth[pcsynth++] = new AudioConnection(voice.env, 0, engines[0].mix, i % 4);
        }

        // Engine FX → main mix
        patchCordsSynth[pcsynth++] = new AudioConnection(engines[0].mix, 0, engines[0].crusher, 0);
        patchCordsSynth[pcsynth++] = new AudioConnection(engines[0].crusher, 0, engines[0].filter, 0);
        patchCordsSynth[pcsynth++] = new AudioConnection(engines[0].filter, 0, mixMain, 0);

        patchCordsSynth[pcsynth++] = new AudioConnection(engines[1].mix, 0, engines[1].crusher, 0);
        patchCordsSynth[pcsynth++] = new AudioConnection(engines[1].crusher, 0, engines[1].filter, 0);
        patchCordsSynth[pcsynth++] = new AudioConnection(engines[1].filter, 0, mixMain, 1);
    }

} // namespace AudioEngine