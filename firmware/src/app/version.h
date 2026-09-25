/**
 * Firmware identity, injected by the Makefile at build time: the release
 * number (FW_VERSION) and the git commit it was built from (FW_GIT_HASH,
 * FW_GIT_DIRTY when the tree had uncommitted changes).
 */
#ifndef PIXLA_VERSION_H
#define PIXLA_VERSION_H

// "V1.1"
extern const char g_fw_version[];

// Short commit hash, with a trailing "+" for a dirty tree: "0ab8f5e+"
extern const char g_fw_build[];

#endif // PIXLA_VERSION_H
