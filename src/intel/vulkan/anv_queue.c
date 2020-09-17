/*
 * Copyright © 2015 Intel Corporation
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

/**
 * This file implements VkQueue, VkFence, and VkSemaphore
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "anv_private.h"
#include "vk_util.h"

#include "genxml/gen7_pack.h"

uint64_t anv_gettime_ns(void)
{
   struct timespec current;
   clock_gettime(CLOCK_MONOTONIC, &current);
   return (uint64_t)current.tv_sec * NSEC_PER_SEC + current.tv_nsec;
}

uint64_t anv_get_absolute_timeout(uint64_t timeout)
{
   if (timeout == 0)
      return 0;
   uint64_t current_time = anv_gettime_ns();
   uint64_t max_timeout = (uint64_t) INT64_MAX - current_time;

   timeout = MIN2(max_timeout, timeout);

   return (current_time + timeout);
}

static int64_t anv_get_relative_timeout(uint64_t abs_timeout)
{
   uint64_t now = anv_gettime_ns();

   /* We don't want negative timeouts.
    *
    * DRM_IOCTL_I915_GEM_WAIT uses a signed 64 bit timeout and is
    * supposed to block indefinitely timeouts < 0.  Unfortunately,
    * this was broken for a couple of kernel releases.  Since there's
    * no way to know whether or not the kernel we're using is one of
    * the broken ones, the best we can do is to clamp the timeout to
    * INT64_MAX.  This limits the maximum timeout from 584 years to
    * 292 years - likely not a big deal.
    */
   if (abs_timeout < now)
      return 0;

   uint64_t rel_timeout = abs_timeout - now;
   if (rel_timeout > (uint64_t) INT64_MAX)
      rel_timeout = INT64_MAX;

   return rel_timeout;
}

static struct anv_semaphore *anv_semaphore_ref(struct anv_semaphore *semaphore);
static void anv_semaphore_unref(struct anv_device *device, struct anv_semaphore *semaphore);
static void anv_semaphore_impl_cleanup(struct anv_device *device,
                                       struct anv_semaphore_impl *impl);

static void
anv_queue_submit_free(struct anv_device *device,
                      struct anv_queue_submit *submit)
{
   const VkAllocationCallbacks *alloc = submit->alloc;

   for (uint32_t i = 0; i < submit->temporary_semaphore_count; i++)
      anv_semaphore_impl_cleanup(device, &submit->temporary_semaphores[i]);
   for (uint32_t i = 0; i < submit->sync_fd_semaphore_count; i++)
      anv_semaphore_unref(device, submit->sync_fd_semaphores[i]);
   /* Execbuf does not consume the in_fence.  It's our job to close it. */
   if (submit->in_fence != -1)
      close(submit->in_fence);
   if (submit->out_fence != -1)
      close(submit->out_fence);
   vk_free(alloc, submit->fences);
   vk_free(alloc, submit->temporary_semaphores);
   vk_free(alloc, submit->wait_timelines);
   vk_free(alloc, submit->wait_timeline_values);
   vk_free(alloc, submit->signal_timelines);
   vk_free(alloc, submit->signal_timeline_values);
   vk_free(alloc, submit->fence_bos);
   vk_free(alloc, submit);
}

static bool
anv_queue_submit_ready_locked(struct anv_queue_submit *submit)
{
   for (uint32_t i = 0; i < submit->wait_timeline_count; i++) {
      if (submit->wait_timeline_values[i] > submit->wait_timelines[i]->highest_pending)
         return false;
   }

   return true;
}

static VkResult
anv_timeline_init(struct anv_device *device,
                  struct anv_timeline *timeline,
                  uint64_t initial_value)
{
   timeline->highest_past =
      timeline->highest_pending = initial_value;
   list_inithead(&timeline->points);
   list_inithead(&timeline->free_points);

   return VK_SUCCESS;
}

static void
anv_timeline_finish(struct anv_device *device,
                    struct anv_timeline *timeline)
{
   list_for_each_entry_safe(struct anv_timeline_point, point,
                            &timeline->free_points, link) {
      list_del(&point->link);
      anv_device_release_bo(device, point->bo);
      vk_free(&device->alloc, point);
   }
   list_for_each_entry_safe(struct anv_timeline_point, point,
                            &timeline->points, link) {
      list_del(&point->link);
      anv_device_release_bo(device, point->bo);
      vk_free(&device->alloc, point);
   }
}

static VkResult
anv_timeline_add_point_locked(struct anv_device *device,
                              struct anv_timeline *timeline,
                              uint64_t value,
                              struct anv_timeline_point **point)
{
   VkResult result = VK_SUCCESS;

   if (list_is_empty(&timeline->free_points)) {
      *point =
         vk_zalloc(&device->alloc, sizeof(**point),
                   8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!(*point))
         result = vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      if (result == VK_SUCCESS) {
         result = anv_device_alloc_bo(device, 4096,
                                      ANV_BO_ALLOC_EXTERNAL |
                                      ANV_BO_ALLOC_IMPLICIT_SYNC,
                                      0 /* explicit_address */,
                                      &(*point)->bo);
         if (result != VK_SUCCESS)
            vk_free(&device->alloc, *point);
      }
   } else {
      *point = list_first_entry(&timeline->free_points,
                                struct anv_timeline_point, link);
      list_del(&(*point)->link);
   }

   if (result == VK_SUCCESS) {
      (*point)->serial = value;
      list_addtail(&(*point)->link, &timeline->points);
   }

   return result;
}

static VkResult
anv_timeline_gc_locked(struct anv_device *device,
                       struct anv_timeline *timeline)
{
   list_for_each_entry_safe(struct anv_timeline_point, point,
                            &timeline->points, link) {
      /* timeline->higest_pending is only incremented once submission has
       * happened. If this point has a greater serial, it means the point
       * hasn't been submitted yet.
       */
      if (point->serial > timeline->highest_pending)
         return VK_SUCCESS;

      /* If someone is waiting on this time point, consider it busy and don't
       * try to recycle it. There's a slim possibility that it's no longer
       * busy by the time we look at it but we would be recycling it out from
       * under a waiter and that can lead to weird races.
       *
       * We walk the list in-order so if this time point is still busy so is
       * every following time point
       */
      assert(point->waiting >= 0);
      if (point->waiting)
         return VK_SUCCESS;

      /* Garbage collect any signaled point. */
      VkResult result = anv_device_bo_busy(device, point->bo);
      if (result == VK_NOT_READY) {
         /* We walk the list in-order so if this time point is still busy so
          * is every following time point
          */
         return VK_SUCCESS;
      } else if (result != VK_SUCCESS) {
         return result;
      }

      assert(timeline->highest_past < point->serial);
      timeline->highest_past = point->serial;

      list_del(&point->link);
      list_add(&point->link, &timeline->free_points);
   }

   return VK_SUCCESS;
}

static VkResult anv_queue_submit_add_fence_bo(struct anv_queue_submit *submit,
                                              struct anv_bo *bo,
                                              bool signal);

static VkResult
anv_queue_submit_timeline_locked(struct anv_queue *queue,
                                 struct anv_queue_submit *submit)
{
   VkResult result;

   for (uint32_t i = 0; i < submit->wait_timeline_count; i++) {
      struct anv_timeline *timeline = submit->wait_timelines[i];
      uint64_t wait_value = submit->wait_timeline_values[i];

      if (timeline->highest_past >= wait_value)
         continue;

      list_for_each_entry(struct anv_timeline_point, point, &timeline->points, link) {
         if (point->serial < wait_value)
            continue;
         result = anv_queue_submit_add_fence_bo(submit, point->bo, false);
         if (result != VK_SUCCESS)
            return result;
         break;
      }
   }
   for (uint32_t i = 0; i < submit->signal_timeline_count; i++) {
      struct anv_timeline *timeline = submit->signal_timelines[i];
      uint64_t signal_value = submit->signal_timeline_values[i];
      struct anv_timeline_point *point;

      result = anv_timeline_add_point_locked(queue->device, timeline,
                                             signal_value, &point);
      if (result != VK_SUCCESS)
         return result;

      result = anv_queue_submit_add_fence_bo(submit, point->bo, true);
      if (result != VK_SUCCESS)
         return result;
   }

   result = anv_queue_execbuf_locked(queue, submit);

   if (result == VK_SUCCESS) {
      /* Update the pending values in the timeline objects. */
      for (uint32_t i = 0; i < submit->signal_timeline_count; i++) {
         struct anv_timeline *timeline = submit->signal_timelines[i];
         uint64_t signal_value = submit->signal_timeline_values[i];

         assert(signal_value > timeline->highest_pending);
         timeline->highest_pending = signal_value;
      }

      /* Update signaled semaphores backed by syncfd. */
      for (uint32_t i = 0; i < submit->sync_fd_semaphore_count; i++) {
         struct anv_semaphore *semaphore = submit->sync_fd_semaphores[i];
         /* Out fences can't have temporary state because that would imply
          * that we imported a sync file and are trying to signal it.
          */
         assert(semaphore->temporary.type == ANV_SEMAPHORE_TYPE_NONE);
         struct anv_semaphore_impl *impl = &semaphore->permanent;

         assert(impl->type == ANV_SEMAPHORE_TYPE_SYNC_FILE);
         impl->fd = dup(submit->out_fence);
      }
   } else {
      /* Unblock any waiter by signaling the points, the application will get
       * a device lost error code.
       */
      for (uint32_t i = 0; i < submit->signal_timeline_count; i++) {
         struct anv_timeline *timeline = submit->signal_timelines[i];
         uint64_t signal_value = submit->signal_timeline_values[i];

         assert(signal_value > timeline->highest_pending);
         timeline->highest_past = timeline->highest_pending = signal_value;
      }
   }

   return result;
}

static VkResult
anv_queue_submit_deferred_locked(struct anv_queue *queue, uint32_t *advance)
{
   VkResult result = VK_SUCCESS;

   /* Go through all the queued submissions and submit then until we find one
    * that's waiting on a point that hasn't materialized yet.
    */
   list_for_each_entry_safe(struct anv_queue_submit, submit,
                            &queue->queued_submits, link) {
      if (!anv_queue_submit_ready_locked(submit))
         break;

      (*advance)++;
      list_del(&submit->link);

      result = anv_queue_submit_timeline_locked(queue, submit);

      anv_queue_submit_free(queue->device, submit);

      if (result != VK_SUCCESS)
         break;
   }

   return result;
}

static VkResult
anv_device_submit_deferred_locked(struct anv_device *device)
{
   uint32_t advance = 0;
   return anv_queue_submit_deferred_locked(&device->queue, &advance);
}

static VkResult
_anv_queue_submit(struct anv_queue *queue, struct anv_queue_submit **_submit,
                  bool flush_queue)
{
   struct anv_queue_submit *submit = *_submit;

   /* Wait before signal behavior means we might keep alive the
    * anv_queue_submit object a bit longer, so transfer the ownership to the
    * anv_queue.
    */
   *_submit = NULL;

   pthread_mutex_lock(&queue->device->mutex);
   list_addtail(&submit->link, &queue->queued_submits);
   VkResult result = anv_device_submit_deferred_locked(queue->device);
   if (flush_queue) {
      while (result == VK_SUCCESS && !list_is_empty(&queue->queued_submits)) {
         int ret = pthread_cond_wait(&queue->device->queue_submit,
                                     &queue->device->mutex);
         if (ret != 0) {
            result = anv_device_set_lost(queue->device, "wait timeout");
            break;
         }

         result = anv_device_submit_deferred_locked(queue->device);
      }
   }
   pthread_mutex_unlock(&queue->device->mutex);
   return result;
}

VkResult
anv_queue_init(struct anv_device *device, struct anv_queue *queue)
{
   queue->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   queue->device = device;
   queue->flags = 0;

   list_inithead(&queue->queued_submits);

   return VK_SUCCESS;
}

void
anv_queue_finish(struct anv_queue *queue)
{
}

static VkResult
anv_queue_submit_add_fence_bo(struct anv_queue_submit *submit,
                              struct anv_bo *bo,
                              bool signal)
{
   if (submit->fence_bo_count >= submit->fence_bo_array_length) {
      uint32_t new_len = MAX2(submit->fence_bo_array_length * 2, 64);
      uintptr_t *new_fence_bos =
         vk_realloc(submit->alloc,
                    submit->fence_bos, new_len * sizeof(*submit->fence_bos),
                    8, submit->alloc_scope);
      if (new_fence_bos == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->fence_bos = new_fence_bos;
      submit->fence_bo_array_length = new_len;
   }

   /* Take advantage that anv_bo are allocated at 8 byte alignement so we can
    * use the lowest bit to store whether this is a BO we need to signal.
    */
   submit->fence_bos[submit->fence_bo_count++] = anv_pack_ptr(bo, 1, signal);

   return VK_SUCCESS;
}

static VkResult
anv_queue_submit_add_syncobj(struct anv_queue_submit* submit,
                             struct anv_device *device,
                             uint32_t handle, uint32_t flags)
{
   assert(flags != 0);

   if (submit->fence_count >= submit->fence_array_length) {
      uint32_t new_len = MAX2(submit->fence_array_length * 2, 64);
      struct drm_i915_gem_exec_fence *new_fences =
         vk_realloc(submit->alloc,
                    submit->fences, new_len * sizeof(*submit->fences),
                    8, submit->alloc_scope);
      if (new_fences == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->fences = new_fences;
      submit->fence_array_length = new_len;
   }

   submit->fences[submit->fence_count++] = (struct drm_i915_gem_exec_fence) {
      .handle = handle,
      .flags = flags,
   };

   return VK_SUCCESS;
}

static VkResult
anv_queue_submit_add_sync_fd_fence(struct anv_queue_submit *submit,
                                   struct anv_semaphore *semaphore)
{
   if (submit->sync_fd_semaphore_count >= submit->sync_fd_semaphore_array_length) {
      uint32_t new_len = MAX2(submit->sync_fd_semaphore_array_length * 2, 64);
      struct anv_semaphore **new_semaphores =
         vk_realloc(submit->alloc, submit->sync_fd_semaphores,
                    new_len * sizeof(*submit->sync_fd_semaphores), 8,
                    submit->alloc_scope);
      if (new_semaphores == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->sync_fd_semaphores = new_semaphores;
   }

   submit->sync_fd_semaphores[submit->sync_fd_semaphore_count++] =
      anv_semaphore_ref(semaphore);
   submit->need_out_fence = true;

   return VK_SUCCESS;
}

static VkResult
anv_queue_submit_add_timeline_wait(struct anv_queue_submit* submit,
                                   struct anv_device *device,
                                   struct anv_timeline *timeline,
                                   uint64_t value)
{
   if (submit->wait_timeline_count >= submit->wait_timeline_array_length) {
      uint32_t new_len = MAX2(submit->wait_timeline_array_length * 2, 64);
      struct anv_timeline **new_wait_timelines =
         vk_realloc(submit->alloc,
                    submit->wait_timelines, new_len * sizeof(*submit->wait_timelines),
                    8, submit->alloc_scope);
      if (new_wait_timelines == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->wait_timelines = new_wait_timelines;

      uint64_t *new_wait_timeline_values =
         vk_realloc(submit->alloc,
                    submit->wait_timeline_values, new_len * sizeof(*submit->wait_timeline_values),
                    8, submit->alloc_scope);
      if (new_wait_timeline_values == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->wait_timeline_values = new_wait_timeline_values;

      submit->wait_timeline_array_length = new_len;
   }

   submit->wait_timelines[submit->wait_timeline_count] = timeline;
   submit->wait_timeline_values[submit->wait_timeline_count] = value;

   submit->wait_timeline_count++;

   return VK_SUCCESS;
}

static VkResult
anv_queue_submit_add_timeline_signal(struct anv_queue_submit* submit,
                                     struct anv_device *device,
                                     struct anv_timeline *timeline,
                                     uint64_t value)
{
   assert(timeline->highest_pending < value);

   if (submit->signal_timeline_count >= submit->signal_timeline_array_length) {
      uint32_t new_len = MAX2(submit->signal_timeline_array_length * 2, 64);
      struct anv_timeline **new_signal_timelines =
         vk_realloc(submit->alloc,
                    submit->signal_timelines, new_len * sizeof(*submit->signal_timelines),
                    8, submit->alloc_scope);
      if (new_signal_timelines == NULL)
            return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->signal_timelines = new_signal_timelines;

      uint64_t *new_signal_timeline_values =
         vk_realloc(submit->alloc,
                    submit->signal_timeline_values, new_len * sizeof(*submit->signal_timeline_values),
                    8, submit->alloc_scope);
      if (new_signal_timeline_values == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->signal_timeline_values = new_signal_timeline_values;

      submit->signal_timeline_array_length = new_len;
   }

   submit->signal_timelines[submit->signal_timeline_count] = timeline;
   submit->signal_timeline_values[submit->signal_timeline_count] = value;

   submit->signal_timeline_count++;

   return VK_SUCCESS;
}

static struct anv_queue_submit *
anv_queue_submit_alloc(struct anv_device *device)
{
   const VkAllocationCallbacks *alloc = &device->alloc;
   VkSystemAllocationScope alloc_scope = VK_SYSTEM_ALLOCATION_SCOPE_DEVICE;

   struct anv_queue_submit *submit = vk_zalloc(alloc, sizeof(*submit), 8, alloc_scope);
   if (!submit)
      return NULL;

   submit->alloc = alloc;
   submit->alloc_scope = alloc_scope;
   submit->in_fence = -1;
   submit->out_fence = -1;

   return submit;
}

VkResult
anv_queue_submit_simple_batch(struct anv_queue *queue,
                              struct anv_batch *batch)
{
   if (queue->device->no_hw)
      return VK_SUCCESS;

   struct anv_device *device = queue->device;
   struct anv_queue_submit *submit = anv_queue_submit_alloc(device);
   if (!submit)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   bool has_syncobj_wait = device->physical->has_syncobj_wait;
   VkResult result;
   uint32_t syncobj;
   struct anv_bo *batch_bo, *sync_bo;

   if (has_syncobj_wait) {
      syncobj = anv_gem_syncobj_create(device, 0);
      if (!syncobj) {
         result = vk_error(VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_free_submit;
      }

      result = anv_queue_submit_add_syncobj(submit, device, syncobj,
                                            I915_EXEC_FENCE_SIGNAL);
   } else {
      result = anv_device_alloc_bo(device, 4096,
                                   ANV_BO_ALLOC_EXTERNAL |
                                   ANV_BO_ALLOC_IMPLICIT_SYNC,
                                   0 /* explicit_address */,
                                   &sync_bo);
      if (result != VK_SUCCESS)
         goto err_free_submit;

      result = anv_queue_submit_add_fence_bo(submit, sync_bo, true /* signal */);
   }

   if (result != VK_SUCCESS)
      goto err_destroy_sync_primitive;

   if (batch) {
      uint32_t size = align_u32(batch->next - batch->start, 8);
      result = anv_bo_pool_alloc(&device->batch_bo_pool, size, &batch_bo);
      if (result != VK_SUCCESS)
         goto err_destroy_sync_primitive;

      memcpy(batch_bo->map, batch->start, size);
      if (!device->info.has_llc)
         gen_flush_range(batch_bo->map, size);

      submit->simple_bo = batch_bo;
      submit->simple_bo_size = size;
   }

   result = _anv_queue_submit(queue, &submit, true);

   if (result == VK_SUCCESS) {
      if (has_syncobj_wait) {
         if (anv_gem_syncobj_wait(device, &syncobj, 1,
                                  anv_get_absolute_timeout(INT64_MAX), true))
            result = anv_device_set_lost(device, "anv_gem_syncobj_wait failed: %m");
         anv_gem_syncobj_destroy(device, syncobj);
      } else {
         result = anv_device_wait(device, sync_bo,
                                  anv_get_relative_timeout(INT64_MAX));
         anv_device_release_bo(device, sync_bo);
      }
   }

   if (batch)
      anv_bo_pool_free(&device->batch_bo_pool, batch_bo);

   if (submit)
      anv_queue_submit_free(device, submit);

   return result;

 err_destroy_sync_primitive:
   if (has_syncobj_wait)
      anv_gem_syncobj_destroy(device, syncobj);
   else
      anv_device_release_bo(device, sync_bo);
 err_free_submit:
   if (submit)
      anv_queue_submit_free(device, submit);

   return result;
}

/* Transfer ownership of temporary semaphores from the VkSemaphore object to
 * the anv_queue_submit object. Those temporary semaphores are then freed in
 * anv_queue_submit_free() once the driver is finished with them.
 */
static VkResult
maybe_transfer_temporary_semaphore(struct anv_queue_submit *submit,
                                   struct anv_semaphore *semaphore,
                                   struct anv_semaphore_impl **out_impl)
{
   struct anv_semaphore_impl *impl = &semaphore->temporary;

   if (impl->type == ANV_SEMAPHORE_TYPE_NONE) {
      *out_impl = &semaphore->permanent;
      return VK_SUCCESS;
   }

   /* BO backed timeline semaphores cannot be temporary. */
   assert(impl->type != ANV_SEMAPHORE_TYPE_TIMELINE);

   /*
    * There is a requirement to reset semaphore to their permanent state after
    * submission. From the Vulkan 1.0.53 spec:
    *
    *    "If the import is temporary, the implementation must restore the
    *    semaphore to its prior permanent state after submitting the next
    *    semaphore wait operation."
    *
    * In the case we defer the actual submission to a thread because of the
    * wait-before-submit behavior required for timeline semaphores, we need to
    * make copies of the temporary syncobj to ensure they stay alive until we
    * do the actual execbuffer ioctl.
    */
   if (submit->temporary_semaphore_count >= submit->temporary_semaphore_array_length) {
      uint32_t new_len = MAX2(submit->temporary_semaphore_array_length * 2, 8);
      /* Make sure that if the realloc fails, we still have the old semaphore
       * array around to properly clean things up on failure.
       */
      struct anv_semaphore_impl *new_array =
         vk_realloc(submit->alloc,
                    submit->temporary_semaphores,
                    new_len * sizeof(*submit->temporary_semaphores),
                    8, submit->alloc_scope);
      if (new_array == NULL)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      submit->temporary_semaphores = new_array;
      submit->temporary_semaphore_array_length = new_len;
   }

   /* Copy anv_semaphore_impl into anv_queue_submit. */
   submit->temporary_semaphores[submit->temporary_semaphore_count++] = *impl;
   *out_impl = &submit->temporary_semaphores[submit->temporary_semaphore_count - 1];

   /* Clear the incoming semaphore */
   impl->type = ANV_SEMAPHORE_TYPE_NONE;

   return VK_SUCCESS;
}

static VkResult
anv_queue_submit(struct anv_queue *queue,
                 struct anv_cmd_buffer *cmd_buffer,
                 const VkSemaphore *in_semaphores,
                 const uint64_t *in_values,
                 uint32_t num_in_semaphores,
                 const VkSemaphore *out_semaphores,
                 const uint64_t *out_values,
                 uint32_t num_out_semaphores,
                 struct anv_bo *wsi_signal_bo,
                 VkFence _fence)
{
   ANV_FROM_HANDLE(anv_fence, fence, _fence);
   struct anv_device *device = queue->device;
   UNUSED struct anv_physical_device *pdevice = device->physical;
   struct anv_queue_submit *submit = anv_queue_submit_alloc(device);
   if (!submit)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   submit->cmd_buffer = cmd_buffer;

   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < num_in_semaphores; i++) {
      ANV_FROM_HANDLE(anv_semaphore, semaphore, in_semaphores[i]);
      struct anv_semaphore_impl *impl;

      result = maybe_transfer_temporary_semaphore(submit, semaphore, &impl);
      if (result != VK_SUCCESS)
         goto error;

      switch (impl->type) {
      case ANV_SEMAPHORE_TYPE_BO:
         assert(!pdevice->has_syncobj);
         result = anv_queue_submit_add_fence_bo(submit, impl->bo, false /* signal */);
         if (result != VK_SUCCESS)
            goto error;
         break;

      case ANV_SEMAPHORE_TYPE_WSI_BO:
         /* When using a window-system buffer as a semaphore, always enable
          * EXEC_OBJECT_WRITE.  This gives us a WaR hazard with the display or
          * compositor's read of the buffer and enforces that we don't start
          * rendering until they are finished.  This is exactly the
          * synchronization we want with vkAcquireNextImage.
          */
         result = anv_queue_submit_add_fence_bo(submit, impl->bo, true /* signal */);
         if (result != VK_SUCCESS)
            goto error;
         break;

      case ANV_SEMAPHORE_TYPE_SYNC_FILE:
         assert(!pdevice->has_syncobj);
         if (submit->in_fence == -1) {
            submit->in_fence = impl->fd;
            if (submit->in_fence == -1) {
               result = vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
               goto error;
            }
            impl->fd = -1;
         } else {
            int merge = anv_gem_sync_file_merge(device, submit->in_fence, impl->fd);
            if (merge == -1) {
               result = vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
               goto error;
            }
            close(impl->fd);
            close(submit->in_fence);
            impl->fd = -1;
            submit->in_fence = merge;
         }
         break;

      case ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ: {
         result = anv_queue_submit_add_syncobj(submit, device,
                                               impl->syncobj,
                                               I915_EXEC_FENCE_WAIT);
         if (result != VK_SUCCESS)
            goto error;
         break;
      }

      case ANV_SEMAPHORE_TYPE_TIMELINE:
         result = anv_queue_submit_add_timeline_wait(submit, device,
                                                     &impl->timeline,
                                                     in_values ? in_values[i] : 0);
         if (result != VK_SUCCESS)
            goto error;
         break;

      default:
         break;
      }
   }

   for (uint32_t i = 0; i < num_out_semaphores; i++) {
      ANV_FROM_HANDLE(anv_semaphore, semaphore, out_semaphores[i]);

      /* Under most circumstances, out fences won't be temporary.  However,
       * the spec does allow it for opaque_fd.  From the Vulkan 1.0.53 spec:
       *
       *    "If the import is temporary, the implementation must restore the
       *    semaphore to its prior permanent state after submitting the next
       *    semaphore wait operation."
       *
       * The spec says nothing whatsoever about signal operations on
       * temporarily imported semaphores so it appears they are allowed.
       * There are also CTS tests that require this to work.
       */
      struct anv_semaphore_impl *impl =
         semaphore->temporary.type != ANV_SEMAPHORE_TYPE_NONE ?
         &semaphore->temporary : &semaphore->permanent;

      switch (impl->type) {
      case ANV_SEMAPHORE_TYPE_BO:
         assert(!pdevice->has_syncobj);
         result = anv_queue_submit_add_fence_bo(submit, impl->bo, true /* signal */);
         if (result != VK_SUCCESS)
            goto error;
         break;

      case ANV_SEMAPHORE_TYPE_SYNC_FILE:
         assert(!pdevice->has_syncobj);
         result = anv_queue_submit_add_sync_fd_fence(submit, semaphore);
         if (result != VK_SUCCESS)
            goto error;
         break;

      case ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ: {
         result = anv_queue_submit_add_syncobj(submit, device, impl->syncobj,
                                               I915_EXEC_FENCE_SIGNAL);
         if (result != VK_SUCCESS)
            goto error;
         break;
      }

      case ANV_SEMAPHORE_TYPE_TIMELINE:
         result = anv_queue_submit_add_timeline_signal(submit, device,
                                                       &impl->timeline,
                                                       out_values ? out_values[i] : 0);
         if (result != VK_SUCCESS)
            goto error;
         break;

      default:
         break;
      }
   }

   if (wsi_signal_bo) {
      result = anv_queue_submit_add_fence_bo(submit, wsi_signal_bo, true /* signal */);
      if (result != VK_SUCCESS)
         goto error;
   }

   if (fence) {
      /* Under most circumstances, out fences won't be temporary.  However,
       * the spec does allow it for opaque_fd.  From the Vulkan 1.0.53 spec:
       *
       *    "If the import is temporary, the implementation must restore the
       *    semaphore to its prior permanent state after submitting the next
       *    semaphore wait operation."
       *
       * The spec says nothing whatsoever about signal operations on
       * temporarily imported semaphores so it appears they are allowed.
       * There are also CTS tests that require this to work.
       */
      struct anv_fence_impl *impl =
         fence->temporary.type != ANV_FENCE_TYPE_NONE ?
         &fence->temporary : &fence->permanent;

      switch (impl->type) {
      case ANV_FENCE_TYPE_BO:
         result = anv_queue_submit_add_fence_bo(submit, impl->bo.bo, true /* signal */);
         if (result != VK_SUCCESS)
            goto error;
         break;

      case ANV_FENCE_TYPE_SYNCOBJ: {
         /*
          * For the same reason we reset the signaled binary syncobj above,
          * also reset the fence's syncobj so that they don't contain a
          * signaled dma-fence.
          */
         result = anv_queue_submit_add_syncobj(submit, device, impl->syncobj,
                                               I915_EXEC_FENCE_SIGNAL);
         if (result != VK_SUCCESS)
            goto error;
         break;
      }

      default:
         unreachable("Invalid fence type");
      }
   }

   result = _anv_queue_submit(queue, &submit, false);
   if (result != VK_SUCCESS)
      goto error;

   if (fence && fence->permanent.type == ANV_FENCE_TYPE_BO) {
      /* If we have permanent BO fence, the only type of temporary possible
       * would be BO_WSI (because BO fences are not shareable). The Vulkan spec
       * also requires that the fence passed to vkQueueSubmit() be :
       *
       *    * unsignaled
       *    * not be associated with any other queue command that has not yet
       *      completed execution on that queue
       *
       * So the only acceptable type for the temporary is NONE.
       */
      assert(fence->temporary.type == ANV_FENCE_TYPE_NONE);

      /* Once the execbuf has returned, we need to set the fence state to
       * SUBMITTED.  We can't do this before calling execbuf because
       * anv_GetFenceStatus does take the global device lock before checking
       * fence->state.
       *
       * We set the fence state to SUBMITTED regardless of whether or not the
       * execbuf succeeds because we need to ensure that vkWaitForFences() and
       * vkGetFenceStatus() return a valid result (VK_ERROR_DEVICE_LOST or
       * VK_SUCCESS) in a finite amount of time even if execbuf fails.
       */
      fence->permanent.bo.state = ANV_BO_FENCE_STATE_SUBMITTED;
   }

 error:
   if (submit)
      anv_queue_submit_free(device, submit);

   return result;
}

VkResult anv_QueueSubmit(
    VkQueue                                     _queue,
    uint32_t                                    submitCount,
    const VkSubmitInfo*                         pSubmits,
    VkFence                                     fence)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);

   if (queue->device->no_hw)
      return VK_SUCCESS;

   /* Query for device status prior to submitting.  Technically, we don't need
    * to do this.  However, if we have a client that's submitting piles of
    * garbage, we would rather break as early as possible to keep the GPU
    * hanging contained.  If we don't check here, we'll either be waiting for
    * the kernel to kick us or we'll have to wait until the client waits on a
    * fence before we actually know whether or not we've hung.
    */
   VkResult result = anv_device_query_status(queue->device);
   if (result != VK_SUCCESS)
      return result;

   if (fence && submitCount == 0) {
      /* If we don't have any command buffers, we need to submit a dummy
       * batch to give GEM something to wait on.  We could, potentially,
       * come up with something more efficient but this shouldn't be a
       * common case.
       */
      result = anv_queue_submit(queue, NULL, NULL, NULL, 0, NULL, NULL, 0,
                                NULL, fence);
      goto out;
   }

   for (uint32_t i = 0; i < submitCount; i++) {
      /* Fence for this submit.  NULL for all but the last one */
      VkFence submit_fence = (i == submitCount - 1) ? fence : VK_NULL_HANDLE;

      const struct wsi_memory_signal_submit_info *mem_signal_info =
         vk_find_struct_const(pSubmits[i].pNext,
                              WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA);
      struct anv_bo *wsi_signal_bo =
         mem_signal_info && mem_signal_info->memory != VK_NULL_HANDLE ?
         anv_device_memory_from_handle(mem_signal_info->memory)->bo : NULL;

      const VkTimelineSemaphoreSubmitInfoKHR *timeline_info =
         vk_find_struct_const(pSubmits[i].pNext,
                              TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR);
      const uint64_t *wait_values =
         timeline_info && timeline_info->waitSemaphoreValueCount ?
         timeline_info->pWaitSemaphoreValues : NULL;
      const uint64_t *signal_values =
         timeline_info && timeline_info->signalSemaphoreValueCount ?
         timeline_info->pSignalSemaphoreValues : NULL;

      if (pSubmits[i].commandBufferCount == 0) {
         /* If we don't have any command buffers, we need to submit a dummy
          * batch to give GEM something to wait on.  We could, potentially,
          * come up with something more efficient but this shouldn't be a
          * common case.
          */
         result = anv_queue_submit(queue, NULL,
                                   pSubmits[i].pWaitSemaphores,
                                   wait_values,
                                   pSubmits[i].waitSemaphoreCount,
                                   pSubmits[i].pSignalSemaphores,
                                   signal_values,
                                   pSubmits[i].signalSemaphoreCount,
                                   wsi_signal_bo,
                                   submit_fence);
         if (result != VK_SUCCESS)
            goto out;

         continue;
      }

      for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
         ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer,
                         pSubmits[i].pCommandBuffers[j]);
         assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
         assert(!anv_batch_has_error(&cmd_buffer->batch));

         /* Fence for this execbuf.  NULL for all but the last one */
         VkFence execbuf_fence =
            (j == pSubmits[i].commandBufferCount - 1) ?
            submit_fence : VK_NULL_HANDLE;

         const VkSemaphore *in_semaphores = NULL, *out_semaphores = NULL;
         const uint64_t *in_values = NULL, *out_values = NULL;
         uint32_t num_in_semaphores = 0, num_out_semaphores = 0;
         if (j == 0) {
            /* Only the first batch gets the in semaphores */
            in_semaphores = pSubmits[i].pWaitSemaphores;
            in_values = wait_values;
            num_in_semaphores = pSubmits[i].waitSemaphoreCount;
         }

         if (j == pSubmits[i].commandBufferCount - 1) {
            /* Only the last batch gets the out semaphores */
            out_semaphores = pSubmits[i].pSignalSemaphores;
            out_values = signal_values;
            num_out_semaphores = pSubmits[i].signalSemaphoreCount;
         }

         result = anv_queue_submit(queue, cmd_buffer,
                                   in_semaphores, in_values, num_in_semaphores,
                                   out_semaphores, out_values, num_out_semaphores,
                                   wsi_signal_bo, execbuf_fence);
         if (result != VK_SUCCESS)
            goto out;
      }
   }

out:
   if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
      /* In the case that something has gone wrong we may end up with an
       * inconsistent state from which it may not be trivial to recover.
       * For example, we might have computed address relocations and
       * any future attempt to re-submit this job will need to know about
       * this and avoid computing relocation addresses again.
       *
       * To avoid this sort of issues, we assume that if something was
       * wrong during submission we must already be in a really bad situation
       * anyway (such us being out of memory) and return
       * VK_ERROR_DEVICE_LOST to ensure that clients do not attempt to
       * submit the same job again to this device.
       *
       * We skip doing this on VK_ERROR_DEVICE_LOST because
       * anv_device_set_lost() would have been called already by a callee of
       * anv_queue_submit().
       */
      result = anv_device_set_lost(queue->device, "vkQueueSubmit() failed");
   }

   return result;
}

VkResult anv_QueueWaitIdle(
    VkQueue                                     _queue)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);

   if (anv_device_is_lost(queue->device))
      return VK_ERROR_DEVICE_LOST;

   return anv_queue_submit_simple_batch(queue, NULL);
}

VkResult anv_CreateFence(
    VkDevice                                    _device,
    const VkFenceCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFence*                                    pFence)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_fence *fence;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);

   fence = vk_zalloc2(&device->alloc, pAllocator, sizeof(*fence), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (fence == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   if (device->physical->has_syncobj_wait) {
      fence->permanent.type = ANV_FENCE_TYPE_SYNCOBJ;

      uint32_t create_flags = 0;
      if (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT)
         create_flags |= DRM_SYNCOBJ_CREATE_SIGNALED;

      fence->permanent.syncobj = anv_gem_syncobj_create(device, create_flags);
      if (!fence->permanent.syncobj)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   } else {
      fence->permanent.type = ANV_FENCE_TYPE_BO;

      VkResult result = anv_bo_pool_alloc(&device->batch_bo_pool, 4096,
                                          &fence->permanent.bo.bo);
      if (result != VK_SUCCESS)
         return result;

      if (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) {
         fence->permanent.bo.state = ANV_BO_FENCE_STATE_SIGNALED;
      } else {
         fence->permanent.bo.state = ANV_BO_FENCE_STATE_RESET;
      }
   }

   *pFence = anv_fence_to_handle(fence);

   return VK_SUCCESS;
}

static void
anv_fence_impl_cleanup(struct anv_device *device,
                       struct anv_fence_impl *impl)
{
   switch (impl->type) {
   case ANV_FENCE_TYPE_NONE:
      /* Dummy.  Nothing to do */
      break;

   case ANV_FENCE_TYPE_BO:
      anv_bo_pool_free(&device->batch_bo_pool, impl->bo.bo);
      break;

   case ANV_FENCE_TYPE_WSI_BO:
      anv_device_release_bo(device, impl->bo.bo);
      break;

   case ANV_FENCE_TYPE_SYNCOBJ:
      anv_gem_syncobj_destroy(device, impl->syncobj);
      break;

   case ANV_FENCE_TYPE_WSI:
      impl->fence_wsi->destroy(impl->fence_wsi);
      break;

   default:
      unreachable("Invalid fence type");
   }

   impl->type = ANV_FENCE_TYPE_NONE;
}

void
anv_fence_reset_temporary(struct anv_device *device,
                          struct anv_fence *fence)
{
   if (fence->temporary.type == ANV_FENCE_TYPE_NONE)
      return;

   anv_fence_impl_cleanup(device, &fence->temporary);
}

void anv_DestroyFence(
    VkDevice                                    _device,
    VkFence                                     _fence,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_fence, fence, _fence);

   if (!fence)
      return;

   anv_fence_impl_cleanup(device, &fence->temporary);
   anv_fence_impl_cleanup(device, &fence->permanent);

   vk_free2(&device->alloc, pAllocator, fence);
}

VkResult anv_ResetFences(
    VkDevice                                    _device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   for (uint32_t i = 0; i < fenceCount; i++) {
      ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);

      /* From the Vulkan 1.0.53 spec:
       *
       *    "If any member of pFences currently has its payload imported with
       *    temporary permanence, that fence’s prior permanent payload is
       *    first restored. The remaining operations described therefore
       *    operate on the restored payload.
       */
      anv_fence_reset_temporary(device, fence);

      struct anv_fence_impl *impl = &fence->permanent;

      switch (impl->type) {
      case ANV_FENCE_TYPE_BO:
         impl->bo.state = ANV_BO_FENCE_STATE_RESET;
         break;

      case ANV_FENCE_TYPE_SYNCOBJ:
         anv_gem_syncobj_reset(device, impl->syncobj);
         break;

      default:
         unreachable("Invalid fence type");
      }
   }

   return VK_SUCCESS;
}

VkResult anv_GetFenceStatus(
    VkDevice                                    _device,
    VkFence                                     _fence)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_fence, fence, _fence);

   if (anv_device_is_lost(device))
      return VK_ERROR_DEVICE_LOST;

   struct anv_fence_impl *impl =
      fence->temporary.type != ANV_FENCE_TYPE_NONE ?
      &fence->temporary : &fence->permanent;

   switch (impl->type) {
   case ANV_FENCE_TYPE_BO:
   case ANV_FENCE_TYPE_WSI_BO:
      switch (impl->bo.state) {
      case ANV_BO_FENCE_STATE_RESET:
         /* If it hasn't even been sent off to the GPU yet, it's not ready */
         return VK_NOT_READY;

      case ANV_BO_FENCE_STATE_SIGNALED:
         /* It's been signaled, return success */
         return VK_SUCCESS;

      case ANV_BO_FENCE_STATE_SUBMITTED: {
         VkResult result = anv_device_bo_busy(device, impl->bo.bo);
         if (result == VK_SUCCESS) {
            impl->bo.state = ANV_BO_FENCE_STATE_SIGNALED;
            return VK_SUCCESS;
         } else {
            return result;
         }
      }
      default:
         unreachable("Invalid fence status");
      }

   case ANV_FENCE_TYPE_SYNCOBJ: {
      int ret = anv_gem_syncobj_wait(device, &impl->syncobj, 1, 0, true);
      if (ret == -1) {
         if (errno == ETIME) {
            return VK_NOT_READY;
         } else {
            /* We don't know the real error. */
            return anv_device_set_lost(device, "drm_syncobj_wait failed: %m");
         }
      } else {
         return VK_SUCCESS;
      }
   }

   default:
      unreachable("Invalid fence type");
   }
}

static VkResult
anv_wait_for_syncobj_fences(struct anv_device *device,
                            uint32_t fenceCount,
                            const VkFence *pFences,
                            bool waitAll,
                            uint64_t abs_timeout_ns)
{
   uint32_t *syncobjs = vk_zalloc(&device->alloc,
                                  sizeof(*syncobjs) * fenceCount, 8,
                                  VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!syncobjs)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < fenceCount; i++) {
      ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);
      assert(fence->permanent.type == ANV_FENCE_TYPE_SYNCOBJ);

      struct anv_fence_impl *impl =
         fence->temporary.type != ANV_FENCE_TYPE_NONE ?
         &fence->temporary : &fence->permanent;

      assert(impl->type == ANV_FENCE_TYPE_SYNCOBJ);
      syncobjs[i] = impl->syncobj;
   }

   /* The gem_syncobj_wait ioctl may return early due to an inherent
    * limitation in the way it computes timeouts.  Loop until we've actually
    * passed the timeout.
    */
   int ret;
   do {
      ret = anv_gem_syncobj_wait(device, syncobjs, fenceCount,
                                 abs_timeout_ns, waitAll);
   } while (ret == -1 && errno == ETIME && anv_gettime_ns() < abs_timeout_ns);

   vk_free(&device->alloc, syncobjs);

   if (ret == -1) {
      if (errno == ETIME) {
         return VK_TIMEOUT;
      } else {
         /* We don't know the real error. */
         return anv_device_set_lost(device, "drm_syncobj_wait failed: %m");
      }
   } else {
      return VK_SUCCESS;
   }
}

static VkResult
anv_wait_for_bo_fences(struct anv_device *device,
                       uint32_t fenceCount,
                       const VkFence *pFences,
                       bool waitAll,
                       uint64_t abs_timeout_ns)
{
   VkResult result = VK_SUCCESS;
   uint32_t pending_fences = fenceCount;
   while (pending_fences) {
      pending_fences = 0;
      bool signaled_fences = false;
      for (uint32_t i = 0; i < fenceCount; i++) {
         ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);

         struct anv_fence_impl *impl =
            fence->temporary.type != ANV_FENCE_TYPE_NONE ?
            &fence->temporary : &fence->permanent;
         assert(impl->type == ANV_FENCE_TYPE_BO ||
                impl->type == ANV_FENCE_TYPE_WSI_BO);

         switch (impl->bo.state) {
         case ANV_BO_FENCE_STATE_RESET:
            /* This fence hasn't been submitted yet, we'll catch it the next
             * time around.  Yes, this may mean we dead-loop but, short of
             * lots of locking and a condition variable, there's not much that
             * we can do about that.
             */
            pending_fences++;
            continue;

         case ANV_BO_FENCE_STATE_SIGNALED:
            /* This fence is not pending.  If waitAll isn't set, we can return
             * early.  Otherwise, we have to keep going.
             */
            if (!waitAll) {
               result = VK_SUCCESS;
               goto done;
            }
            continue;

         case ANV_BO_FENCE_STATE_SUBMITTED:
            /* These are the fences we really care about.  Go ahead and wait
             * on it until we hit a timeout.
             */
            result = anv_device_wait(device, impl->bo.bo,
                                     anv_get_relative_timeout(abs_timeout_ns));
            switch (result) {
            case VK_SUCCESS:
               impl->bo.state = ANV_BO_FENCE_STATE_SIGNALED;
               signaled_fences = true;
               if (!waitAll)
                  goto done;
               break;

            case VK_TIMEOUT:
               goto done;

            default:
               return result;
            }
         }
      }

      if (pending_fences && !signaled_fences) {
         /* If we've hit this then someone decided to vkWaitForFences before
          * they've actually submitted any of them to a queue.  This is a
          * fairly pessimal case, so it's ok to lock here and use a standard
          * pthreads condition variable.
          */
         pthread_mutex_lock(&device->mutex);

         /* It's possible that some of the fences have changed state since the
          * last time we checked.  Now that we have the lock, check for
          * pending fences again and don't wait if it's changed.
          */
         uint32_t now_pending_fences = 0;
         for (uint32_t i = 0; i < fenceCount; i++) {
            ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);
            if (fence->permanent.bo.state == ANV_BO_FENCE_STATE_RESET)
               now_pending_fences++;
         }
         assert(now_pending_fences <= pending_fences);

         if (now_pending_fences == pending_fences) {
            struct timespec abstime = {
               .tv_sec = abs_timeout_ns / NSEC_PER_SEC,
               .tv_nsec = abs_timeout_ns % NSEC_PER_SEC,
            };

            ASSERTED int ret;
            ret = pthread_cond_timedwait(&device->queue_submit,
                                         &device->mutex, &abstime);
            assert(ret != EINVAL);
            if (anv_gettime_ns() >= abs_timeout_ns) {
               pthread_mutex_unlock(&device->mutex);
               result = VK_TIMEOUT;
               goto done;
            }
         }

         pthread_mutex_unlock(&device->mutex);
      }
   }

done:
   if (anv_device_is_lost(device))
      return VK_ERROR_DEVICE_LOST;

   return result;
}

static VkResult
anv_wait_for_wsi_fence(struct anv_device *device,
                       struct anv_fence_impl *impl,
                       uint64_t abs_timeout)
{
   return impl->fence_wsi->wait(impl->fence_wsi, abs_timeout);
}

static VkResult
anv_wait_for_fences(struct anv_device *device,
                    uint32_t fenceCount,
                    const VkFence *pFences,
                    bool waitAll,
                    uint64_t abs_timeout)
{
   VkResult result = VK_SUCCESS;

   if (fenceCount <= 1 || waitAll) {
      for (uint32_t i = 0; i < fenceCount; i++) {
         ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);
         struct anv_fence_impl *impl =
            fence->temporary.type != ANV_FENCE_TYPE_NONE ?
            &fence->temporary : &fence->permanent;

         switch (impl->type) {
         case ANV_FENCE_TYPE_BO:
         case ANV_FENCE_TYPE_WSI_BO:
            result = anv_wait_for_bo_fences(device, 1, &pFences[i],
                                            true, abs_timeout);
            break;
         case ANV_FENCE_TYPE_SYNCOBJ:
            result = anv_wait_for_syncobj_fences(device, 1, &pFences[i],
                                                 true, abs_timeout);
            break;
         case ANV_FENCE_TYPE_WSI:
            result = anv_wait_for_wsi_fence(device, impl, abs_timeout);
            break;
         case ANV_FENCE_TYPE_NONE:
            result = VK_SUCCESS;
            break;
         }
         if (result != VK_SUCCESS)
            return result;
      }
   } else {
      do {
         for (uint32_t i = 0; i < fenceCount; i++) {
            if (anv_wait_for_fences(device, 1, &pFences[i], true, 0) == VK_SUCCESS)
               return VK_SUCCESS;
         }
      } while (anv_gettime_ns() < abs_timeout);
      result = VK_TIMEOUT;
   }
   return result;
}

static bool anv_all_fences_syncobj(uint32_t fenceCount, const VkFence *pFences)
{
   for (uint32_t i = 0; i < fenceCount; ++i) {
      ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);
      struct anv_fence_impl *impl =
         fence->temporary.type != ANV_FENCE_TYPE_NONE ?
         &fence->temporary : &fence->permanent;
      if (impl->type != ANV_FENCE_TYPE_SYNCOBJ)
         return false;
   }
   return true;
}

static bool anv_all_fences_bo(uint32_t fenceCount, const VkFence *pFences)
{
   for (uint32_t i = 0; i < fenceCount; ++i) {
      ANV_FROM_HANDLE(anv_fence, fence, pFences[i]);
      struct anv_fence_impl *impl =
         fence->temporary.type != ANV_FENCE_TYPE_NONE ?
         &fence->temporary : &fence->permanent;
      if (impl->type != ANV_FENCE_TYPE_BO &&
          impl->type != ANV_FENCE_TYPE_WSI_BO)
         return false;
   }
   return true;
}

VkResult anv_WaitForFences(
    VkDevice                                    _device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences,
    VkBool32                                    waitAll,
    uint64_t                                    timeout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   if (device->no_hw)
      return VK_SUCCESS;

   if (anv_device_is_lost(device))
      return VK_ERROR_DEVICE_LOST;

   uint64_t abs_timeout = anv_get_absolute_timeout(timeout);
   if (anv_all_fences_syncobj(fenceCount, pFences)) {
      return anv_wait_for_syncobj_fences(device, fenceCount, pFences,
                                         waitAll, abs_timeout);
   } else if (anv_all_fences_bo(fenceCount, pFences)) {
      return anv_wait_for_bo_fences(device, fenceCount, pFences,
                                    waitAll, abs_timeout);
   } else {
      return anv_wait_for_fences(device, fenceCount, pFences,
                                 waitAll, abs_timeout);
   }
}

void anv_GetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice                            physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo*    pExternalFenceInfo,
    VkExternalFenceProperties*                  pExternalFenceProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   switch (pExternalFenceInfo->handleType) {
   case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT:
      if (device->has_syncobj_wait) {
         pExternalFenceProperties->exportFromImportedHandleTypes =
            VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT |
            VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
         pExternalFenceProperties->compatibleHandleTypes =
            VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT |
            VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
         pExternalFenceProperties->externalFenceFeatures =
            VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
         return;
      }
      break;

   default:
      break;
   }

   pExternalFenceProperties->exportFromImportedHandleTypes = 0;
   pExternalFenceProperties->compatibleHandleTypes = 0;
   pExternalFenceProperties->externalFenceFeatures = 0;
}

VkResult anv_ImportFenceFdKHR(
    VkDevice                                    _device,
    const VkImportFenceFdInfoKHR*               pImportFenceFdInfo)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_fence, fence, pImportFenceFdInfo->fence);
   int fd = pImportFenceFdInfo->fd;

   assert(pImportFenceFdInfo->sType ==
          VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR);

   struct anv_fence_impl new_impl = {
      .type = ANV_FENCE_TYPE_NONE,
   };

   switch (pImportFenceFdInfo->handleType) {
   case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT:
      new_impl.type = ANV_FENCE_TYPE_SYNCOBJ;

      new_impl.syncobj = anv_gem_syncobj_fd_to_handle(device, fd);
      if (!new_impl.syncobj)
         return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);

      break;

   case VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT: {
      /* Sync files are a bit tricky.  Because we want to continue using the
       * syncobj implementation of WaitForFences, we don't use the sync file
       * directly but instead import it into a syncobj.
       */
      new_impl.type = ANV_FENCE_TYPE_SYNCOBJ;

      /* "If handleType is VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, the
       *  special value -1 for fd is treated like a valid sync file descriptor
       *  referring to an object that has already signaled. The import
       *  operation will succeed and the VkFence will have a temporarily
       *  imported payload as if a valid file descriptor had been provided."
       */
      uint32_t create_flags = 0;
      if (fd == -1)
         create_flags |= DRM_SYNCOBJ_CREATE_SIGNALED;

      new_impl.syncobj = anv_gem_syncobj_create(device, create_flags);
      if (!new_impl.syncobj)
         return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

      if (fd != -1 &&
          anv_gem_syncobj_import_sync_file(device, new_impl.syncobj, fd)) {
         anv_gem_syncobj_destroy(device, new_impl.syncobj);
         return vk_errorf(device, NULL, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                          "syncobj sync file import failed: %m");
      }
      break;
   }

   default:
      return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   /* From the Vulkan 1.0.53 spec:
    *
    *    "Importing a fence payload from a file descriptor transfers
    *    ownership of the file descriptor from the application to the
    *    Vulkan implementation. The application must not perform any
    *    operations on the file descriptor after a successful import."
    *
    * If the import fails, we leave the file descriptor open.
    */
   close(fd);

   if (pImportFenceFdInfo->flags & VK_FENCE_IMPORT_TEMPORARY_BIT) {
      anv_fence_impl_cleanup(device, &fence->temporary);
      fence->temporary = new_impl;
   } else {
      anv_fence_impl_cleanup(device, &fence->permanent);
      fence->permanent = new_impl;
   }

   return VK_SUCCESS;
}

VkResult anv_GetFenceFdKHR(
    VkDevice                                    _device,
    const VkFenceGetFdInfoKHR*                  pGetFdInfo,
    int*                                        pFd)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_fence, fence, pGetFdInfo->fence);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR);

   struct anv_fence_impl *impl =
      fence->temporary.type != ANV_FENCE_TYPE_NONE ?
      &fence->temporary : &fence->permanent;

   assert(impl->type == ANV_FENCE_TYPE_SYNCOBJ);
   switch (pGetFdInfo->handleType) {
   case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT: {
      int fd = anv_gem_syncobj_handle_to_fd(device, impl->syncobj);
      if (fd < 0)
         return vk_error(VK_ERROR_TOO_MANY_OBJECTS);

      *pFd = fd;
      break;
   }

   case VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT: {
      int fd = anv_gem_syncobj_export_sync_file(device, impl->syncobj);
      if (fd < 0)
         return vk_error(VK_ERROR_TOO_MANY_OBJECTS);

      *pFd = fd;
      break;
   }

   default:
      unreachable("Invalid fence export handle type");
   }

   /* From the Vulkan 1.0.53 spec:
    *
    *    "Export operations have the same transference as the specified handle
    *    type’s import operations. [...] If the fence was using a
    *    temporarily imported payload, the fence’s prior permanent payload
    *    will be restored.
    */
   if (impl == &fence->temporary)
      anv_fence_impl_cleanup(device, impl);

   return VK_SUCCESS;
}

// Queue semaphore functions

static VkSemaphoreTypeKHR
get_semaphore_type(const void *pNext, uint64_t *initial_value)
{
   const VkSemaphoreTypeCreateInfoKHR *type_info =
      vk_find_struct_const(pNext, SEMAPHORE_TYPE_CREATE_INFO_KHR);

   if (!type_info)
      return VK_SEMAPHORE_TYPE_BINARY_KHR;

   if (initial_value)
      *initial_value = type_info->initialValue;
   return type_info->semaphoreType;
}

static VkResult
binary_semaphore_create(struct anv_device *device,
                        struct anv_semaphore_impl *impl,
                        bool exportable)
{
   if (device->physical->has_syncobj) {
      impl->type = ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ;
      impl->syncobj = anv_gem_syncobj_create(device, 0);
      if (!impl->syncobj)
            return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
      return VK_SUCCESS;
   } else {
      impl->type = ANV_SEMAPHORE_TYPE_BO;
      VkResult result =
         anv_device_alloc_bo(device, 4096,
                             ANV_BO_ALLOC_EXTERNAL |
                             ANV_BO_ALLOC_IMPLICIT_SYNC,
                             0 /* explicit_address */,
                             &impl->bo);
      /* If we're going to use this as a fence, we need to *not* have the
       * EXEC_OBJECT_ASYNC bit set.
       */
      assert(!(impl->bo->flags & EXEC_OBJECT_ASYNC));
      return result;
   }
}

static VkResult
timeline_semaphore_create(struct anv_device *device,
                          struct anv_semaphore_impl *impl,
                          uint64_t initial_value)
{
   impl->type = ANV_SEMAPHORE_TYPE_TIMELINE;
   anv_timeline_init(device, &impl->timeline, initial_value);
   return VK_SUCCESS;
}

VkResult anv_CreateSemaphore(
    VkDevice                                    _device,
    const VkSemaphoreCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSemaphore*                                pSemaphore)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_semaphore *semaphore;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);

   uint64_t timeline_value = 0;
   VkSemaphoreTypeKHR sem_type = get_semaphore_type(pCreateInfo->pNext, &timeline_value);

   semaphore = vk_alloc(&device->alloc, sizeof(*semaphore), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (semaphore == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   p_atomic_set(&semaphore->refcount, 1);

   const VkExportSemaphoreCreateInfo *export =
      vk_find_struct_const(pCreateInfo->pNext, EXPORT_SEMAPHORE_CREATE_INFO);
    VkExternalSemaphoreHandleTypeFlags handleTypes =
      export ? export->handleTypes : 0;
   VkResult result;

   if (handleTypes == 0) {
      if (sem_type == VK_SEMAPHORE_TYPE_BINARY_KHR)
         result = binary_semaphore_create(device, &semaphore->permanent, false);
      else
         result = timeline_semaphore_create(device, &semaphore->permanent, timeline_value);
      if (result != VK_SUCCESS) {
         vk_free2(&device->alloc, pAllocator, semaphore);
         return result;
      }
   } else if (handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
      assert(handleTypes == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
      assert(sem_type == VK_SEMAPHORE_TYPE_BINARY_KHR);
      result = binary_semaphore_create(device, &semaphore->permanent, true);
      if (result != VK_SUCCESS) {
         vk_free2(&device->alloc, pAllocator, semaphore);
         return result;
      }
   } else if (handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
      assert(handleTypes == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
      assert(sem_type == VK_SEMAPHORE_TYPE_BINARY_KHR);
      if (device->physical->has_syncobj) {
         semaphore->permanent.type = ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ;
         semaphore->permanent.syncobj = anv_gem_syncobj_create(device, 0);
         if (!semaphore->permanent.syncobj) {
            vk_free2(&device->alloc, pAllocator, semaphore);
            return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
         }
      } else {
         semaphore->permanent.type = ANV_SEMAPHORE_TYPE_SYNC_FILE;
         semaphore->permanent.fd = -1;
      }
   } else {
      assert(!"Unknown handle type");
      vk_free2(&device->alloc, pAllocator, semaphore);
      return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   semaphore->temporary.type = ANV_SEMAPHORE_TYPE_NONE;

   *pSemaphore = anv_semaphore_to_handle(semaphore);

   return VK_SUCCESS;
}

static void
anv_semaphore_impl_cleanup(struct anv_device *device,
                           struct anv_semaphore_impl *impl)
{
   switch (impl->type) {
   case ANV_SEMAPHORE_TYPE_NONE:
   case ANV_SEMAPHORE_TYPE_DUMMY:
      /* Dummy.  Nothing to do */
      break;

   case ANV_SEMAPHORE_TYPE_BO:
   case ANV_SEMAPHORE_TYPE_WSI_BO:
      anv_device_release_bo(device, impl->bo);
      break;

   case ANV_SEMAPHORE_TYPE_SYNC_FILE:
      if (impl->fd >= 0)
         close(impl->fd);
      break;

   case ANV_SEMAPHORE_TYPE_TIMELINE:
      anv_timeline_finish(device, &impl->timeline);
      break;

   case ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ:
      anv_gem_syncobj_destroy(device, impl->syncobj);
      break;

   default:
      unreachable("Invalid semaphore type");
   }

   impl->type = ANV_SEMAPHORE_TYPE_NONE;
}

void
anv_semaphore_reset_temporary(struct anv_device *device,
                              struct anv_semaphore *semaphore)
{
   if (semaphore->temporary.type == ANV_SEMAPHORE_TYPE_NONE)
      return;

   anv_semaphore_impl_cleanup(device, &semaphore->temporary);
}

static struct anv_semaphore *
anv_semaphore_ref(struct anv_semaphore *semaphore)
{
   assert(semaphore->refcount);
   p_atomic_inc(&semaphore->refcount);
   return semaphore;
}

static void
anv_semaphore_unref(struct anv_device *device, struct anv_semaphore *semaphore)
{
   if (!p_atomic_dec_zero(&semaphore->refcount))
      return;

   anv_semaphore_impl_cleanup(device, &semaphore->temporary);
   anv_semaphore_impl_cleanup(device, &semaphore->permanent);
   vk_free(&device->alloc, semaphore);
}

void anv_DestroySemaphore(
    VkDevice                                    _device,
    VkSemaphore                                 _semaphore,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_semaphore, semaphore, _semaphore);

   if (semaphore == NULL)
      return;

   anv_semaphore_unref(device, semaphore);
}

void anv_GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice                            physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties*               pExternalSemaphoreProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   VkSemaphoreTypeKHR sem_type =
      get_semaphore_type(pExternalSemaphoreInfo->pNext, NULL);

   switch (pExternalSemaphoreInfo->handleType) {
   case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
      /* Timeline semaphores are not exportable. */
      if (sem_type == VK_SEMAPHORE_TYPE_TIMELINE_KHR)
         break;
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      pExternalSemaphoreProperties->compatibleHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
      return;

   case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT:
      if (sem_type == VK_SEMAPHORE_TYPE_TIMELINE_KHR)
         break;
      if (!device->has_exec_fence)
         break;
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->compatibleHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
      return;

   default:
      break;
   }

   pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
   pExternalSemaphoreProperties->compatibleHandleTypes = 0;
   pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
}

VkResult anv_ImportSemaphoreFdKHR(
    VkDevice                                    _device,
    const VkImportSemaphoreFdInfoKHR*           pImportSemaphoreFdInfo)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_semaphore, semaphore, pImportSemaphoreFdInfo->semaphore);
   int fd = pImportSemaphoreFdInfo->fd;

   struct anv_semaphore_impl new_impl = {
      .type = ANV_SEMAPHORE_TYPE_NONE,
   };

   switch (pImportSemaphoreFdInfo->handleType) {
   case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
      if (device->physical->has_syncobj) {
         new_impl.type = ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ;

         new_impl.syncobj = anv_gem_syncobj_fd_to_handle(device, fd);
         if (!new_impl.syncobj)
            return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
      } else {
         new_impl.type = ANV_SEMAPHORE_TYPE_BO;

         VkResult result = anv_device_import_bo(device, fd,
                                                ANV_BO_ALLOC_EXTERNAL |
                                                ANV_BO_ALLOC_IMPLICIT_SYNC,
                                                0 /* client_address */,
                                                &new_impl.bo);
         if (result != VK_SUCCESS)
            return result;

         if (new_impl.bo->size < 4096) {
            anv_device_release_bo(device, new_impl.bo);
            return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
         }

         /* If we're going to use this as a fence, we need to *not* have the
          * EXEC_OBJECT_ASYNC bit set.
          */
         assert(!(new_impl.bo->flags & EXEC_OBJECT_ASYNC));
      }

      /* From the Vulkan spec:
       *
       *    "Importing semaphore state from a file descriptor transfers
       *    ownership of the file descriptor from the application to the
       *    Vulkan implementation. The application must not perform any
       *    operations on the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd);
      break;

   case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT:
      if (device->physical->has_syncobj) {
         new_impl = (struct anv_semaphore_impl) {
            .type = ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ,
            .syncobj = anv_gem_syncobj_create(device, 0),
         };
         if (!new_impl.syncobj)
            return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
         if (anv_gem_syncobj_import_sync_file(device, new_impl.syncobj, fd)) {
            anv_gem_syncobj_destroy(device, new_impl.syncobj);
            return vk_errorf(device, NULL, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                             "syncobj sync file import failed: %m");
         }
         /* Ownership of the FD is transfered to Anv. Since we don't need it
          * anymore because the associated fence has been put into a syncobj,
          * we must close the FD.
          */
         close(fd);
      } else {
         new_impl = (struct anv_semaphore_impl) {
            .type = ANV_SEMAPHORE_TYPE_SYNC_FILE,
            .fd = fd,
         };
      }
      break;

   default:
      return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   if (pImportSemaphoreFdInfo->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT) {
      anv_semaphore_impl_cleanup(device, &semaphore->temporary);
      semaphore->temporary = new_impl;
   } else {
      anv_semaphore_impl_cleanup(device, &semaphore->permanent);
      semaphore->permanent = new_impl;
   }

   return VK_SUCCESS;
}

VkResult anv_GetSemaphoreFdKHR(
    VkDevice                                    _device,
    const VkSemaphoreGetFdInfoKHR*              pGetFdInfo,
    int*                                        pFd)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_semaphore, semaphore, pGetFdInfo->semaphore);
   VkResult result;
   int fd;

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR);

   struct anv_semaphore_impl *impl =
      semaphore->temporary.type != ANV_SEMAPHORE_TYPE_NONE ?
      &semaphore->temporary : &semaphore->permanent;

   switch (impl->type) {
   case ANV_SEMAPHORE_TYPE_BO:
      result = anv_device_export_bo(device, impl->bo, pFd);
      if (result != VK_SUCCESS)
         return result;
      break;

   case ANV_SEMAPHORE_TYPE_SYNC_FILE: {
      /* There's a potential race here with vkQueueSubmit if you are trying
       * to export a semaphore Fd while the queue submit is still happening.
       * This can happen if we see all dependencies get resolved via timeline
       * semaphore waits completing before the execbuf completes and we
       * process the resulting out fence.  To work around this, take a lock
       * around grabbing the fd.
       */
      pthread_mutex_lock(&device->mutex);

      /* From the Vulkan 1.0.53 spec:
       *
       *    "...exporting a semaphore payload to a handle with copy
       *    transference has the same side effects on the source
       *    semaphore’s payload as executing a semaphore wait operation."
       *
       * In other words, it may still be a SYNC_FD semaphore, but it's now
       * considered to have been waited on and no longer has a sync file
       * attached.
       */
      int fd = impl->fd;
      impl->fd = -1;

      pthread_mutex_unlock(&device->mutex);

      /* There are two reasons why this could happen:
       *
       *  1) The user is trying to export without submitting something that
       *     signals the semaphore.  If this is the case, it's their bug so
       *     what we return here doesn't matter.
       *
       *  2) The kernel didn't give us a file descriptor.  The most likely
       *     reason for this is running out of file descriptors.
       */
      if (fd < 0)
         return vk_error(VK_ERROR_TOO_MANY_OBJECTS);

      *pFd = fd;
      return VK_SUCCESS;
   }

   case ANV_SEMAPHORE_TYPE_DRM_SYNCOBJ:
      if (pGetFdInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)
         fd = anv_gem_syncobj_export_sync_file(device, impl->syncobj);
      else {
         assert(pGetFdInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
         fd = anv_gem_syncobj_handle_to_fd(device, impl->syncobj);
      }
      if (fd < 0)
         return vk_error(VK_ERROR_TOO_MANY_OBJECTS);
      *pFd = fd;
      break;

   default:
      return vk_error(VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   /* From the Vulkan 1.0.53 spec:
    *
    *    "Export operations have the same transference as the specified handle
    *    type’s import operations. [...] If the semaphore was using a
    *    temporarily imported payload, the semaphore’s prior permanent payload
    *    will be restored.
    */
   if (impl == &semaphore->temporary)
      anv_semaphore_impl_cleanup(device, impl);

   return VK_SUCCESS;
}

VkResult anv_GetSemaphoreCounterValue(
    VkDevice                                    _device,
    VkSemaphore                                 _semaphore,
    uint64_t*                                   pValue)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_semaphore, semaphore, _semaphore);

   struct anv_semaphore_impl *impl =
      semaphore->temporary.type != ANV_SEMAPHORE_TYPE_NONE ?
      &semaphore->temporary : &semaphore->permanent;

   switch (impl->type) {
   case ANV_SEMAPHORE_TYPE_TIMELINE: {
      pthread_mutex_lock(&device->mutex);
      anv_timeline_gc_locked(device, &impl->timeline);
      *pValue = impl->timeline.highest_past;
      pthread_mutex_unlock(&device->mutex);
      return VK_SUCCESS;
   }

   default:
      unreachable("Invalid semaphore type");
   }
}

static VkResult
anv_timeline_wait_locked(struct anv_device *device,
                         struct anv_timeline *timeline,
                         uint64_t serial, uint64_t abs_timeout_ns)
{
   /* Wait on the queue_submit condition variable until the timeline has a
    * time point pending that's at least as high as serial.
    */
   while (timeline->highest_pending < serial) {
      struct timespec abstime = {
         .tv_sec = abs_timeout_ns / NSEC_PER_SEC,
         .tv_nsec = abs_timeout_ns % NSEC_PER_SEC,
      };

      int ret = pthread_cond_timedwait(&device->queue_submit,
                                       &device->mutex, &abstime);
      assert(ret != EINVAL);
      if (anv_gettime_ns() >= abs_timeout_ns &&
          timeline->highest_pending < serial)
         return VK_TIMEOUT;
   }

   while (1) {
      VkResult result = anv_timeline_gc_locked(device, timeline);
      if (result != VK_SUCCESS)
         return result;

      if (timeline->highest_past >= serial)
         return VK_SUCCESS;

      /* If we got here, our earliest time point has a busy BO */
      struct anv_timeline_point *point =
         list_first_entry(&timeline->points,
                          struct anv_timeline_point, link);

      /* Drop the lock while we wait. */
      point->waiting++;
      pthread_mutex_unlock(&device->mutex);

      result = anv_device_wait(device, point->bo,
                               anv_get_relative_timeout(abs_timeout_ns));

      /* Pick the mutex back up */
      pthread_mutex_lock(&device->mutex);
      point->waiting--;

      /* This covers both VK_TIMEOUT and VK_ERROR_DEVICE_LOST */
      if (result != VK_SUCCESS)
         return result;
   }
}

static VkResult
anv_timelines_wait(struct anv_device *device,
                   struct anv_timeline **timelines,
                   const uint64_t *serials,
                   uint32_t n_timelines,
                   bool wait_all,
                   uint64_t abs_timeout_ns)
{
   if (!wait_all && n_timelines > 1) {
      pthread_mutex_lock(&device->mutex);

      while (1) {
         VkResult result;
         for (uint32_t i = 0; i < n_timelines; i++) {
            result =
               anv_timeline_wait_locked(device, timelines[i], serials[i], 0);
            if (result != VK_TIMEOUT)
               break;
         }

         if (result != VK_TIMEOUT ||
             anv_gettime_ns() >= abs_timeout_ns) {
            pthread_mutex_unlock(&device->mutex);
            return result;
         }

         /* If none of them are ready do a short wait so we don't completely
          * spin while holding the lock. The 10us is completely arbitrary.
          */
         uint64_t abs_short_wait_ns =
            anv_get_absolute_timeout(
               MIN2((anv_gettime_ns() - abs_timeout_ns) / 10, 10 * 1000));
         struct timespec abstime = {
            .tv_sec = abs_short_wait_ns / NSEC_PER_SEC,
            .tv_nsec = abs_short_wait_ns % NSEC_PER_SEC,
         };
         ASSERTED int ret;
         ret = pthread_cond_timedwait(&device->queue_submit,
                                      &device->mutex, &abstime);
         assert(ret != EINVAL);
      }
   } else {
      VkResult result = VK_SUCCESS;
      pthread_mutex_lock(&device->mutex);
      for (uint32_t i = 0; i < n_timelines; i++) {
         result =
            anv_timeline_wait_locked(device, timelines[i],
                                     serials[i], abs_timeout_ns);
         if (result != VK_SUCCESS)
            break;
      }
      pthread_mutex_unlock(&device->mutex);
      return result;
   }
}

VkResult anv_WaitSemaphores(
    VkDevice                                    _device,
    const VkSemaphoreWaitInfoKHR*               pWaitInfo,
    uint64_t                                    timeout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   if (device->no_hw)
      return VK_SUCCESS;

   struct anv_timeline **timelines =
      vk_alloc(&device->alloc,
               pWaitInfo->semaphoreCount * sizeof(*timelines),
               8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!timelines)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   uint64_t *values = vk_alloc(&device->alloc,
                               pWaitInfo->semaphoreCount * sizeof(*values),
                               8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!values) {
      vk_free(&device->alloc, timelines);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   uint32_t handle_count = 0;
   for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
      ANV_FROM_HANDLE(anv_semaphore, semaphore, pWaitInfo->pSemaphores[i]);
      struct anv_semaphore_impl *impl =
         semaphore->temporary.type != ANV_SEMAPHORE_TYPE_NONE ?
         &semaphore->temporary : &semaphore->permanent;

      assert(impl->type == ANV_SEMAPHORE_TYPE_TIMELINE);

      if (pWaitInfo->pValues[i] == 0)
         continue;

      timelines[handle_count] = &impl->timeline;
      values[handle_count] = pWaitInfo->pValues[i];
      handle_count++;
   }

   VkResult result = VK_SUCCESS;
   if (handle_count > 0) {
      result = anv_timelines_wait(device, timelines, values, handle_count,
                                  !(pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT_KHR),
                                  anv_get_absolute_timeout(timeout));
   }

   vk_free(&device->alloc, timelines);
   vk_free(&device->alloc, values);

   return result;
}

VkResult anv_SignalSemaphore(
    VkDevice                                    _device,
    const VkSemaphoreSignalInfoKHR*             pSignalInfo)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_semaphore, semaphore, pSignalInfo->semaphore);

   struct anv_semaphore_impl *impl =
      semaphore->temporary.type != ANV_SEMAPHORE_TYPE_NONE ?
      &semaphore->temporary : &semaphore->permanent;

   switch (impl->type) {
   case ANV_SEMAPHORE_TYPE_TIMELINE: {
      pthread_mutex_lock(&device->mutex);

      VkResult result = anv_timeline_gc_locked(device, &impl->timeline);

      assert(pSignalInfo->value > impl->timeline.highest_pending);

      impl->timeline.highest_pending = impl->timeline.highest_past = pSignalInfo->value;

      if (result == VK_SUCCESS)
         result = anv_device_submit_deferred_locked(device);

      pthread_cond_broadcast(&device->queue_submit);
      pthread_mutex_unlock(&device->mutex);
      return result;
   }

   default:
      unreachable("Invalid semaphore type");
   }
}
