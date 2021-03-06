///////////////////////////////////////////////////////////////////////////////
// MCUCPP formatted IO demo.
// Target hardware: MSP430 Launchpad with MSP430G2231 MCU,
//		HD44780 compatible display. External watch cristal is connected
//		to MCU to calibrate internal DCO.
// Description:
// Current temperature is measured with MSP430G2231 built-in temperature sensor,
// displayed on LCD and send via USART using MCUCPP formatted IO capability.
// Target pins description:
// P1.0 -
// P1.1 -
// P1.2 -
// P1.3 -
// P1.4 -
// P1.5 -
// P1.6 -
// P1.7 -
///////////////////////////////////////////////////////////////////////////////

#include <io.h>
#include <signal.h>
#include <isr_compat.h>

// mcucpp headers
#include <iopins.h>
#include <delay.h>
#include <tiny_ostream.h>
#include <tiny_iomainp.h>
#include <format_parser.h>
#include <drivers/HD44780.h>

using namespace IO;

class MyStreamBase
{
    public:

    virtual void put(char value)=0;
    // writes block of data
    void write(const char *ptr, size_t size)
    {
        for(size_t i=0; i<size; i++)
            put(ptr[i]);
    }
};

// Dummy TX only software USART
template<class TxPin, uint32_t Baud, class OstreamClass>
class SoftUsart :public OstreamClass
{
    static const uint32_t BitDelay = 1000000000/ Baud;
    public:
    // Writes one char to USART
    void put(char value)
    {
        if(value == '\n')
			put('\r');
        TxPin::SetConfiguration(TxPin::Port::Out);
        // start bit
        TxPin::Clear();
        Util::delay_ns<BitDelay, F_CPU>();
        for(uint8_t i = 0; i < 8; i++)
        {
            // data bits
            TxPin::Set((value & 1) );
            value >>= 1;
            Util::delay_ns<BitDelay, F_CPU>();
        }
        // stop bits
        TxPin::Set();
        Util::delay_ns<BitDelay*5, F_CPU>();
    }
};

template<class Display, class OstreamClass>
class LcdStream : public OstreamClass
{
    public:
    LcdStream()
    {
        Display::Init();
        _position = 0;
    }

    void put(char value)
    {
        if(value == '\n')
        {
            Display::Home();
            _position = 0;
        }
        else
        {
            Display::Putch(value);
            _position++;
        }
        if(_position == Display::LineWidth())
			Display::Goto(0, 1);
    }
private:
    uint8_t _position;
};

static void Set_DCO(unsigned int Delta)            // Set DCO to selected frequency
{
  unsigned int Compare, Oldcapture = 0;

  BCSCTL1 |= DIVA_3;                        // ACLK = LFXT1CLK/8
  TACCTL0 = CM_1 + CCIS_1 + CAP;            // CAP, ACLK
  TACTL = TASSEL_2 + MC_2 + TACLR;          // SMCLK, cont-mode, clear

  while (1)
  {
    while (!(CCIFG & TACCTL0));             // Wait until capture occured
    TACCTL0 &= ~CCIFG;                      // Capture occured, clear flag
    Compare = TACCR0;                       // Get current captured SMCLK
    Compare = Compare - Oldcapture;         // SMCLK difference
    Oldcapture = TACCR0;                    // Save current captured SMCLK

    if (Delta == Compare)
      break;                                // If equal, leave "while(1)"
    else if (Delta < Compare)
    {
      DCOCTL--;                             // DCO is too fast, slow it down
      if (DCOCTL == 0xFF)                   // Did DCO roll under?
        if (BCSCTL1 & 0x0f)
          BCSCTL1--;                        // Select lower RSEL
    }
    else
    {
      DCOCTL++;                             // DCO is too slow, speed it up
      if (DCOCTL == 0x00)                   // Did DCO roll over?
        if ((BCSCTL1 & 0x0f) != 0x0f)
          BCSCTL1++;                        // Sel higher RSEL
    }
  }
  TACCTL0 = 0;                              // Stop TACCR0
  TACTL = 0;                                // Stop Timer_A
  BCSCTL1 &= ~DIVA_3;                       // ACLK = LFXT1CLK
}

static void SetUpClock()
{
	P2_6::SetConfiguration(Port2::AltOut);
	P2_7::SetConfiguration(Port2::AltOut);
	Set_DCO(F_CPU / 4096);
}

typedef FormatWriter<MyStreamBase> AbstractStream;

typedef SoftUsart<P1_1, 9600, AbstractStream> MyUsartStream;

typedef Lcd<
        P1_0,   // RS
        NullPin,// RW
        P1_2,   // E
        P1_4,   // D4
        P1_5,   // D5
        P1_6,   // D6
        P1_7,	// D7
        8,		// Line width
        2		// number of lines
    > MyLcd;


typedef LcdStream<MyLcd, AbstractStream> MyLcdStream;
MyUsartStream usart;
MyLcdStream lcd;

// return curent temperature in 1/10 Celsius degrees
static int AdcGetTemp()
{
    ADC10CTL0 |= ENC + ADC10SC;
    //while (ADC10CTL1 & ADC10BUSY);
    __bis_SR_register(CPUOFF + GIE);   // LPM0 with interrupts enabled
    long temp = ADC10MEM;
    long degC = ((temp - 673) * 423 * 10) / 1024;
    return degC;
}

static void AdcInitTempSence()
{
    ADC10CTL1 = INCH_10 + ADC10DIV_3;
    ADC10CTL0 = SREF_1 + ADC10SHT_3 + REFON + ADC10ON + ADC10IE;
}

template<class T>
void ShiftData(T * buffer, unsigned size)
{
	for(unsigned i=1; i < size; i++)
		buffer[size - i] = buffer[size - i - 1];
}

template<class T>
T Avg(T * buffer, unsigned size)
{
	T result;
	for(unsigned i=0; i < size; i++)
		result += buffer[i];
	return result / size;
}

const unsigned bufferSize = 20;
unsigned adcData[bufferSize];

void PrintTemp(int temp, AbstractStream &out)
{
	unsigned tempC = temp / 10;
   	unsigned temp10 = temp % 10;
  	out % IO::Format("Temp =%|+4|.% C\n") % tempC % temp10;
   	//out << "Temp =" << IO::showpos << tempC << "." << IO::noshowpos << temp10 << " C\n";
}

int main()
{
    WDTCTL = WDTPW + WDTHOLD;
	SetUpClock();

    AdcInitTempSence();

    for(unsigned i=0; i < bufferSize; i++)
		adcData[i] = AdcGetTemp();

    while(1)
    {
    	int temp = Avg(adcData, bufferSize);
    	PrintTemp(temp, usart);
		PrintTemp(temp, lcd);

    	ShiftData(adcData, bufferSize);
        adcData[0] = AdcGetTemp();
        Util::delay_ms<100, F_CPU>();
        //printf("%d", 0);
    }
    return 0;
}

interrupt(ADC10_VECTOR) adc_service_routine()
{
    __bic_SR_register_on_exit(CPUOFF);
}


extern "C" void __cxa_pure_virtual()
{
    while(1);
}

