// SPDX-License-Identifier: BSD-3-Clause
#ifndef MXKEYBOARD__HXX
#define MXKEYBOARD__HXX

#include <avr/io.h>

#define DEFAULT_VISIBILITY __attribute__ ((visibility("default")))
#define USED __attribute__ ((__used__))
#define SECTION(name) __attribute__ ((__section__(name)))

extern void run() USED;

#endif /*MXKEYBOARD__HXX*/
