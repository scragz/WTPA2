// Where's the Party At?
// VERSION 2 DA EMPIRE STRIKES BLACK
//=============================
// Todd Michael Bailey
// todd@narrat1ve.com
// Tue Jul  6 19:36:23 EDT 2010

#include	"includes.h"

// Byte arrays for testing in place of an including a binary
//#include	"TMNT_SAMPLE.c"
//#include	"laugh_sample.c"

#define		CURRENT_FIRMWARE_VERSION	0x10		// Starts at 0x10 for WTPA2

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// Application Globals:
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

// ADC globals:
//-----------------------------------------------------------------------
static signed char
	adcByte;			// The current reading from the ADC.

// Keys and switch variables
//-----------------------------------------------------------------------
static unsigned char
	keyState,
	newKeys;

static unsigned char		
	encoderState,			// What the encoder switches look like.
	encoderValue,			// Incremental ticks on the encoder.
	scaledEncoderValue;		// The number that we display on the LEDs and use to select different effects and stuff.  Generated from the encoder reading.

static bool
	cardDetect;		// Is SD card physically in the slot?	

static unsigned long
	sampleLength[128];	 // 4 byte value for each sample read from block immediately after sample data
		
static unsigned char;
	sampleAssignment[8]; // Sample assignment to switches for one-shot playback

// Dummy functions for RAM access
static unsigned char ReadRAM(unsigned long address)
{
	static unsigned char
		temp;
	
	// Read memory (as of now all audio functions end with the LATCH_DDR as an output so we don't need to set it at the beginning of this function)
	LATCH_PORT=(address);				// Put the LSB of the address on the latch.
	PORTA|=(Om_RAM_L_ADR_LA);			// Strobe it to the latch output...
	PORTA&=~(Om_RAM_L_ADR_LA);			// ...Keep it there.
	
	LATCH_PORT=(address>>8);			// Put the middle byte of the address on the latch.
	PORTA|=(Om_RAM_H_ADR_LA);			// Strobe it to the latch output...
	PORTA&=~(Om_RAM_H_ADR_LA);			// ...Keep it there.
	
	PORTC=((0x88|((address>>16)&0x07)));	// Keep the switch OE high (hi z) (PC3), test pin high (PC7 used to time isrs), and the unused pins (PC4-6) low, and put the high addy bits on 0-2.
	
	LATCH_DDR=0x00;						// Turn the data bus around (AVR's data port to inputs)
	PORTA&=~(Om_RAM_OE);				// RAM's IO pins to outputs.
	
	// NOP(s) here?
	asm volatile("nop"::);				// Just in case the RAM needs to settle down
	asm volatile("nop"::);
	
	// Finish getting the byte from RAM.
	
	temp=LATCH_INPUT;					// Get the byte from this address in RAM.
	PORTA|=(Om_RAM_OE);					// Tristate the RAM.
	LATCH_DDR=0xFF;						// Turn the data bus around (AVR's data port to outputs)
	
	return temp;
}

static void WriteRAM(unsigned long address, unsigned char value)
{
	LATCH_DDR=0xFF;						// Data bus to output -- we never need to read the RAM in this version of the ISR.
	
	LATCH_PORT=(address);	// Put the LSB of the address on the latch.
	PORTA|=(Om_RAM_L_ADR_LA);								// Strobe it to the latch output...
	PORTA&=~(Om_RAM_L_ADR_LA);								// ...Keep it there.
	
	LATCH_PORT=((address>>8));	// Put the middle byte of the address on the latch.
	PORTA|=(Om_RAM_H_ADR_LA);									// Strobe it to the latch output...
	PORTA&=~(Om_RAM_H_ADR_LA);									// ...Keep it there.
	
	PORTC=(0x88|((address>>16)&0x07));	// Keep the switch OE high (hi z) (PC3), test pin high (PC7 used to time isrs), and the unused pins (PC4-6) low, and put the high addy bits on 0-2.
	
	LATCH_PORT=value;				// Put the data to write on the RAM's input port
	
	// Finish writing to RAM.
	PORTA&=~(Om_RAM_WE);				// Strobe Write Enable low.  This latches the data in.
	PORTA|=(Om_RAM_WE);					// Disbale writes.
}

// Supporting 4 x DMC channels
struct channelStruct
{
	unsigned long 
		currentAddress,
		endAddress;
	unsigned char
		sampleNumber,
		bitIndex;
	signed char
		deltaTrack;
	bool
		isPlaying;
};

static struct channelStruct dmcChannels[4];

// Find an open channel and direct it to start playing a sample
// We might need to disable interrupts before we do this
static void PlaySample(unsigned long sampleNumber)
{
	cli();
	for(int i=0; i < 4; i++)
	{
		if(dmcChannels[i].isPlaying == 0)
		{
			dmcChannels[i].isPlaying = 1;							// Let UpdateChannel() know to start playing
			dmcChannels[i].currentAddress = (sampleNumber * 4096);  // Which 4K sample? (could << 12 here, should compile that way anyway?)
			dmcChannels[i].endAddress = (dmcChannels[i].currentAddress + sampleLength[sampleNumber]); //*4096	// Use the whole bank for now - until I figure out to index
			
			// DEBUG fixed sample length
			//dmcChannels[i].endAddress = (dmcChannels[i].currentAddress + 0x400);
			// DEBUG play all of RAM
			//dmcChannels[i].endAddress = 0x80000;
			
			// Reset our other parameters
			dmcChannels[i].bitIndex = 0;
			dmcChannels[i].deltaTrack = 0;
			break;
		}
		
		// Else we're all busy - take a hike kid.
	}
	sei();
}

// DPCM implementation - 1-bit delta sample storage
// NES sample encoding works like this:
// 1) Sample IRQ causes the CPU to fetch a byte from a pointer in memory and places it in a buffer
// 2) Counter is reset - clocked by (~22KHz?) drives out a single bit at a time
// 3) Waveform DAC adjusts a fixed amount based on the bit (0=-x / 1=+x)
// 4) When counter has reached 8 and the buffer is exhausted address is incremented and the sample IRQ (fetch byte) is triggered again
// 5) Internal counter keeps track of length and ends if not in "loop mode"
static signed char UpdateChannel(unsigned char i)
{

	if(dmcChannels[i].isPlaying == 1)
	{
		if (((ReadRAM(dmcChannels[i].currentAddress) >> dmcChannels[i].bitIndex) & 0x01) == 0)		// Right shift register driven by an 8-bit counter - only interested in a single bit
		{
			dmcChannels[i].deltaTrack -= 1;	   // Apply delta change
		}
		else 
		{
			dmcChannels[i].deltaTrack += 1;
		}
		
		dmcChannels[i].bitIndex++;				// Increment counter
		
		if(dmcChannels[i].bitIndex == 8)		// If we've shifted out 8 bits fetch a new byte and reset counter
		{
			dmcChannels[i].bitIndex = 0;
			dmcChannels[i].currentAddress++;
		}
		
		if(dmcChannels[i].currentAddress == dmcChannels[i].endAddress)	// Did we reach the end?
		{
			dmcChannels[i].currentAddress = 0;							// If so, reset parameters and free the channel
			dmcChannels[i].bitIndex = 0;
			dmcChannels[i].deltaTrack = 0;
			dmcChannels[i].isPlaying = 0;
		}
		
		return(dmcChannels[i].deltaTrack);
	}
	
	else 
	{
		return 0;
	}

}

static unsigned char
	lastDacByte;	// Very possible we haven't changed output values since last time (like for instance we're recording) so don't bother strobing it out (adds noise to ADC)

static void OutputAudio(void)
{
	signed int
		sum0;				// Temporary variables for saturated adds, multiplies, other math.

	unsigned char
		output;			// What to put on the DAC
	
	sum0=UpdateChannel(0)+UpdateChannel(1)+UpdateChannel(2)+UpdateChannel(3);	// Sum everything that might be involved in our output waveform:
	
	if(sum0>127)		// Pin high.
	{
		sum0=127;
	}
	else if(sum0<-128)		// Pin low.
	{
		sum0=-128;
	}
	output=(signed char)sum0;	// Cast back to 8 bits.
	output^=(0x80);				// Make unsigned again (shift 0 up to 128, -127 up to 0, etc)

	if(output!=lastDacByte)	// Don't toggle PORTA pins if you don't have to (keep ADC noise down)
	{
		LATCH_DDR=0xFF;			// Turn the data bus around (AVR's data port to outputs)

		LATCH_PORT=output;		// Put the output on the output latch's input.
		PORTA|=(Om_DAC_LA);		// Strobe dac latch enable high -- this puts the output on the 373's output...
		PORTA&=~(Om_DAC_LA);	// ...And keeps it there.
	}

	lastDacByte=output;		// Flag this byte has having been spit out last time.
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Interrupt Vectors:
// These handle updating audio in the different banks (and the dumb LED intro)
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

ISR(TIMER1_CAPT_vect)
// The vector triggered by an external clock edge and associated with Bank0
{
	OutputAudio();				// Let's do this thang
	if(!(ADCSRA&(1<<ADSC)))		// Last conversion done (note that once we start using different clock sources it's really possible to read this too often, so always check to make sure a conversion is done)
	{
		adcByte=(ADCH^0x80);	// Update our ADC conversion variable.  If we're really flying or using both interrupt sources we may use this value more than once.  Make it a signed char.
		ADCSRA |= (1<<ADSC);  	// Start the next ADC conversion (do it here so the ADC S/H acquires the sample after noisy RAM/DAC activity on PORTA)
	}
}

ISR(PCINT2_vect)
// The vector triggered by a pin change and associated with Bank1
// It's on PC4
{
	
}

ISR(TIMER1_COMPA_vect)
// The bank0 internal timer vectors here on an interrupt.
{
	
}

ISR(TIMER1_COMPB_vect)
// The interrupt associated with bank1 when it's using internal interrupts goes here.
{
	
}

ISR(TIMER2_COMPA_vect)
// Serves exclusively to make our gay intro happen
// As far as the PWM goes, this should happen as often as possible.
{
	
}

ISR(__vector_default)
{
    //  This means a bug happened.  Some interrupt that shouldn't have generated an interrupt went here, the default interrupt vector.
	//	printf("Buggy Interrupt Generated!  Flags = ");
	//  printf("*****put interrupt register values here****");
}

static void WriteLedLatch(unsigned char theMask)
// Take the current on/off LED mask and put it onto the LED output latch.
// This and the switch handler are the only mainline (non IRQ) code that messes with the databus.
{
	unsigned char
		sreg;

	sreg=SREG;	// Store global interrupt state
	cli();		// Clear global interrupts (so we don't get an audio interrupt while messing with OE and WE)

	LATCH_PORT=theMask;				// Put passed data onto bus.
	LATCH_DDR=0xFF;					// Make sure the bus is an output.
	PORTD|=(Om_LED_LA);				// Strobe it to the latch output...
	PORTD&=~(Om_LED_LA);			// ...Keep it there.

	SREG=sreg;						// Restore interrupts.
}

static void InitLeds(void)
{
	WriteLedLatch(0);	// ...send the LED value to the latch.
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// Local Software Clock stuff.
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

void HandleSoftclock(void)
{
	if(TIFR0&(1<<TOV0))		// Got a timer overflow flag?
	{
		TIFR0 |= (1<<TOV0);		// Reset the flag (by writing a one).
		systemTicks++;			// Increment the system ticks.
	}
}

static void InitSoftclock(void)
// Wed Dec  3 22:28:06 CST 2008
// I've changed the way the softclock works from the last rev.  It's no longer and interrupt based thing, so it doesn't potentially get in the way of the audio interrupts.
// Also, since hardware TIMR1 is needed for bigger and better things than keeping human-time, we're going to set the softclock timer up to poll a flag.
// This means we don't steal cycles from any other ISRs, but it also means that if we write dumb code (if we hang the main loop for more than a tick time)
// we might miss a systemTick.
// NOTE:  w/ TMR0 running at 1/256 prescale at 20MHz, our smallest time unit is 3.2768mSecs.
// NOTE:  w/ 16-bit system ticks we can only time 214 seconds at this rate.

// With /64 those times are 0.8192 mSecs and 53 seconds.  We did this for the sake of not missing encoder states.

{
	PRR&=~(1<<PRTIM0);	// Turn the TMR0 power on.
	TIMSK0=0x00;		// Disable all Timer 0 associated interrupts.
	TCCR0A=0;			// Normal Ports.
	TCNT0=0;			// Initialize the counter to 0.
	TIFR0=0xFF;			// Clear the interrupt flags by writing ones.
	systemTicks=0;
	//	TCCR0B=0x04;		// Start the timer in Normal mode, prescaler at 1/256
	TCCR0B=0x03;		// Start the timer in Normal mode, prescaler at 1/64
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// Switch functions:
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

static void InitSwitches(void)
{
	SetTimer(TIMER_DEBOUNCE,(SECOND/32));	// Start debounce counter.

	DDRC&=~Im_CARD_DETECT;	// Card detect pin to input.
	PORTC|=Im_CARD_DETECT;	// Pull it up.
	
}

static void HandleSwitches(void)
// Read input pins, debounce, make keypresses positive-true, and flag newly-appeared keys.
// Make sure we've allowed enough time to turn the bus around.  The old RAM took a couple cycles before its data was valid, and this does too.
{
	static unsigned char
		lastKeyState;
	unsigned char
		sreg;

	if(CheckTimer(TIMER_DEBOUNCE))	// Time to read switches?
	{
		// Pause interrupts, monkey with datalatch, get switch data, resume interrupts.
		sreg=SREG;
		cli();						// Store sreg, pause interrupts.
		LATCH_PORT=0xFF;			// @@@ Pullups here seem to make the bus turn around less of a mess.
		LATCH_DDR=0x00;				// Turn the data bus around (AVR's data port to inputs)
		PORTC&=~Om_SWITCH_LA;		// Enable switch latch outputs (OE Low)
		asm volatile("nop"::);		// Needed for bus turnaround time (2 nops)
		asm volatile("nop"::);
		keyState=~LATCH_INPUT;		// Grab new keystate and invert so that pressed keys are 1s
		PORTC|=Om_SWITCH_LA;		// Tristate switch latch outputs (OE high)
		LATCH_DDR=0xFF;				// Turn the data bus rightside up (AVR gots the bus)
		SREG=sreg;					// Stop tying up interrupts		

		if(!(PINC&Im_CARD_DETECT))	// Handle SD card switch (has a real hardware pin)
		{
			cardDetect=true;
		}
		else
		{
			cardDetect=false;
		}

		SetTimer(TIMER_DEBOUNCE,(SECOND/32));	// Reset timer (allow time for bus to turn around)		
	}

	newKeys=((keyState^lastKeyState)&(keyState));		// Flag the keys which have been pressed since the last test.
	lastKeyState=keyState;								// And store this keystate as old news.
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// A/D Control Functions:
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// WTPA2 uses only one analog input (ADC0).  It's used to sample the audio input.  In old versions there were others that handled overdub and control pot reading, etc.
// The best resolution we can get from this hardware is 10 bits, +/- 2 lsbs.
// The max sampling rate we can pull at full resolution is 15kHz.  We always use the ADC single ended (so far) as the datasheet says that the ADC isn't guaranteed to
// work in differential mode with the PDIP package.  A conversion takes 13 ADC clocks normally, or 25 for the first conversion after the channel is selected.
// The datasheet is unclear how much resolution is lost above 15kHz.  Guess we'll find out!
// NOTE:  Since the RAM can only store 8 bits per sample, we're only using 8 bits of the conversion data.  Can't imagine those last two bits are going to be worth much at these sample rates anyhoo.


static void InitAdc(void)
// Note, we don't set up the Adc to trigger on anything right away.
{
	PRR&=~(1<<PRADC);		// Turn the ADC power on (clear the bit to power on).
	ADMUX = 0x60; 			// 0110 0000.  AVCC is the reference (w/ ext cap), left justify the result, start with ADC0 as the channel.  The ADC always wants to see a cap at AREF if the internal references or VCC are used.
	DIDR0 = 0x01;			// Disable the digital input for ADC0.
	ADCSRA = 0x95;			// 1001 0101 (prescaler to 32 = ~48kHz sample rate, (64 = 24kHz).  Enable the ADC, no autotrigger or interrupts. (For ex: at 8MHz sysclk and 1/64, ADC clock = 125kHz, normal conversion ~= 10kHz (13 samples per conversion).  According to the datasheet, to get max resolution from the converter, the ADC clock must be between 50 and 200kHz.)
//	ADCSRA = 0x96;			// 1001 0110 (prescaler to 64 = ~24kHz sample rate, (32 = 48kHz).  Enable the ADC, no autotrigger or interrupts. (For ex: at 8MHz sysclk and 1/64, ADC clock = 125kHz, normal conversion ~= 10kHz (13 samples per conversion).  According to the datasheet, to get max resolution from the converter, the ADC clock must be between 50 and 200kHz.)
	ADCSRA |= (1<<ADSC);  	// Start the first conversion to initialize the ADC.
	// ADCSRB controls auto-triggering, which we aren't using right now.
}

static void InitSampleClock(void)
// Get TMR1 ready to generate some equal temperament, or as equal as I can do (for MIDI)
// Or just turn it on so we can use the Input Capture pin to generate interrupts for the external clock (for the loop sampler).
{
	PRR&=~(1<<PRTIM1);	// Turn the TMR1 power on.
	TIMSK1=0x00;		// Disable all Timer 1 associated interrupts.
	OCR1A=65535;		// Set the compare register arbitrarily
	OCR1B=65535;		// Set the compare register arbitrarily
	TCCR1A=0;			// Normal Ports.
	TCCR1B=0;			// Stop the timer.
	TCNT1=0;			// Initialize the counter to 0.
	TIFR1=0xFF;			// Clear the interrupt flags by writing ones.
	
	// Bank 0 is based on Input Capture interrupts (rising edge from the sample clock oscillator)
	TCCR1B|=(1<<ICES1);		// Trigger on a rising edge.
	TIFR1|=(1<<ICF1);		// Clear Input Capture interrupt flag.
	TIMSK1|=(1<<ICIE1);		// Enable Input Capture Interrupt (yo, son, I thought I was the Icy One?)
	
	// Bank1 is based on PC4's pin change interrupt (PCINT20, which is associated with PC interrupt 2)
	PCIFR|=(1<<PCIF2);		// Clear any pending interrupts hanging around.
	PCICR=(1<<PCIE2);		// Enable the pin change interrupt for PORTC.
	PCMSK2=0x10;			// PORTC pin 4 generates interrupt
}

// Just for testing - we really don't care here
static void ClearRAM()
{
	for(int i=0; i < 0x1000; i++)
	{
		WriteRAM(i, 0);
		WriteLedLatch(i / (0x80000 >> 8));
	}
}
/*
static void PopulateRAM()
{
	unsigned char
		progressBar;
	
	for(int i=0; i < 1024; i++)
	{
		WriteRAM(i, rawDataHey[i]);
		WriteLedLatch(i / 256);
	}
	
	for(int i=0; i < 2400; i++)
	{
		WriteRAM(i+4096, rawDataLaugh[i]);
		WriteLedLatch(i / 256);
	}
}
*/
// Crappy function to indicate that something has failed in absence of a printf
// Don't leave this function (while 1) since we're not interested in going further
static void ShowFailure(unsigned char pattern)
{	
	cli();						// Why bother... it's all over..
	
	WriteLedLatch(pattern);		
	while(1)				
	{
		SetTimer(TIMER_SD,(SECOND/2));
		while (!CheckTimer(TIMER_SD))
		{
			HandleSoftclock();
		}
		pattern ^= 0xFF;		// Toggle LEDs to absolutely dazzle the user
		WriteLedLatch(pattern);
	}
}

// Junk function to expand blocks stored on uSD card 1 to 1 to RAM
static void SDtoRAM()
{
	static unsigned long 
		currentAddress;
	static unsigned char
		currentByte;
	currentAddress = 0;
	
	SetTimer(TIMER_SD,SECOND);
	while(!CheckTimer(TIMER_SD))
	{	
		HandleSoftclock();
	}
	if(SdHandshake() == false)
	{
		ShowFailure(0x55);	// Branch off here and never come back if we don't have an SD card
	}
	
	for(int blockNum = 0; blockNum < (8*128); blockNum++)	// 8 blocks * 128 samples * 512B = 512KB 
	{
		StartSdTransfer();
		
		if(SdBeginSingleBlockRead(blockNum) == true)
		{
			SetTimer(TIMER_SD,(SECOND/10));		// 100mSecs timeout
			
			while((!(CheckTimer(TIMER_SD)))&&(TransferSdByte(DUMMY_BYTE)!=0xFE))	// Wait for the start of the packet.  Could take 100mS
			{
				HandleSoftclock();	// Kludgy
			}
			
			for(int i=0; i < 512; i++)
			{
				currentByte = TransferSdByte(DUMMY_BYTE);
				WriteRAM(currentAddress, currentByte);
				currentAddress++;
			}
			TransferSdByte(DUMMY_BYTE);		// Eat a couple of CRC bytes
			TransferSdByte(DUMMY_BYTE);
			
			WriteLedLatch(blockNum / 8);	// Show progress
		}
		else 
		{
			ShowFailure(0x03);
		}
		
		while(!(UCSR1A&(1<<TXC1)))	// Spin until the last clocks go out
			;
		
		EndSdTransfer();				// Bring CS high
	}
	
	
	// BULLSHIT LENGTH CALCULATION - stored as 4-byte values on the block immediately after the 512KB sample data
	static unsigned long
		charToInt[4];

	StartSdTransfer();
	
	if(SdBeginSingleBlockRead(1024) == true)
	{
		SetTimer(TIMER_SD,(SECOND/10));		// 100mSecs timeout
		
		while((!(CheckTimer(TIMER_SD)))&&(TransferSdByte(DUMMY_BYTE)!=0xFE))	// Wait for the start of the packet.  Could take 100mS
		{
			HandleSoftclock();	// Kludgy
		}
		
		
		for(int j=0; j < 128; j++)
		{
			
			charToInt[0] = TransferSdByte(DUMMY_BYTE);
			charToInt[1] = TransferSdByte(DUMMY_BYTE);
			charToInt[2] = TransferSdByte(DUMMY_BYTE);
			charToInt[3] = TransferSdByte(DUMMY_BYTE);
			
			sampleLength[j] = (charToInt[3] << 24) | (charToInt[2] << 16) | (charToInt[1] << 8) | (charToInt[0]);	// There might be a function to do this - whatever.
			//sampleLength[j] = 0x1000;	// Debug fixed length
			
		}
		TransferSdByte(DUMMY_BYTE);		// Eat a couple of CRC bytes
		TransferSdByte(DUMMY_BYTE);
	}
	else 
	{
		ShowFailure(0x03);
	}
	
	while(!(UCSR1A&(1<<TXC1)))	// Spin until the last clocks go out
		;
		
	EndSdTransfer();				// Bring CS high
	

	UnInitSdInterface();
	WriteLedLatch(0x01);
}

// Encoder bit masks (dependent on port position)
#define	ENC_POS_A	0x00
#define	ENC_POS_B	0x40
#define	ENC_POS_C	0xC0
#define	ENC_POS_D	0x80

static void InitEncoder(void)
{
	encoderState=(PINA&Im_ENCODER_PINS);	// Initial encoder state read from pins directly.
	encoderValue=0;							// zero our relative position.
}

static void HandleEncoder(void)
// Fri Jun 24 11:29:53 EDT 2011
// Steps backwards from earlier prototype for some reason
{
	static unsigned char
	lastEncoderState=0;
	static unsigned int
	lastEncTime=0;
	
	if(systemTicks!=lastEncTime)		// Read encoder once every system tick (around 0.8 mSecs)
	{
		lastEncTime=systemTicks;					// update last read time.
		encoderState=(PINA&Im_ENCODER_PINS);		// Read encoder state from pins directly.
		
		if(encoderState!=lastEncoderState)	// Has the encoder value changed?  If so, update the value if the change looks valid.
		{
			if(encoderState==ENC_POS_A)
			{
				if(lastEncoderState==ENC_POS_D)
				{
					//					encoderValue++;
					encoderValue--;
				}
				else if(lastEncoderState==ENC_POS_B)
				{
					//					encoderValue--;
					encoderValue++;
				}
			}		
			else if(encoderState==ENC_POS_B)
			{
				if(lastEncoderState==ENC_POS_A)
				{
					//					encoderValue++;
					encoderValue--;
				}
				else if(lastEncoderState==ENC_POS_C)
				{
					//					encoderValue--;
					encoderValue++;
				}
			}		
			else if(encoderState==ENC_POS_C)
			{
				if(lastEncoderState==ENC_POS_B)
				{
					//					encoderValue++;
					encoderValue--;
				}
				else if(lastEncoderState==ENC_POS_D)
				{
					//					encoderValue--;
					encoderValue++;
				}
			}		
			else if(encoderState==ENC_POS_D)
			{
				if(lastEncoderState==ENC_POS_C)
				{
					//					encoderValue++;
					encoderValue--;
				}
				else if(lastEncoderState==ENC_POS_A)
				{
					//					encoderValue--;
					encoderValue++;
				}
			}		
			
			lastEncoderState=encoderState;		// Keep this state around to evaluate the next one.
		}		
	}
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
// Program main loop.
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

int main(void)
// Initialize this mess.
{
	PRR=0xFF;			// Power off everything, let the initialization routines turn on modules you need.
	MCUCR&=~(1<<PUD);	// Globally enable pullups (we need them for the encoder)

	// Set the DDRs for all RAM, DAC, latch related pins here.  Any non-SFR related IO pin gets initialized here.  ADC and anything else with a specific init routine will be intialized separately.

	DDRC=0xEF;			// PORTC is the switch latch OE and direct address line outputs which must be initialized here.  PC4 is the interrupt for the bank1 clock.  Pins PC5-PC7 are unused now, so pull them low.
	PORTC=0x08;			// 0, 1, 2 are the address lines, pull them low.  Pull bit 3 high to tristate the switch latch.  Pull unused pins low.

	DDRD=0x80;			// PORTD is the UART (midi) the Flash interface (SPI) the ACLK input and the LED latch enable.  Make UART and Flash input for now and the rest make appropriate (LE is an output) .
	PORTD=0x00;			// Drive the LE for the LEDs low and leave the rest floating.
	
	PORTA=0x06;			// OE and WE high, latch enables low, no pullup on AIN0, encoder now has hardware pullups.
	DDRA=0x3E;			// PORTA is digital outputs (latch strobe lines and OE/WE lines PA1-5), the analog in (on PA0) and the encoder inputs (PA6 and PA7)

	DDRB=0xFF;			// Latch port to OP.
	LATCH_PORT=128;		// And set it equal to midscale for the DAC.

	// Set the DAC to midscale to avoid pops on the first interrupt call.
	PORTA|=(Om_RAM_OE);		// Tristate the RAM.
	LATCH_DDR=0xFF;			// Turn the data bus around (AVR's data port to outputs)
	PORTA|=(Om_DAC_LA);		// Strobe dac latch enable high -- this puts the dacByte on the 373's output...
	PORTA&=~(Om_DAC_LA);	// ...And keeps it there.
	
	InitAdc();
	InitSampleClock();
	InitSoftclock();
	InitSwitches();
	InitEncoder();
	InitMidi();					// Get the MIDI stack initialized.
	InitUart0();
	InitSdInterface();
	SDtoRAM();
	
	//ClearRAM();
	//PopulateRAM();			// Temporary really - take some variables and expand them to RAM
	
	midiChannelNumberA = 0;
	midiChannelNumberB = 9;
	sei();						// THE ONLY PLACE we should globally enable interrupts in this code.

	//WriteLedLatch(0x55);
	
	static unsigned char 
		oldEncoderState,
		setMode;
	
	
	while(1)
	{
		HandleSoftclock();
		HandleSwitches();
		HandleEncoder();
		
		if(oldEncoderState != encoderValue)
		{
			oldEncoderState = encoderValue;
			WriteLedLatch(encoderValue);
			PlaySample(encoderValue);
		}
		
		// Wasteful code for these switches - just here for a demo - MIDI is where it's at
		if(newKeys&Im_SWITCH_0)
		{
			if(setMode)							// If you hit the 8th switch
			{
				sampleAssignment[0] = encoderValue;	// Take on that value
			}
			PlaySample(sampleAssignment[0]);	// Preview sound that has just been written
			WriteLedLatch(0x01);				// Indicate bank slot
			setMode = 0;					    // Clear set mode and go back to playing on press
		}
		
		// Copy pasta v
		if(newKeys&Im_SWITCH_1)
		{
			if(setMode)
			{
				sampleAssignment[1] = encoderValue;
			}
			PlaySample(sampleAssignment[1]);
			WriteLedLatch(0x02);
			setMode = 0;
		}
		
		if(newKeys&Im_SWITCH_2)
		{
			if(setMode)
			{
				sampleAssignment[2] = encoderValue;
			}
			PlaySample(sampleAssignment[2]);
			WriteLedLatch(0x04);
			setMode = 0;
		}

		if(newKeys&Im_SWITCH_3)
		{
			if(setMode)
			{
				sampleAssignment[3] = encoderValue;
			}
			PlaySample(sampleAssignment[3]);
			WriteLedLatch(0x08);
			setMode = 0;
		}
		
		// Set mode assigns samples to the switch
		if(newKeys&Im_SWITCH_7)
		{
			setMode = 1;
			WriteLedLatch(0x80);
		}
		
		
		if(Uart0GotByte())		// Handle receiving midi messages: Parse any bytes coming in over the UART into MIDI messages we can use.
		{
			HandleIncomingMidiByte(Uart0GetByte());		// Deal with the UART message and put it in the incoming MIDI FIFO if relevant.
		}
		
		static MIDI_MESSAGE
			currentMidiMessage;				// Used to point to incoming midi messages.
		
		GetMidiMessageFromIncomingFifo(&currentMidiMessage);
		
		if(currentMidiMessage.messageType==MESSAGE_TYPE_NOTE_ON)		// Note on.
		{
			WriteLedLatch(currentMidiMessage.dataByteOne);
			PlaySample(currentMidiMessage.dataByteOne);
			currentMidiMessage.messageType = IGNORE_ME;					// Crumby way to operate especially with all the information available from midi.c - but only look at the note on to be simple (drumkit)
		}
		
	}
	return(0);
}

