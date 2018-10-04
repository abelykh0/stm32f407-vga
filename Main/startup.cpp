#include "startup.h"

#include <string.h>
#include "stm32f4xx.h"
#include "m4vgalib/vga.h"
#include "m4vgalib/timing.h"

#include "etl/stm32f4xx/gpio.h"
using namespace etl::stm32f4xx;

#include "display/screen.h"
using namespace Display;

// Screen 48 x 37 characters
#define TEXT_COLUMNS 48
#define TEXT_ROWS 37

// Video memory
uint8_t _pixels[TEXT_COLUMNS * 8 * TEXT_ROWS];
uint16_t _attributes[TEXT_COLUMNS * TEXT_ROWS];
uint8_t _borderColor;

// Define single rasterizer band
VideoSettings _videoSettings {
	// timing_vesa_800x600_60hz
	// timing_vesa_640x480_60hz
	// timing_800x600_56hz
	&vga::timing_vesa_800x600_60hz, // Timing
	2,  // Scale

	TEXT_COLUMNS, TEXT_ROWS,
	_pixels, _attributes, &_borderColor
};
Screen _screen(_videoSettings, 0);
vga::Band _band {
	&_screen,
	(unsigned int)(_videoSettings.Timing->video_end_line - _videoSettings.Timing->video_start_line),
	nullptr
};

extern "C" void setup()
{
	vga::init();

    // This changes CPU speed
    vga::configure_timing(*_videoSettings.Timing);

    // Inform HAL that CPU speed changed
    SystemCoreClockUpdate();

    vga::configure_band_list(&_band);
    vga::video_on();

    _screen.PrintAt(10, 10, "Hello, world!");

    // Initialize GPIOA
    rcc.enable_clock(AhbPeripheral::gpioa);
    gpioa.set_mode(Gpio::p6, Gpio::Mode::gpio);
}

extern "C" void loop()
{
	// Blink LED on PA6
	gpioa.toggle(Gpio::p6);

	// Delay 1000 ms
	HAL_Delay(1000);
}
