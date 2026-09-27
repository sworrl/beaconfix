// The estimation engine is header-only (src/estimator.h) so it can be shared by the
// app, the tests and any ad-hoc tool without link steps. This translation unit exists
// so the engine gets compiled (and its warnings surfaced) as part of the normal build
// once it is listed in CMakeLists.txt.
#include "estimator.h"
