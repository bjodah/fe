#include "fe.h"

// Both version macros, the pair README.md tells a downstream to assert: a C
// embedding break and a Lisp language break are separate axes, and pinning
// only one of them hides the other. `FeVersion` is not asserted here -- it is
// an extern pointer rather than a constant expression, so it cannot be part
// of a syntax-only header check; test_api.c asserts its value instead.
static_assert(FE_API_VERSION == 5);
static_assert(FE_LANGUAGE_VERSION == 5);

void TestPublicHeaderCompiles(void);
