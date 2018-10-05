#ifndef _SCREEN_H
#define _SCREEN_H

#include "VideoSettings.h"
#include "font8x8.h"
#include "m4vgalib/rasterizer.h"

using namespace vga;

namespace Display
{

class Screen: public Rasterizer
{
private:
	VideoSettings _setings;
	uint16_t _startLine;

    uint16_t _hResolution;
    uint16_t _vResolution;
    uint16_t _pixelCount;
    uint16_t _attributeCount;

    uint8_t* _font = (uint8_t*)font8x8;
    uint16_t _attribute = 0x3F10; // white on blue
    uint8_t _cursor_x = 0;
    uint8_t _cursor_y = 0;

//    uint16_t _leftBorder;
//    uint16_t _rightBorder;
//    uint8_t _bottomBorder;
//    uint8_t _topBorder;

    void Draw4(uint8_t *bitmap, uint16_t *colors, uint8_t *dest);
    void PrintChar(char c, uint16_t color);
    void PrintCharAt(uint8_t x, uint8_t y, unsigned char c, uint16_t color);
    void DrawChar(const uint8_t *f, uint16_t x, uint16_t y, uint8_t c);
    void Bitmap(uint16_t x, uint16_t y, const unsigned char *bmp,
                uint16_t i, uint8_t width, uint8_t lines);
    void CursorNext();

public:
	Screen(VideoSettings setings, uint16_t startLine);

	void Clear();
	void SetFont(const uint8_t* font);
	void SetAttribute(uint16_t attribute);
	void SetCursorPosition(uint8_t x, uint8_t y);
	void Print(const char* str);
	void PrintAt(uint8_t x, uint8_t y, const char* str);
	void PrintAlignRight(uint8_t x, uint8_t y, const char *str);

	uint8_t* GetPixelPointer(uint8_t line);
	uint8_t* GetPixelPointer(uint8_t line, uint8_t character);

	void ShowSinclairScreenshot(const char *screenshot);
	uint16_t FromSinclairColor(uint8_t sinclairColor);
	uint8_t ToSinclairColor(uint16_t color);

	RasterInfo rasterize(unsigned cycles_per_pixel, unsigned line_number,
			Pixel *target) override;

	virtual ~Screen() = default;
};

}

#endif
