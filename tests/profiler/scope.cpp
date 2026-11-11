/* PROFILE_SCOPE from C++: two in one block must be two objects. The
 * header once pasted __LINE__ unexpanded, so both were named
 * profile_scope___LINE__ and the second did not compile. */
#include "Profiler.h"

extern "C" void profiler_test_spin(void);

extern "C" void profiler_test_cxx_scopes(void)
{
	PROFILE_SCOPE(ZONE_GS_VSYNC);
	profiler_test_spin();
	PROFILE_SCOPE(ZONE_GS_SYNC);
	profiler_test_spin();
}
