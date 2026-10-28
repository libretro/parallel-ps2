#pragma once

/*
 * Installs a reporter for the signals that kill the core outright.
 *
 * It has to go in before vtlb installs the fastmem page fault handler: that one
 * takes over SIGSEGV (and SIGBUS on aarch64) and chains to whatever was there
 * before it when a fault is not one of its own, so installing first is what
 * makes a genuine segfault reach this instead of going straight to the default
 * action. Signals fastmem does not touch are taken directly.
 */
void CrashHandler_Install(void);
