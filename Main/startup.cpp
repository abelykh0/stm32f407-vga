#include "startup.h"

#include <string.h>
#include "stm32f4xx.h"
#include "m4vgalib/vga.h"
#include "m4vgalib/timing.h"

#include "etl/stm32f4xx/gpio.h"
#include <Display/Screen.h>
#include <Display/SpectrumScreen.h>
#include <Keyboard/ps2Keyboard.h>
#include <resources/keyboard.h>

using namespace etl::stm32f4xx;
using namespace Display;

// Screen 48 x 37 characters
#define TEXT_COLUMNS 48
#define TEXT_ROWS 37

// Spectrum screen
//#define TEXT_COLUMNS 32
//#define TEXT_ROWS 24

// Video memory
uint8_t _pixels[TEXT_COLUMNS * 8 * TEXT_ROWS];
uint16_t _attributes[TEXT_COLUMNS * TEXT_ROWS];
uint8_t _borderColor;

// Define single rasterizer band
VideoSettings _videoSettings {
	// timing_vesa_800x600_60hz
	// timing_vesa_640x480_60hz
	// timing_800x600_56hz
	&vga::timing_800x600_56hz, // Timing
	2,  // Scale

	TEXT_COLUMNS, TEXT_ROWS,
	_pixels, _attributes, &_borderColor
};
Screen _screen(_videoSettings);
vga::Band _band {
	&_screen,
	(unsigned int)(_videoSettings.Timing->video_end_line - _videoSettings.Timing->video_start_line),
	nullptr
};

extern "C" void setup()
{
	vga::init();

    // This changes the CPU clock speed
    vga::configure_timing(*_videoSettings.Timing);

    // Inform HAL that the CPU clock speed changed
    SystemCoreClockUpdate();

    _screen.Clear();
    vga::configure_band_list(&_band);
    vga::video_on();

    // Display frame
    _screen.PrintAt(0, 0, "\xC9"); // ╔
    _screen.PrintAt(TEXT_COLUMNS - 1, 0, "\xBB"); // ╗
    _screen.PrintAt(0, TEXT_ROWS - 1, "\xC8"); // ╚
    _screen.PrintAt(TEXT_COLUMNS - 1, TEXT_ROWS - 1, "\xBC"); // ╝
    for (int i = 1; i < TEXT_COLUMNS - 1; i++)
    {
        _screen.PrintAt(i, 0, "\x0CD"); // ═
        _screen.PrintAt(i, TEXT_ROWS - 1, "\x0CD"); // ═
    }
    for (int i = 1; i < TEXT_ROWS - 1; i++)
    {
        _screen.PrintAt(0, i, "\x0BA"); // ║
        _screen.PrintAt(TEXT_COLUMNS - 1, i, "\x0BA"); // ║
    }

    _screen.PrintAt(17, 1, "Hello, world!");

    for (int i = 0; i < 64; i++)
    {
    	char buf[20];
    	sprintf(buf, BYTE_TO_BINARY_PATTERN, BYTE_TO_BINARY(i));
    	_screen.SetAttribute((i << 8) | 0x10);
    	_screen.PrintAt(3 + (i % 6) * 7, 4 + (i / 6) * 2, "\xDF\xDF\xDF\xDF\xDF\xDF");
    	_screen.SetAttribute(0x1510);
    	_screen.PrintAt(3 + (i % 6) * 7, 3 + (i / 6) * 2, buf);
    }

    //_screen.ShowScreenshot(spectrumKeyboard);

    // Initialize PS2 Keyboard
    Ps2_Initialize();

    // Initialize GPIOA
    rcc.enable_clock(AhbPeripheral::gpioa);
    gpioa.set_mode(Gpio::p6, Gpio::Mode::gpio);
}

extern "C" void loop()
{
	// Blink LED on PA6
	//gpioa.toggle(Gpio::p6);

	// Delay 1000 ms
	//HAL_Delay(1000);
}
