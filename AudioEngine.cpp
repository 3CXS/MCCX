#include "AudioEngine.h"

namespace AudioEngine {

    // ------------------ AUDIO OBJECTS ------------------

    AudioMixer4             mixV1, mixV2, mixMetro, mixMain;
    AudioOutputI2S          i2s1;
    AudioControlSGTL5000    sgtl5000_1;

    // SYNNTH

    AudioSynthWaveform      oscA[NUM_VOICES];
    AudioSynthWaveform      oscB[NUM_VOICES];
    AudioMixer4             oscMix[NUM_VOICES];

    AudioConnection*        patchCords[NUM_VOICES * 6];
    AudioConnection         patchV1(mixV1, 0, mixMain, 0);          // MAIN CH 1  
    AudioConnection         patchV2(mixV2, 0, mixMain, 1);          // MAIN CH 2

    AudioEffectBitcrusher   crusher[NUM_VOICES];
    AudioFilterStateVariable filter[NUM_VOICES];
    AudioEffectEnvelope     env[NUM_VOICES];

    // METRO
    AudioSynthWaveform      metroOsc;
    AudioEffectEnvelope     metroEnv;
    AudioConnection         patchMetro1(metroOsc, metroEnv);
    AudioConnection         patchMetro2(metroEnv, 0, mixMetro, 0);
    AudioConnection         patchMetro3(mixMetro, 0, mixMain, 2);   // MAIN CH 3    

    // OUTPUT
    AudioConnection         patchMainL(mixMain, 0, i2s1, 0);
    AudioConnection         patchMainR(mixMain, 0, i2s1, 1);   

    uint8_t voiceNote[NUM_VOICES];




    void setSynthParam(SynthParam param, float value) {
        for (int i = 0; i < NUM_VOICES; i++) {
            switch (param) {

            case SynthParam::FILTER_CUTOFF:
                filter[i].frequency(value);
                break;

            case SynthParam::FILTER_RESONANCE:
                filter[i].resonance(value);
                break;

            case SynthParam::BITCRUSH_BITS:
                crusher[i].bits((int)value);
                break;

            case SynthParam::OSC1_PULSE: {
                static const float dutyTable[] = {
                    0.125f, 0.25f, 0.5f, 0.75f
                };
                int idx = constrain((int)value, 0, 3);
                oscA[i].pulseWidth(dutyTable[idx]);
                break;
            }

            default:
                break;
            }
        }
    }

    // ------------------ PENDING BUFFER ------------------
    volatile uint8_t pend_note[64], pend_vel[64], pend_w=0, pend_r=0;

    bool popPending(uint8_t &note,uint8_t &vel) {
        noInterrupts(); 
        if(pend_r==pend_w) {
            interrupts(); 
            return false;
            } 
        note=pend_note[pend_r]; 
        vel=pend_vel[pend_r]; 
        pend_r=(pend_r+1)&PEND_MASK; 
        interrupts(); 
        return true;
    }

    void pushPending(uint8_t note,uint8_t vel) {
        uint8_t next=(pend_w+1)&PEND_MASK; 
        if(next==pend_r) return; 
        pend_note[pend_w]=note; 
        pend_vel[pend_w]=vel; 
        pend_w=next;
    }

    // Execute pending note events in main loop context
    void processAudio() {
        uint8_t n, v;
        while (popPending(n, v)) {
            if (v > 0) AudioEngine::noteOn(n, v);  // note-on
            else       AudioEngine::noteOff(n);    // note-off
        }
    }

    // ------------------ VOICE MANAGEMENT ------------------

    float midiToFreq(uint8_t note) {
        return 440.0f*pow(2.0f,(note-69)/12.0f);
    }

    int findFreeVoice() {
        for(int i=0;i<NUM_VOICES;i++) { 
            if(!env[i].isActive()) return i;
        } 
        return 0;
    }

    void noteOn(uint8_t note, uint8_t vel) {
        int v = findFreeVoice();
        float f = midiToFreq(note);

        oscA[v].frequency(f);
        oscB[v].frequency(f * 1.0f); // slight detune (or *2 for octave)

        oscA[v].amplitude(1.0);
        oscB[v].amplitude(1.0);

        env[v].noteOn();
        voiceNote[v] = note;
    }

    void noteOff(uint8_t note) { 
        for(int i=0;i<NUM_VOICES;i++){ 
            if(voiceNote[i]==note){ 
                env[i].noteOff(); 
                voiceNote[i]=255;
            }
        }
    }

    void allNotesOff() { 
        metroEnv.noteOff(); 
        for(int i=0;i<NUM_VOICES;i++){ 
            env[i].noteOff(); 
            voiceNote[i]=255;
        } 
        noInterrupts(); 
        pend_w=pend_r=0; 
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
        // BOARD 
        AudioMemory(60);
        sgtl5000_1.enable();
        sgtl5000_1.volume(0.5);

        // METRO 
        metroOsc.begin(WAVEFORM_SQUARE);
        metroOsc.frequency(2000);
        metroOsc.amplitude(0.8);

        metroEnv.attack(0);
        metroEnv.hold(1);
        metroEnv.decay(15);
        metroEnv.sustain(0);
        metroEnv.release(5);

        mixMetro.gain(0, 1.0f);

        //  SYNTH VOICES 
        int pc = 0, m1 = 0, m2 = 0;
        for (int i = 0; i < NUM_VOICES; i++) {

            oscA[i].begin(WAVEFORM_PULSE);
            oscA[i].pulseWidth(0.5);  

            oscB[i].begin(WAVEFORM_PULSE);
            oscB[i].pulseWidth(0.7);  

            oscMix[i].gain(0, 0.5);
            oscMix[i].gain(1, 0.5);

            crusher[i].bits(24);        // try 6–8
            crusher[i].sampleRate(16000); // or 8000 for dirt

            filter[i].frequency(3000);
            filter[i].resonance(0.1);

            env[i].attack(0);
            env[i].hold(5);
            env[i].decay(40);
            env[i].sustain(5);
            env[i].release(80);

            // oscA -> oscMix
            patchCords[pc++] = new AudioConnection(oscA[i], 0, oscMix[i], 0);
            // oscB -> oscMix
            patchCords[pc++] = new AudioConnection(oscB[i], 0, oscMix[i], 1);

            // oscMix -> bitcrusher
            patchCords[pc++] = new AudioConnection(oscMix[i], 0, crusher[i], 0);

            // bitcrusher -> filter
            patchCords[pc++] = new AudioConnection(crusher[i], 0, filter[i], 0);

            // filter LP -> envelope
            patchCords[pc++] = new AudioConnection(filter[i], 0, env[i], 0);

            // envelope -> voice mixer
            patchCords[pc++] = new AudioConnection(env[i], 0,
                (i < 4 ? mixV1 : mixV2),
                (i < 4 ? m1++ : m2++)
            );

        }

        //  MIXER GAINS 
        mixMain.gain(0, 0.5);           // synth mix1
        mixMain.gain(1, 0.5);           // synth mix2
        mixMain.gain(2, 0.8);           // metro

    } 

} // namespace AudioEngine