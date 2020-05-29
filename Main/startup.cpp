#include "startup.h"

#include <stdio.h>
#include <string.h>
#include "stm32f4xx.h"
#include "m4vgalib/vga.h"
#include "m4vgalib/timing.h"

#include "etl/stm32f4xx/gpio.h"
#include <Display/Screen.h>
#include <Display/StatusScreen.h>
#include <Keyboard/ps2Keyboard.h>

using namespace etl::stm32f4xx;
using namespace Display;

static uint32_t _frames = 0;
extern RTC_HandleTypeDef hrtc;

#define TEXT_COLUMNS 50
#define TEXT_ROWS 37
//#define TEXT_COLUMNS 80
//#define TEXT_ROWS 36

// Video memory
static uint8_t _pixels[TEXT_COLUMNS * 8 * TEXT_ROWS];
static uint8_t _borderColor;
static uint16_t _attributes[TEXT_COLUMNS * TEXT_ROWS];
//uint8_t _palette[] = { 0B00000000, 0B00001100 };
//uint16_t* _attributes = (uint16_t*)_palette;

// Define single rasterizer band
VideoSettings _videoSettings {
	// timing_vesa_800x600_60hz
	// timing_vesa_640x480_60hz
	// timing_800x600_56hz
	&vga::timing_vesa_800x600_60hz, // Timing
	2, 2,  // Scale
	TEXT_COLUMNS, TEXT_ROWS,
	_pixels, _attributes, &_borderColor
};
static Screen _screen(&_videoSettings);
//StatusScreen _screen(&_videoSettings, 0, _videoSettings.Timing->video_end_line - _videoSettings.Timing->video_start_line);
static vga::Band _band {
	&_screen,
	(unsigned int)(_videoSettings.Timing->video_end_line - _videoSettings.Timing->video_start_line),
	nullptr
};

extern "C" void initialize()
{
	vga::init();

    // This changes the CPU clock speed
    vga::configure_timing(*_videoSettings.Timing);

    // Inform HAL that the CPU clock speed changed
    SystemCoreClockUpdate();
}

extern "C" void setup()
{
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

    _screen.PrintAt(17, 2, "Hello, world!");

	char buf[20];

    for (int i = 0; i < 64; i++)
    {
    	sprintf(buf, BYTE_TO_BINARY_PATTERN, BYTE_TO_BINARY(i));
    	_screen.SetAttribute((i << 8) | 0x10);
    	_screen.PrintAt(4 + (i % 6) * 7, 5 + (i / 6) * 2, "\xDF\xDF\xDF\xDF\xDF\xDF");
    	_screen.SetAttribute(0x1510);
    	_screen.PrintAt(4 + (i % 6) * 7, 4 + (i / 6) * 2, buf);
    }

	_screen.SetAttribute(0x3F10);
    for (uint16_t character = 1; character <= 255; character++)
    {
		_screen.PrintCharAt(character % 48 + 1, character / 48 + 27, character);
    }

    // Initialize PS2 Keyboard
    Ps2_Initialize();
}

extern "C" void loop()
{
	// Display current date and time

	RTC_TimeTypeDef time;
	RTC_DateTypeDef date;
	if (_screen._frames > _frames)
	{
		char formattedDateTime[50];
		HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
		HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
		sprintf(formattedDateTime, "%04d-%02d-%02d %02d:%02d:%02d",
				date.Year + 2000, date.Month, date.Date,
				time.Hours, time.Minutes, time.Seconds);
		_screen.PrintAlignCenter(34, formattedDateTime);
		_frames = _screen._frames + 5;
	}
}
