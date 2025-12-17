//////////////////////////////////////////////////////////////////////////////////
// MCCX - 001.01
/*////////////////////////////////////////////////////////////////////////////////
                                      ---
                      - SEQUENCER SAMPLER SYNTH MIDI CONTROL - 
            TEENSY MICROCONTROLLER BASED AUDIO LIVE & STUDIO WORKSTATION
                          Legacy devices source inspired 
                        AKAI MPC JJ-OS / LSDJ / MIDIBOX SID 
                             ----- EVERNESS ------                           
  FEATURES >
  --------------------------------------------------------------------------------
    SEQUENCER 96 PPQN
    32 TRACKS [TYPE> Sampler / Synth / Granular / Perc&Noise]
    LIVE SEQUENCING Quantice, Noterepeat
    STEP SEQUENCING
    PER TRACK FX
    MASTER EQ & COMPRESSION
    ARPEGGIATOR      
    MODULATIONS & RANDOMS
    ONE STEP UNDO/REDO
    COPY/PASTE
    SONG MODE
    PROJECT AND SAMPLES FILESYSTEM
    UNIFIED MINIMALISTIC UI

  HARDWARE SETUP >
  --------------------------------------------------------------------------------
    - TEENSY 4.1
    - AUDIOBOARD REV C 
    - NEXTION NX8048P050-011C    SERIAL6 TX 24 / RX 25   	800×480px
    - 2x NEOTRELLIS BUTTONPAD    IC2   SCL1/SDA1
    - 2x ENCODERBOARD            IC2   SCL1/SDA1
    - 16CH Analog Multiplexer    SIG-32 / S0-28 / S1-29 / S2-30 / S3-31
    - Logic Level Converter 4-Kanal 5V-3.3V
  --------------------------------------------------------------------------------
    IC2
      Wire  SCL0 19      --> AudioBoard
            SDA0 18

      Wire1 SCL1 16      --> ButtonPads
            SDA1 17

          Adafruit_seesaw.cpp   --> Change &Wire to &Wire1
          Adafruit_NeoTrellis.h --> Change &Wire to &Wire1

    BUTTON PAD ADRESS:
      Board1  (default)   0x2E  46
      Board2  A0 (+1)     0x2F  47

    ENCODER BOARDS
      Board1  (default)   0x49  73
      Board2  A0 (+1)     0x4A  74

  DATA STRUCTURE >
  --------------------------------------------------------------------------------
    PROJECT
      SEQ 1 ...         Length / BPM / LOOP
        TRACK 1 - 32    TYPE[SAMP,SYNTH,GRAIN,PERC] / MUTE / MIDI-CH
          PROGRAM       SAMP  - SampleAllocation / Gain / Pitch / Filter / Bit
                        SYNTH - EngineParams / NoteScale / ADSR / GAIN / FX
                        GRAIN - EngineParams / NoteScale / ADSR / GAIN / FX
                        PERC  - EngineParams / NoteScale / ADSR / GAIN / FX
          PATTERN       MIDI: Note / Velocity / 4 x Modulation
      SONG              Sequence / Repeat / BPM

  AUDIO ENGINE >
  --------------------------------------------------------------------------------
    TRACK TYPE:
      Sampler 
      Synth - PULSE / WAVETABLE
      Granular - Monophonic 8bit Samples
      Perc&Noise - Drum Synthesis / Drone
      Midi - Note/Vel CC NRPN SYSEX

    PER TRACK FX:
      GAIN/DRIVE - Gain,Over
      FILTER     - Type,Freq,Res,Mod
      DELAY      - Dry/Wet,Time,Feedback,Mod,Sync, + EQ Low/High/Spread
      REVERB     - Dry/Wet,Time,Feedback,Mod
      BITCRUSH   - Bit, Mod

    MASTER FX:
      EQ         - Low/Mid/High 
      COMP       - Gain,Ratio,Attack,Release,

  UI >
  --------------------------------------------------------------------------------
    DISPLAY
    MAIN ENCODER
    NAVIGATION BUTTONS x4
    BUTTONPAD 32 Buttons
    ENCODERS x8
    FUNCTION BUTTONS x15

    MENUSTRUCTURE:
      Sequence / Track / Song
      Notescale
      Velocity
      Modulation/Random
      FX
      Quant/Arp/Repeat
      Program
      Instrument
      Filesystem
      Settings
  
////////////////////////////////////////////////////////////////////////////////*/
