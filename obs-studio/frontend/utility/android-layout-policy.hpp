/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace OBSAndroidLayout {
// Qt widget sizes are logical pixels. Use the available window, not the device
// model or physical screen resolution; a tablet in a small window is compact.
constexpr bool compact(int width, int height, bool wasCompact = false)
{
	if (width <= 0 || height <= 0)
		return wasCompact;
	// Hysteresis prevents alternating layouts near a breakpoint.
	return width < (wasCompact ? 1000 : 952) || height < (wasCompact ? 600 : 552);
}

constexpr bool phoneLandscape(int width, int height, bool wasPhone = false)
{
	if (width <= 0 || height <= 0)
		return wasPhone;
	// Keep enough width for two dock columns and the preview. Hysteresis also applies to aspect ratio.
	return width >= (wasPhone ? 600 : 640) && height < (wasPhone ? 580 : 552) &&
	       width * 10 >= height * (wasPhone ? 16 : 17);
}

constexpr int touchRowHeight(bool isCompact)
{
	return isCompact ? 32 : 48;
}

constexpr int fitLimit(int available, int margin = 8)
{
	return available > margin * 2 ? available - margin * 2 : 1;
}
} // namespace OBSAndroidLayout
