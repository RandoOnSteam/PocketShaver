/*
 *  gfx_color_policy.h - shared display gamma policy for accelerated paths.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GFX_COLOR_POLICY_H
#define GFX_COLOR_POLICY_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * DMC snapshots hold the guest gamma/fade LUT. The compositor turns it into
 * the display LUT sampled by every visible path. During a DSp fade the LUT is
 * used as-is so fade ramps stay linear.
 *
 * On iOS the compositor LUT is the only place driver gamma is applied; the
 * cscSetEntries palette bake in video.cpp is disabled there, or indexed paths
 * would get gamma twice. New consumers of csSave->gammaTable must go through
 * publish_gamma_lut_to_display_controller rather than baking.
 */

static inline uint8_t GfxColorByteFromUnitFloat(float unit)
{
	if (!(unit > 0.0f)) return 0;
	if (unit >= 1.0f) return 255;
	return (uint8_t)(unit * 255.0f + 0.5f);
}

static inline uint8_t GfxColorClassicMacToSRGBByte(uint8_t value)
{
	if (value == 0 || value == 255) return value;
	const float unit = (float)value / 255.0f;
	return GfxColorByteFromUnitFloat(powf(unit, 1.8f / 2.2f));
}

/* Exact inverse of GfxColorClassicMacToSRGBByte (up to +/-1 LSB rounding).
 * Composing a guest table with this curve cancels the Mac Standard profile
 * gamma back to identity - the anchor for the Linear display policy. */
static inline uint8_t GfxColorSRGBToClassicMacByte(uint8_t value)
{
	if (value == 0 || value == 255) return value;
	const float unit = (float)value / 255.0f;
	return GfxColorByteFromUnitFloat(powf(unit, 2.2f / 1.8f));
}
static inline void GfxColorFillIdentityGammaLUT(uint8_t *lut)
{
	if (lut == NULL) return;
	for (int i = 0; i < 256; i++) {
		lut[i]       = (uint8_t)i;
		lut[256 + i] = (uint8_t)i;
		lut[512 + i] = (uint8_t)i;
	}
}

// apply_correction follows the user "Gamma ramp" pref: when true (OS defined)
// the classic-Mac 1.8 -> sRGB 2.2 display correction is composed in as before;
// when false (Linear) the LUT is left as a passthrough so the guest table is
// presented raw, giving the darker / less color-distorted image the pref
// promises and removing the display-side midtone lift.
static inline void GfxColorFillDefaultDisplayGammaLUT(uint8_t *lut,
                                                      bool apply_correction)
{
	if (lut == NULL) return;
	if (!apply_correction) {
		GfxColorFillIdentityGammaLUT(lut);
		return;
	}
	for (int i = 0; i < 256; i++) {
		const uint8_t corrected = GfxColorClassicMacToSRGBByte((uint8_t)i);
		lut[i]       = corrected;
		lut[256 + i] = corrected;
		lut[512 + i] = corrected;
	}
}

static inline void GfxColorBuildDisplayGammaLUT(const uint8_t *mac_lut,
                                                bool fade_active,
                                                bool apply_correction,
                                                uint8_t *display_lut)
{
	if (mac_lut == NULL || display_lut == NULL) return;

	// A fade ramp must stay linear, and Linear gamma mode also wants the guest
	// table verbatim - both are a straight passthrough of mac_lut.
	if (fade_active || apply_correction) {
		memcpy(display_lut, mac_lut, 768);
		return;
	}

	for (int i = 0; i < 768; i++) {
		display_lut[i] = GfxColorSRGBToClassicMacByte(mac_lut[i]);
	}
}

#endif /* GFX_COLOR_POLICY_H */
