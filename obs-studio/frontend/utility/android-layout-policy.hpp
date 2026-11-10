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

constexpr int touchRowHeight(bool isCompact)
{
	return isCompact ? 32 : 48;
}

constexpr int fitLimit(int available, int margin = 8)
{
	return available > margin * 2 ? available - margin * 2 : 1;
}
} // namespace OBSAndroidLayout
