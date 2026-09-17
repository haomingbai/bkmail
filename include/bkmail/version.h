/**
 * @file version.h
 * @brief Version information for bkmail.
 */

#pragma once
#ifndef BKMAIL_VERSION_H_
#define BKMAIL_VERSION_H_

#include <bkmail/export.h>

namespace bkmail {

/// Returns the bkmail library version as a "major.minor.patch" string.
BKMAIL_EXPORT const char* version();

}  // namespace bkmail

#endif  // BKMAIL_VERSION_H_
