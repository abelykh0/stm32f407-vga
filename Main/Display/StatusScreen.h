#ifndef __STATUSSCREEN_H
#define __STATUSSCREEN_H

#include "Screen.h"
#include "m4vgalib/rasterizer.h"

using namespace vga;

namespace Display
{

class StatusScreen: public Screen
{
protected:
    virtual void InvertColor() override;
    virtual void DrawChar(const uint8_t *f, uint16_t x, uint16_t y, uint8_t c) override;

public:
    StatusScreen(VideoSettings* settings, uint16_t startLine, uint16_t height);

	virtual RasterInfo rasterize(unsigned cycles_per_pixel, unsigned line_number,
			Pixel *target) override;

	virtual ~StatusScreen() = default;
};

}

#endif
