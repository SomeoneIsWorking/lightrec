/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __LIGHTNING_COMPAT_H__
#define __LIGHTNING_COMPAT_H__

#if defined(__has_include)
#if __has_include(<lightning.h>)
#include <lightning.h>
#elif __has_include(<lightning/lightning.h>)
#include <lightning/lightning.h>
#else
#error "GNU Lightning headers are required"
#endif
#else
#include <lightning.h>
#endif

#endif /* __LIGHTNING_COMPAT_H__ */
