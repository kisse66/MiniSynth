#ifndef _SYNTH
#define _SYNTH
//-------------------------------------------------------------------------------------
// Added filter and patch functions
// (C) 2018 Krister W <kisse66@hobbylabs.org>

//*************************************************************************************
//  Arduino synth V4.1
//  Optimized audio driver, modulation engine, envelope engine.
//
//  Dzl/Illutron 2014
//
//*************************************************************************************
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include "tables.h"

#define DIFF 1
#define CHA 2
#define CHB 3

#define SINE     0
#define TRIANGLE 1
#define SQUARE   2
#define SAW      3
#define RAMP     4
#define NOISE    5

#define ENVELOPE0 0
#define ENVELOPE1 1
#define ENVELOPE2 2
#define ENVELOPE3 3

#define FS 16000.0 //20000.0                                              //-Sample rate (NOTE: must match tables.h)

#define SET(x,y) (x |=(1<<y))		        		//-Bit set/clear macros
#define CLR(x,y) (x &= (~(1<<y)))       			// |
#define CHK(x,y) (x & (1<<y))           			// |
#define TOG(x,y) (x^=(1<<y))            			//-+

volatile unsigned int PCW[4] = {
  0, 0, 0, 0};			//-Wave phase accumolators
volatile unsigned int FTW[4] = {
  1000, 200, 300, 400};           //-Wave frequency tuning words
volatile unsigned char AMP[4] = {
  255, 255, 255, 255};           //-Wave amplitudes [0-255]
volatile unsigned int PITCH[4] = {
  500, 500, 500, 500};          //-Voice pitch

volatile unsigned int EPCW[4] = {
  0x8000, 0x8000, 0x8000, 0x8000}; //-Envelope phase accumulator

volatile unsigned char divider = 4;                             //-Sample rate decimator for envelope
volatile unsigned int tim = 0;
volatile unsigned char tik = 0;
volatile unsigned char output_mode;


#define NUM_POLES	2
int16_t t[NUM_POLES+1];


// moved all patch data here for easy load/save
struct voiceData {

	volatile int MOD[4] = {				//-Voice envelope modulation [0-1023 512=no mod. <512 pitch down >512 pitch up]
		20, 0, 64, 127 };
	volatile int FMOD = 64;			// -Filter envelope modulation                      
	volatile unsigned int wavs[4];                                  //-Wave table selector [address of wave in memory]
	volatile unsigned int envs[4];                                  //-Envelopte selector [address of envelope in memory]
	volatile unsigned int EFTW[4] = {
		10, 10, 10, 10 };               //-Envelope speed tuning word

	int8_t octave[4];
	uint8_t CUTOFF = 255;
	uint8_t RESONANCE = 0;

} voiceData;

//*********************************************************************************************
//  Audio driver interrupt
//
// this code uses from ~25 (filter off) to 36 of 62.5uS (16000) on 16MHz ATmega328
//*********************************************************************************************
SIGNAL(TIMER1_COMPA_vect)
{
	PORTD |= 0x08; // PD3

  //-------------------------------
  // Time division
  //-------------------------------
  divider++;
  if(!(divider&=0x03))
    tik=1;

  //-------------------------------
  // Volume envelope generator
  //-------------------------------

  if (!(((unsigned char*)&EPCW[divider])[1]&0x80))
    AMP[divider] = pgm_read_byte(voiceData.envs[divider] + (((unsigned char*)&(EPCW[divider]+=voiceData.EFTW[divider]))[1]));
  else
    AMP[divider] = 0;

  //-------------------------------
  //  Mixer
  //-------------------------------
  int8_t sample = ((
	  (((signed char)pgm_read_byte(voiceData.wavs[0] + ((unsigned char *)&(PCW[0] += FTW[0]))[1]) * AMP[0]) >> 8) +
	  (((signed char)pgm_read_byte(voiceData.wavs[1] + ((unsigned char *)&(PCW[1] += FTW[1]))[1]) * AMP[1]) >> 8) +
	  (((signed char)pgm_read_byte(voiceData.wavs[2] + ((unsigned char *)&(PCW[2] += FTW[2]))[1]) * AMP[2]) >> 8) +
	  (((signed char)pgm_read_byte(voiceData.wavs[3] + ((unsigned char *)&(PCW[3] += FTW[3]))[1]) * AMP[3]) >> 8)
	  ) >> 2);


#if(1)
  //-------------------------------
  //  Resonating low-pass filter (shared with all voices)
  //-------------------------------
  if (voiceData.CUTOFF < 254) {

	  for (uint8_t i = 0; i < NUM_POLES; i++) {

		  int16_t x = (t[i] * (255 - voiceData.CUTOFF)) + (t[i + 1] * voiceData.CUTOFF);
		  t[i] = x >> 8;
	  }
	  t[NUM_POLES] = sample - ((t[0] * voiceData.RESONANCE) >> 8);
	  sample = t[0];
  }
#endif

  //-------------------------------
  //  DAC
  //-------------------------------
  OCR2A = OCR2B = 127 + sample;	// sample = +-127

  //************************************************
  //  Pitch modulation engine
  //************************************************
  //  FTW[divider] = PITCH[divider] + (int)   (((PITCH[divider]/64)*(EPCW[divider]/64)) /128)*MOD[divider];
  FTW[divider] = PITCH[divider] + (int)   (((PITCH[divider]>>6)*(EPCW[divider]>>6))/128)*voiceData.MOD[divider];
	tim++;

	//************************************************
	//  Filter modulation engine
	//************************************************

		// TODO

	PORTD &= ~(0x08);
}

class synth
{
private:

public:

  synth()
  {
  }

  //*********************************************************************
  //  Startup default
  //*********************************************************************

  void begin()
  {
    output_mode=CHA;
    TCCR1A = 0x00;                                  //-Start audio interrupt
    TCCR1B = 0x09;
    TCCR1C = 0x00;
    OCR1A=16000000.0 / FS;			    //-Auto sample rate
    SET(TIMSK1, OCIE1A);                            //-Start audio interrupt
    sei();                                          //-+

    TCCR2A = 0x83;                                  //-8 bit audio PWM
    TCCR2B = 0x01;                                  // |
    OCR2A = 127;                                    //-+
    SET(DDRB, 3);				    //-PWM pin
  }

  //*********************************************************************
  //  Startup fancy selecting varoius output modes
  //*********************************************************************

  void begin(unsigned char d)
  {
    TCCR1A = 0x00;                                  //-Start audio interrupt
    TCCR1B = 0x09;
    TCCR1C = 0x00;
    OCR1A=16000000.0 / FS;			    //-Auto sample rate
    SET(TIMSK1, OCIE1A);                            //-Start audio interrupt
    sei();                                          //-+

    output_mode=d;

    switch(d)
    {
    case DIFF:                                        //-Differntial signal on CHA and CHB pins (11,3)
      TCCR2A = 0xB3;                                  //-8 bit audio PWM
      TCCR2B = 0x01;                                  // |
      OCR2A = OCR2B = 127;                            //-+
      SET(DDRB, 3);				      //-PWM pin
      SET(DDRD, 3);				      //-PWM pin
      break;

    case CHB:                                         //-Single ended signal on CHB pin (3)
      TCCR2A = 0x23;                                  //-8 bit audio PWM
      TCCR2B = 0x01;                                  // |
      OCR2A = OCR2B = 127;                            //-+
      SET(DDRD, 3);				      //-PWM pin
      break;

    case CHA:
    default:
      output_mode=CHA;                                //-Single ended signal in CHA pin (11)
      TCCR2A = 0x83;                                  //-8 bit audio PWM
      TCCR2B = 0x01;                                  // |
      OCR2A = OCR2B = 127;                            //-+
      SET(DDRB, 3);				      //-PWM pin
      break;

    }
  }

  //*********************************************************************
  //  Timing/sequencing functions
  //*********************************************************************

  unsigned char synthTick(void)
  {
    if(tik)
    {
      tik=0;
      return 1;  //-True every 4 samples
    }
    return 0;
  }

  unsigned char voiceFree(unsigned char voice)
  {
    if (!(((unsigned char*)&EPCW[voice])[1]&0x80))
      return 0;
    return 1;
  }


  //*********************************************************************
  //  Setup all voice parameters in MIDI range
  //  voice[0-3],wave[0-6],pitch[0-127],envelope[0-4],length[0-127],mod[0-127:64=no mod]
  //*********************************************************************

  void setupVoice(unsigned char voice, unsigned char wave, unsigned char pitch, unsigned char env, unsigned char length, unsigned int mod)
  {
    setWave(voice,wave);
    setPitch(voice,pitch);
    setEnvelope(voice,env);
    setLength(voice,length);
    setMod(voice,mod);
  }

  //*********************************************************************
  //  Setup wave [0-6]
  //*********************************************************************

  void setWave(unsigned char voice, unsigned char wave)
  {
    switch (wave)
    {
    case TRIANGLE:
		voiceData.wavs[voice] = (unsigned int)TriangleTable;
      break;
    case SQUARE:
		voiceData.wavs[voice] = (unsigned int)SquareTable;
      break;
    case SAW:
		voiceData.wavs[voice] = (unsigned int)SawTable;
      break;
    case RAMP:
		voiceData.wavs[voice] = (unsigned int)RampTable;
      break;
    case NOISE:
		voiceData.wavs[voice] = (unsigned int)NoiseTable;
      break;
    default:
		voiceData.wavs[voice] = (unsigned int)SinTable;
      break;
    }
  }
  //*********************************************************************
  //  Setup Pitch [0-127]
  //*********************************************************************

  void setPitch(unsigned char voice,unsigned char MIDInote)
  {
    PITCH[voice]=pgm_read_word(&PITCHS[MIDInote]);
  }

  //*********************************************************************
  //  Setup Envelope [0-4]
  //*********************************************************************

  void setEnvelope(unsigned char voice, unsigned char env)
  {
    switch (env)
    {
    case 1:
		voiceData.envs[voice] = (unsigned int)Env0;
      break;
    case 2:
		voiceData.envs[voice] = (unsigned int)Env1;
      break;
    case 3:
		voiceData.envs[voice] = (unsigned int)Env2;
      break;
    case 4:
		voiceData.envs[voice] = (unsigned int)Env3;
      break;
    default:
		voiceData.envs[voice] = (unsigned int)Env0;
      break;
    }
  }

  //*********************************************************************
  //  Setup Length [0-128]
  //*********************************************************************

  void setLength(unsigned char voice,unsigned char length)
  {
	  voiceData.EFTW[voice]=pgm_read_word(&EFTWS[length]);
  }

  //*********************************************************************
  //  Setup pitch mod
  //*********************************************************************

  void setMod(unsigned char voice,unsigned char mod)
  {
    //    MOD[voice]=(unsigned int)mod*8;//0-1023 512=no mod
	  voiceData.MOD[voice]=(int)mod-64;//0-1023 512=no mod
  }

  //*********************************************************************
  //  Setup filter mod
  //*********************************************************************

  void setFilterMod(unsigned char voice, unsigned char mod)
  {	 
	  // filter is now shared, maybe could be optimized to have time for each voice?

	  voiceData.FMOD = (int)mod - 64;
  }

  //*********************************************************************
  //  Midi trigger
  //*********************************************************************

  void mTrigger(unsigned char voice,unsigned char MIDInote)
  {
	MIDInote += voiceData.octave[voice]; // TODO range check  
	PITCH[voice]=pgm_read_word(&PITCHS[MIDInote]);
    EPCW[voice]=0;
    FTW[divider] = PITCH[voice] + (int)   (((PITCH[voice]>>6)*(EPCW[voice]>>6))/128)*voiceData.MOD[voice];
  }

  //*********************************************************************
  //  Set frequency direct
  //*********************************************************************

  void setFrequency(unsigned char voice,float f)
  {
    PITCH[voice]=f/(FS/65535.0);

  }

  //*********************************************************************
  //  Set time
  //*********************************************************************

  void setTime(unsigned char voice,float t)
  {
	  voiceData.EFTW[voice]=(1.0/t)/(FS/(32767.5*10.0));//[s];
  }


  //*********************************************************************
  //  Set cutoff (0-255)
  //*********************************************************************

  void setCutoff(unsigned char voice, uint8_t cutoff)
  {
	  voiceData.CUTOFF = cutoff;
  }

  //*********************************************************************
  //  Set resonance (0-255)
  //*********************************************************************

  void setResonance(unsigned char voice, uint8_t res)
  {
	  voiceData.RESONANCE = res;
  }

  //*********************************************************************
  //  Set octave
  //*********************************************************************

  void setOctave(unsigned char voice, int8_t oct)
  {
	  if(voice < 4)
		  voiceData.octave[voice] = oct * 12;
  }

  //*********************************************************************
  //  Save voice data
  //*********************************************************************

  void getDump(char *data, uint8_t len)
  {
	  if (len >= sizeof(voiceData)) {

		  struct voiceData *dptr = (struct voiceData *)data;
		  *dptr = voiceData;
	  }
  }

  //*********************************************************************
  //  Load voice data
  //*********************************************************************

  void setDump(char *data, uint8_t len)
  {
	  if (len == sizeof(voiceData)) {

		  struct voiceData *dptr = (struct voiceData *)data;
		  voiceData = *dptr;
	  }
  }

  //*********************************************************************
  //  Return length of voice data
  //*********************************************************************

  uint8_t getDataLength()
  {
	  return sizeof(voiceData);	// just a binary dump. Now 40 bytes
  }


  //*********************************************************************
  //  Simple trigger
  //*********************************************************************

  void trigger(unsigned char voice)
  {
    EPCW[voice]=0;
    FTW[voice]=PITCH[voice];
    //    FTW[voice]=PITCH[voice]+(PITCH[voice]*(EPCW[voice]/(32767.5*128.0  ))*((int)MOD[voice]-512));
  }

  //*********************************************************************
  //  Suspend/resume synth
  //*********************************************************************

  void suspend()
  {
    CLR(TIMSK1, OCIE1A);                            //-Stop audio interrupt
  }
  void resume()
  {
    SET(TIMSK1, OCIE1A);                            //-Start audio interrupt
  }

  void disable()	// stop all (e.g. for reset). Needs new begin() to resume
  {
	  suspend();
	  TCCR2A = 0;
	  TCCR2B = 0;
  }

};

#endif