/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* string.h for the target build only (tools/build.py adds -Ifirmware/src/chips/shim): the JieLi toolchain has no libc
 * headers, and vendored C (chips/ym3438.c) includes this one. Felucca's own definitions are in libc.c. */
#pragma once
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
