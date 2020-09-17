/*
 * Copyright (c) 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef GEN_BUFFER_ALLOC_H
#define GEN_BUFFER_ALLOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gen_buffer {
   uint64_t gpu;
   uint64_t gpu_end;
   void *map;
   void *driver_bo;
};

struct gen_mapped_pinned_buffer_alloc {
   struct gen_buffer * (*alloc)(void *driver_ctx, uint32_t size);
   void (*free)(void *driver_ctx, struct gen_buffer *buffer);
};

#ifdef __cplusplus
}
#endif

#endif /* GEN_BUFFER_ALLOC_H */
