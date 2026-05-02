#include "DefaultPalette.h"

#include <cmath>
#include <cstring>

namespace {

// HSV (h in [0,360), s,v in [0,1]) → RGB bytes. Standard formula from
// Wikipedia; not the most numerically robust but sufficient at uint8
// quantization.
void HsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
	float c = v * s;
	float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
	float m = v - c;
	float rf, gf, bf;
	if (h < 60)       { rf = c; gf = x; bf = 0; }
	else if (h < 120) { rf = x; gf = c; bf = 0; }
	else if (h < 180) { rf = 0; gf = c; bf = x; }
	else if (h < 240) { rf = 0; gf = x; bf = c; }
	else if (h < 300) { rf = x; gf = 0; bf = c; }
	else              { rf = c; gf = 0; bf = x; }
	r = static_cast<uint8_t>((rf + m) * 255.0f);
	g = static_cast<uint8_t>((gf + m) * 255.0f);
	b = static_cast<uint8_t>((bf + m) * 255.0f);
}

}  // namespace

std::array<uint8_t, 256 * 4> BuildDefaultPalette() {
	std::array<uint8_t, 256 * 4> palette{};

	const uint8_t colors[][4] = {
		{   0,   0,   0,   0 },   // 0: empty (never sampled)
		{ 230,  60,  60, 255 },   // 1: red (sphere)
		{  60, 180,  60, 255 },   // 2: green (torus)
		{  60,  60, 230, 255 },   // 3: blue (box frame)
		{ 230, 230,  60, 255 },   // 4: yellow (cylinder)
		{ 230, 130,  60, 255 },   // 5: orange (cone)
		{ 180,  60, 230, 255 },   // 6: purple (octahedron)
		{  60, 230, 230, 255 },   // 7: cyan (gyroid)
		{ 230,  60, 180, 255 },   // 8: pink (sine blob)
		{ 180, 180, 180, 255 },   // 9: gray (menger sponge)
	};

	for (int i = 0; i < 10; i++) {
		std::memcpy(&palette[i * 4], colors[i], 4);
	}
	for (int i = 10; i < 256; i++) {
		float hue = static_cast<float>(i - 10) / 246.0f * 360.0f;
		HsvToRgb(hue, 0.85f, 0.95f,
		         palette[i * 4 + 0],
		         palette[i * 4 + 1],
		         palette[i * 4 + 2]);
		palette[i * 4 + 3] = 255;
	}
	return palette;
}
