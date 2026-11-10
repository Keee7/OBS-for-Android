#include "../../obs-studio/frontend/utility/android-layout-policy.hpp"
#include <cassert>

int main()
{
	using namespace OBSAndroidLayout;
	assert(!compact(1280, 800)); // Tablet in a full-size window.
	assert(!compact(1067, 667));
	assert(compact(873, 393)); // Landscape phone.
	assert(compact(800, 700)); // The same tablet in a smaller window.
	assert(compact(1280, 500));
	assert(!compact(1000, 600));
	assert(compact(1024, 620, true)); // Avoid oscillation around a breakpoint.
	assert(!compact(1048, 648, true));
	assert(compact(0, 0, true));
	assert(!compact(0, 0, false));
	assert(touchRowHeight(false) == 48 && touchRowHeight(true) == 32);
	assert(fitLimit(300) == 284 && fitLimit(220) == 204);
	assert(fitLimit(1) == 1 && fitLimit(16) == 1);
	for (int available = 1; available <= 2048; available++)
		assert(fitLimit(available) >= 1 && fitLimit(available) <= available);
}
