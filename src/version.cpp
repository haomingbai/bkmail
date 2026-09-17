#include <bkmail/version.h>

#ifndef BKMAIL_VERSION_STR
#define BKMAIL_VERSION_STR "unknown"
#endif

namespace bkmail {

const char* version() { return BKMAIL_VERSION_STR; }

}  // namespace bkmail
