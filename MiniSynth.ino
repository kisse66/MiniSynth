/*
   Mini Arduino synthesizer for 16MHz ATmega328 (also fits on 168). Work in progress,
   some things not done and many could be improved. But hey it generates sound...

   Display used for this project is a 4-digit 7-segment serial LED module TDI1400.


   Based on synth engine at https://github.com/dzlonline/the_synth with several
   enhancements.
   Credits also for http://blog.dspsynth.eu/build-the-minimo-synth/, which
   inspired me to do this and provided idea for the filter.

   Copyright 2018 Krister W <kisse66@hobbylabs.org>

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
   IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
   BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
   OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
   ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <EEPROM.h>
#include <MIDI.h>
#include <TDI1400.h>
#include "synth.h"

// Pins (disp uses 6-8, PWM=9 (alternate 11))
#define LED1_PIN	10
#define LED2_PIN	5
#define BTN_PIN		2

#define BTN_DEBOUNCE	90		// ADC cycles
#define DISP_TIMER		10000	// ms
#define BTN_TIMER		750		// ms
#define MAX_SELECTOR	4		// 4 voices + SHIFT
#define NUM_POTS		6
#define POT_THRESHOLD	10		// +-

#define EEP_BLOCK		64		// space reserved for patch data + globals
#define EEP_PATCH		48		// space for synth patch

#define DEBUGPIN 3

#define SW_VERSION 1

#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif
#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif

synth syna;
MIDI_CREATE_DEFAULT_INSTANCE();
TDI1400 disp(8, 6, 7);

uint8_t selector = 0;
int16_t pots[NUM_POTS];
const uint8_t potPins[NUM_POTS] = { 0,1,2,3,6,7 };
unsigned long disptimer;
uint8_t func;
bool multitimbral = true;
uint8_t midibase = 1;


void setup() {

	wdt_reset();
	uint8_t mcusr = MCUSR;
	MCUSR = 0;
	wdt_disable();

	ADCSRA |= (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
	ADMUX = 64 + potPins[0];
	sbi(ADCSRA, ADSC);

	pinMode(LED1_PIN, OUTPUT);
	pinMode(LED2_PIN, OUTPUT);
	pinMode(BTN_PIN, INPUT_PULLUP);

	disp.begin();
	disp.print("MINI");
	disp.update();
	delay(1000);
	disp.print("SYN ");
	disp.update();
	delay(1000);

	syna.begin();

	//setupVoice( voice[0-3] , waveform[SINE,TRIANGLE,SQUARE,SAW,RAMP,NOISE] , pitch[0-127], envelope[ENVELOPE0-ENVELOPE3], length[0-127], mod[0-127, 64=no mod])
	syna.setupVoice(0, SAW, 65, ENVELOPE2, 82, 64);
	syna.setupVoice(1, TRIANGLE, 65, ENVELOPE1, 72, 64);
	syna.setupVoice(2, NOISE, 65, ENVELOPE2, 60, 32);
	syna.setupVoice(3, NOISE, 65, ENVELOPE2, 84, 64-44);

	MIDI.begin(MIDI_CHANNEL_OMNI);
	MIDI.setHandleNoteOn(NoteOnMidi);
	MIDI.setHandleNoteOff(NoteOffMidi);
	MIDI.setHandlePitchBend(Pitchbend);
	MIDI.setHandleControlChange(HandleControlChange);
	// MIDI.setHandleProgramChange  TODO?  needs changes to synth set/getDump
	// MIDI.setHandleSystemExclusive  TODO? could load pathes over MIDI
	MIDI.setHandleSystemReset(HandleSystemReset);

#ifdef DEBUGPIN
	pinMode(DEBUGPIN, OUTPUT);
#endif
}

// prepare and display selected function
void PrepFunction(int16_t val)
{
	func = val / 110; // 0-9

	switch (func) {

	case 1: // Multitimbral mode
		disp.print("MUL");
		break;
	case 2: // monotimbral
		disp.print("MONO");
		break;
	case 3: // save patch
		disp.print("SA 1");
		break;
	case 4: // save patch
		disp.print("SA 2");		
		break;
	case 5: // load patch
		disp.print("LD 1");
		break;
	case 6: // load patch
		disp.print("LD 2");
		break;
	case 7: // show SW ("PROGRAM") version
		disp.print("PR");
		disp.udec(SW_VERSION, 2);
		break;
	case 8: // reset
		disp.print("RES");		
		break;
	default:
		disp.print("NONE");
		func = 0;	// N/A
		break;
	}

	disp.update();
	disptimer = millis();
}

// execute the selected function
void DoFunction()
{
	char buf[EEP_PATCH];  // TODO change synth get/setData to work without intermediate buffer
			
	switch (func) {

	case 1: // Multitimbral 4-voice mode
		multitimbral = true;
		break;
	case 2:	// Monotimbral 4-voice
		multitimbral = false;
		break;
	case 3: // save to patch 1
		syna.getDump(buf, EEP_PATCH);
		EEPROM.put(0, buf);
		EEPROM.write(EEP_PATCH, multitimbral);
		EEPROM.write(EEP_PATCH+1, midibase);
		break;
	case 4: // save to patch 2
		syna.getDump(buf, EEP_PATCH);
		EEPROM.put(EEP_BLOCK, buf);
		EEPROM.write(EEP_BLOCK+EEP_PATCH, multitimbral);
		EEPROM.write(EEP_BLOCK+EEP_PATCH + 1, midibase);
		break;
	case 5: // load from patch 1
		EEPROM.get(0, buf);
		syna.setDump(buf, syna.getDataLength());
		multitimbral = (bool)EEPROM.read(EEP_PATCH);
		midibase = EEPROM.read(EEP_PATCH + 1);
		break;
	case 6: // load from patch 2
		EEPROM.get(EEP_BLOCK, buf);
		syna.setDump(buf, syna.getDataLength());
		multitimbral = (bool)EEPROM.read(EEP_BLOCK+EEP_PATCH);
		midibase = EEPROM.read(EEP_BLOCK + EEP_PATCH+1);
		break;
	case 8: // reset
		HandleSystemReset();

	default:
		break;
	}

	func = 0;
}

// Pot moved, update parameter
void UpdatePars(uint8_t mux, int16_t val)
{
	disp.clear();

	if (selector < MAX_SELECTOR) {

		switch (mux) {
		case 0:
			val = val / 200;
			if(multitimbral)
				syna.setWave(selector, val);	// waveform
			else {
				for(uint8_t i=0; i<4; i++)
					syna.setWave(i, val);
			}
			break;
		case 1:
			val = val / 254;
			val -= 2;
			if (multitimbral)
				syna.setOctave(selector, val);	// octave
			else {
				for (uint8_t i = 0; i<4; i++)
					syna.setOctave(i, val);
			}
			break;
		case 2:
			val = val / 255;
			if (multitimbral)
				syna.setEnvelope(selector, val);	// ENV
			else {
				for (uint8_t i = 0; i<4; i++)
					syna.setEnvelope(i, val);
			}
			break;
		case 3:
			val = val / 8;
			if (multitimbral)
				syna.setLength(selector, val);	// length 0..127
			else {
				for (uint8_t i = 0; i<4; i++)
					syna.setLength(i, val);
			}
			break;
		case 4:
			val = val / 8;
			if (multitimbral)
				syna.setMod(selector, val);		// pitch modulation, 64=off
			else {
				for (uint8_t i = 0; i<4; i++)
					syna.setMod(i, val);
			}
			val -= 64;		// show +-
			break;
		case 5:		// "Var"-pot = MOD speed	TODO range adjust
			val = val / 32 + 1;
			if (multitimbral)
				syna.setTime(selector, 5.0 / (float)val);
			else {
				for (uint8_t i = 0; i<4; i++)
					syna.setTime(i, val);
			}
			break;
		
		}
	} else {	// SHIFT on

		switch (mux) {
		case 0:	// cutoff
			val = val / 4;
			syna.setCutoff(0, val);		// common to all voices
			break;
		case 1:	// Filter Mod rate
			break;
		case 2:	// resonance
			val /= 4;
			syna.setResonance(0, val);	// common to all voices
			break;
		case 3:	// First MIDI channel
			val /= 73;	// 1-13, 0=MIDI off
			midibase = val;
			break;
		case 4:	// Filter modulation
			val = val / 8;
			syna.setFilterMod(0, val);		// filter modulation, 64=off
			val -= 64;		// show +-
			break;
		case 5:	// special functions
			PrepFunction(val);			
			return; // function cancels on display timer, commits when pressing BTN
			//break;
		}
	}

	disp.digit(0, mux + 1);
	if (val<100 && val >= 0)
		disp.dec(val, 2);
	else
		disp.dec(val, 1);

	disp.update();
	disptimer = millis();
}

void loop()
{
	static uint8_t adcMux = 0;
	static uint8_t debo = 0;
	
	MIDI.read();

	// update pots
	if (!(ADCSRA & 64)) {
		int16_t val = (ADCL + (ADCH << 8));
		if (abs(pots[adcMux] - val) >= POT_THRESHOLD) {			
			pots[adcMux] = val;
			UpdatePars(adcMux, val);
		}
		adcMux++;
		if (adcMux >= NUM_POTS)
			adcMux = 0;
		ADMUX = 64 | potPins[adcMux];
		sbi(ADCSRA, ADSC); // next conv

		// Button?
		if (!digitalRead(BTN_PIN)) { // down
			if (debo < BTN_DEBOUNCE) {
				debo++;
				if (debo >= BTN_DEBOUNCE) {
					// pressed
					selector++;
					if (selector > MAX_SELECTOR) { // out of shift
						if (func)
							DoFunction();
						selector = 0;
					}

					// show selected voice
					disp.clear();
					if (selector < MAX_SELECTOR) {
						disp.print("SO");
						disp.udec(selector, 2);
					}
					else
						disp.print("SH");

					disp.update();
					disptimer = millis();

					if (selector == MAX_SELECTOR)
						digitalWrite(LED1_PIN, 1);
					else
						digitalWrite(LED1_PIN, 0);
				}
			}
		}
		else {
			debo = 0;
		}		
	}
	
	// display timer
	if (disptimer && millis() > disptimer + DISP_TIMER) {
		disp.clear();
		if (func) { // cancel selected function
			disp.print("CNCL");
			func = 0;
			disptimer = millis();
		} else {
			disp.print("--", 1);
			disptimer = 0;
		}

		disp.update();
		digitalWrite(LED2_PIN, 0);
	}
}



void NoteOnMidi(byte channel, byte pitch, byte velocity)
{
	if (!velocity || !midibase)
		return;
	if (channel < midibase || channel > midibase + 3)
		return;

	digitalWrite(LED2_PIN, 1);

	if (multitimbral) { // default mode, one voice per channel, last note priority
			syna.mTrigger(channel-midibase, pitch);
	} else {	// one timbre, 4 voices polyphonic (ignores  channel), first note priority
		for(uint8_t i=0; i<4; i++)
			if (syna.voiceFree(i)) {
				syna.mTrigger(i, pitch);
				break;
			}
	}
}

void NoteOffMidi(byte channel, byte pitch, byte velocity)
{
	digitalWrite(LED2_PIN, 0);
}

void Pitchbend(byte channel, int bend)
{
	// TODO
}

void HandleControlChange(byte channel, byte number, byte value)
{
	// TODO
}

void HandleSystemReset()
{
	syna.disable();
	cli();
	
	asm volatile ("  jmp 0");

	/*  needs modified bootloader to avoid reset loop!
	wdt_enable(WDTO_60MS);
	for(;;) {}
	*/
}
