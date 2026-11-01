#pragma once

// The release workflow checks that a tag v<major>.<minor>.<patch> matches these numbers.
#define ASMEDITOR_VERSION_MAJOR 1
#define ASMEDITOR_VERSION_MINOR 5
#define ASMEDITOR_VERSION_PATCH 0
#define ASMEDITOR_VERSION_TEXT "1.5.0"

#ifndef RC_INVOKED
#define ASMEDITOR_WIDEN_(s) L##s
#define ASMEDITOR_WIDEN(s) ASMEDITOR_WIDEN_(s)
#define ASMEDITOR_VERSION_TEXT_W ASMEDITOR_WIDEN(ASMEDITOR_VERSION_TEXT)
#endif
