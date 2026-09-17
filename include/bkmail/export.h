/**
 * @file export.h
 * @brief Symbol export macros.
 */

#pragma once
#ifndef BKMAIL_EXPORT_H_
#define BKMAIL_EXPORT_H_

#if defined(BKMAIL_SHARED_LIBRARY)
#if defined(_WIN32)
#if defined(BKMAIL_BUILDING_LIBRARY)
#define BKMAIL_EXPORT __declspec(dllexport)
#else
#define BKMAIL_EXPORT __declspec(dllimport)
#endif
#else
#define BKMAIL_EXPORT __attribute__((visibility("default")))
#endif
#else
#define BKMAIL_EXPORT
#endif

#endif  // BKMAIL_EXPORT_H_
