// Version.h - the one place the version number and product names live.
//
// Every executable carries these in its VERSIONINFO resource (VersionInfo.rc2),
// the installer writes them into Installed Apps, and the service logs the
// version when it starts, which answers "which build is actually running" from
// the log alone. Bump the numbers here and nowhere else.
//
// Plain #defines and an include guard rather than constexpr and #pragma once,
// because the resource compiler reads this file too and understands neither.
#ifndef CEC_VERSION_H
#define CEC_VERSION_H

#define CEC_VERSION_MAJOR 1
#define CEC_VERSION_MINOR 0
#define CEC_VERSION_PATCH 0

#define CEC_PRODUCT_NAME "HDMI-CEC Display Control"
#define CEC_PUBLISHER    "PDM-27"
#define CEC_COPYRIGHT    "Copyright (C) 2026 PDM-27. Licensed under the GPL-3.0."

// "1.0.0", built from the numbers above so the two can never disagree.
#define CEC_STRINGIZE2(x) #x
#define CEC_STRINGIZE(x) CEC_STRINGIZE2(x)
#define CEC_VERSION_STRING                                              \
    CEC_STRINGIZE(CEC_VERSION_MAJOR) "." CEC_STRINGIZE(CEC_VERSION_MINOR) \
    "." CEC_STRINGIZE(CEC_VERSION_PATCH)

// The same strings as wide literals, for code that works in UTF-16.
#define CEC_WIDEN2(x) L##x
#define CEC_WIDEN(x) CEC_WIDEN2(x)

#endif  // CEC_VERSION_H
