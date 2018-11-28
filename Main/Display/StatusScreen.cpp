#include "StatusScreen.h"
#include <string.h>
#include <stm32f407xx.h>
#include "m4vgalib/vga.h"
#include "m4vgalib/rast/unpack_1bpp.h"

namespace Display
{

StatusScreen::StatusScreen(VideoSettings* settings, uint16_t startLine, uint16_t height)
	: Screen(settings, startLine, height)
{
	this->_attributeCount = 0;
}

void StatusScreen::DrawChar(const uint8_t *f, uint16_t x, uint16_t y, uint8_t c)
{
	c -= *(f + 2);
	uint8_t charDefinition[8];
	memcpy(charDefinition, f + (c * *(f + 1)) + 3, 8);

	uint32_t* w = (uint32_t*)charDefinition;
	*w = __REV(*w);
	*w = __RBIT(*w);
	w++;
	*w = __REV(*w);
	*w = __RBIT(*w);
	Screen::Bitmap(x, y, charDefinition, 0, *f, *(f + 1));
}

void StatusScreen::InvertColor()
{
	for (int i = 0; i < 8; i++)
	{
		uint8_t* pixels = this->GetPixelPointer(this->_cursor_y * 8 + i, this->_cursor_x);
		*pixels = ~(*pixels);
	}
}

//__attribute__((section(".ramcode")))
Rasterizer::RasterInfo StatusScreen::rasterize(
		unsigned cycles_per_pixel, unsigned line_number, Pixel *target)
{
	uint8_t borderColor = *this->Settings->BorderColor;
	uint16_t scaledLine = (line_number - this->_startLine) / 2;
	uint16_t scaledResolution = this->_hResolution;

	Rasterizer::RasterInfo result;
	result.offset = 0;
	result.length = scaledResolution;
	result.cycles_per_pixel = cycles_per_pixel;

	if (scaledLine == 0)
	{
		this->_frames++;
	}

	if (this->_verticalBorder > 0 && scaledLine == 0)
	{
		uint32_t fill = borderColor << 8 | borderColor;
		fill |= fill << 16;
		for (uint32_t* ptr = (uint32_t*)target; ptr <= (uint32_t*)target + scaledResolution / 4; ptr++)
		{
			*ptr = fill;
		}
		result.repeat_lines = this->_verticalBorder * 2 - 1;
	}
	else if (this->_verticalBorder > 0 && scaledLine == (unsigned)(this->_vResolution - this->_verticalBorder))
	{
		uint32_t fill = borderColor << 8 | borderColor;
		fill |= fill << 16;
		for (uint32_t* ptr = (uint32_t*)target; ptr <= (uint32_t*)target + scaledResolution / 4; ptr++)
		{
			*ptr = fill;
		}
		result.repeat_lines = this->_verticalBorder * 2 - 1;
	}
	else
	{
		uint16_t vline = scaledLine - this->_verticalBorder;
		uint32_t* bitmap = (uint32_t*)this->GetPixelPointer(vline);
		uint8_t* dest = &target[this->_horizontalBorder];
		rast::unpack_1bpp_impl(bitmap, (const uint8_t*)this->Settings->Attributes, dest, this->Settings->TextColumns / 4);
		result.repeat_lines = 1;
	}

	return result;
}
}
