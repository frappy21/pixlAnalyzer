#include "version.h"

// Defaults for a build outside the Makefile (an IDE, a host test)
#ifndef FW_VERSION
#define FW_VERSION "0.0"
#endif
#ifndef FW_GIT_HASH
#define FW_GIT_HASH "nogit"
#endif
#ifndef FW_GIT_DIRTY
#define FW_GIT_DIRTY 0
#endif

const char g_fw_version[] = "V" FW_VERSION;

#if FW_GIT_DIRTY
const char g_fw_build[] = FW_GIT_HASH "+";
#else
const char g_fw_build[] = FW_GIT_HASH;
#endif
