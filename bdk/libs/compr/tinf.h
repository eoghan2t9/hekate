/*
 * tinf - tiny inflate library (inflate)
 *
 * Copyright (c) 2003-2019 Joergen Ibsen
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must
 *    not claim that you wrote the original software. If you use this
 *    software in a product, an acknowledgment in the product
 *    documentation would be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must
 *    not be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source
 *    distribution.
 */

#ifndef TINF_H_INCLUDED
#define TINF_H_INCLUDED

#include <utils/types.h>

/* Status codes returned by tinf_uncompress(). */
typedef enum {
	TINF_OK         = 0,  /* Success */
	TINF_DATA_ERROR = -3, /* Input error */
	TINF_BUF_ERROR  = -5  /* Not enough room for output */
} tinf_error_code;

/*
 * Decompress raw DEFLATE data of `sourceLen` bytes from `source` to `dest`.
 *
 * `*destLen` must contain the size of `dest` on entry and is set to the
 * size of the decompressed data on success.
 *
 * @return TINF_OK on success, error code on error.
 */
int tinf_uncompress(void *dest, u32 *destLen, const void *source, u32 sourceLen);

#endif
