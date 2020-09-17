/*
 * Copyright © 2017, Google Inc.
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

#include <hardware/gralloc.h>

#if ANDROID_API_LEVEL >= 26
#include <hardware/gralloc1.h>
#endif

#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <sync/sync.h>

#include "anv_private.h"
#include "vk_format_info.h"
#include "vk_util.h"

static int anv_hal_open(const struct hw_module_t* mod, const char* id, struct hw_device_t** dev);
static int anv_hal_close(struct hw_device_t *dev);

static void UNUSED
static_asserts(void)
{
   STATIC_ASSERT(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC);
}

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common = {
      .tag = HARDWARE_MODULE_TAG,
      .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
      .hal_api_version = HARDWARE_MAKE_API_VERSION(1, 0),
      .id = HWVULKAN_HARDWARE_MODULE_ID,
      .name = "Intel Vulkan HAL",
      .author = "Intel",
      .methods = &(hw_module_methods_t) {
         .open = anv_hal_open,
      },
   },
};

/* If any bits in test_mask are set, then unset them and return true. */
static inline bool
unmask32(uint32_t *inout_mask, uint32_t test_mask)
{
   uint32_t orig_mask = *inout_mask;
   *inout_mask &= ~test_mask;
   return *inout_mask != orig_mask;
}

static int
anv_hal_open(const struct hw_module_t* mod, const char* id,
             struct hw_device_t** dev)
{
   assert(mod == &HAL_MODULE_INFO_SYM.common);
   assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

   hwvulkan_device_t *hal_dev = malloc(sizeof(*hal_dev));
   if (!hal_dev)
      return -1;

   *hal_dev = (hwvulkan_device_t) {
      .common = {
         .tag = HARDWARE_DEVICE_TAG,
         .version = HWVULKAN_DEVICE_API_VERSION_0_1,
         .module = &HAL_MODULE_INFO_SYM.common,
         .close = anv_hal_close,
      },
     .EnumerateInstanceExtensionProperties = anv_EnumerateInstanceExtensionProperties,
     .CreateInstance = anv_CreateInstance,
     .GetInstanceProcAddr = anv_GetInstanceProcAddr,
   };

   *dev = &hal_dev->common;
   return 0;
}

static int
anv_hal_close(struct hw_device_t *dev)
{
   /* hwvulkan.h claims that hw_device_t::close() is never called. */
   return -1;
}

#if ANDROID_API_LEVEL >= 26
static VkResult
get_ahw_buffer_format_properties(
   VkDevice device_h,
   const struct AHardwareBuffer *buffer,
   VkAndroidHardwareBufferFormatPropertiesANDROID *pProperties)
{
   ANV_FROM_HANDLE(anv_device, device, device_h);

   /* Get a description of buffer contents . */
   AHardwareBuffer_Desc desc;
   AHardwareBuffer_describe(buffer, &desc);

   /* Verify description. */
   uint64_t gpu_usage =
      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
      AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
      AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;

   /* "Buffer must be a valid Android hardware buffer object with at least
    * one of the AHARDWAREBUFFER_USAGE_GPU_* usage flags."
    */
   if (!(desc.usage & (gpu_usage)))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   /* Fill properties fields based on description. */
   VkAndroidHardwareBufferFormatPropertiesANDROID *p = pProperties;

   p->format = vk_format_from_android(desc.format, desc.usage);

   const struct anv_format *anv_format = anv_get_format(p->format);
   p->externalFormat = (uint64_t) (uintptr_t) anv_format;

   /* Default to OPTIMAL tiling but set to linear in case
    * of AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER usage.
    */
   VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;

   if (desc.usage & AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER)
      tiling = VK_IMAGE_TILING_LINEAR;

   p->formatFeatures =
      anv_get_image_format_features(&device->info, p->format, anv_format,
                                    tiling);

   /* "Images can be created with an external format even if the Android hardware
    *  buffer has a format which has an equivalent Vulkan format to enable
    *  consistent handling of images from sources that might use either category
    *  of format. However, all images created with an external format are subject
    *  to the valid usage requirements associated with external formats, even if
    *  the Android hardware buffer’s format has a Vulkan equivalent."
    *
    * "The formatFeatures member *must* include
    *  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT and at least one of
    *  VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT or
    *  VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT"
    */
   p->formatFeatures |=
      VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;

   /* "Implementations may not always be able to determine the color model,
    *  numerical range, or chroma offsets of the image contents, so the values
    *  in VkAndroidHardwareBufferFormatPropertiesANDROID are only suggestions.
    *  Applications should treat these values as sensible defaults to use in
    *  the absence of more reliable information obtained through some other
    *  means."
    */
   p->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_IDENTITY;
   p->samplerYcbcrConversionComponents.g = VK_COMPONENT_SWIZZLE_IDENTITY;
   p->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_IDENTITY;
   p->samplerYcbcrConversionComponents.a = VK_COMPONENT_SWIZZLE_IDENTITY;

   p->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
   p->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

   p->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
   p->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;

   return VK_SUCCESS;
}

VkResult
anv_GetAndroidHardwareBufferPropertiesANDROID(
   VkDevice device_h,
   const struct AHardwareBuffer *buffer,
   VkAndroidHardwareBufferPropertiesANDROID *pProperties)
{
   ANV_FROM_HANDLE(anv_device, dev, device_h);

   VkAndroidHardwareBufferFormatPropertiesANDROID *format_prop =
      vk_find_struct(pProperties->pNext,
                     ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);

   /* Fill format properties of an Android hardware buffer. */
   if (format_prop)
      get_ahw_buffer_format_properties(device_h, buffer, format_prop);

   /* NOTE - We support buffers with only one handle but do not error on
    * multiple handle case. Reason is that we want to support YUV formats
    * where we have many logical planes but they all point to the same
    * buffer, like is the case with VK_FORMAT_G8_B8R8_2PLANE_420_UNORM.
    */
   const native_handle_t *handle =
      AHardwareBuffer_getNativeHandle(buffer);
   int dma_buf = (handle && handle->numFds) ? handle->data[0] : -1;
   if (dma_buf < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   /* All memory types. */
   uint32_t memory_types = (1ull << dev->physical->memory.type_count) - 1;

   pProperties->allocationSize = lseek(dma_buf, 0, SEEK_END);
   pProperties->memoryTypeBits = memory_types;

   return VK_SUCCESS;
}

VkResult
anv_GetMemoryAndroidHardwareBufferANDROID(
   VkDevice device_h,
   const VkMemoryGetAndroidHardwareBufferInfoANDROID *pInfo,
   struct AHardwareBuffer **pBuffer)
{
   ANV_FROM_HANDLE(anv_device_memory, mem, pInfo->memory);

   /* Some quotes from Vulkan spec:
    *
    * "If the device memory was created by importing an Android hardware
    * buffer, vkGetMemoryAndroidHardwareBufferANDROID must return that same
    * Android hardware buffer object."
    *
    * "VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID must
    * have been included in VkExportMemoryAllocateInfo::handleTypes when
    * memory was created."
    */
   if (mem->ahw) {
      *pBuffer = mem->ahw;
      /* Increase refcount. */
      AHardwareBuffer_acquire(mem->ahw);
      return VK_SUCCESS;
   }

   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

#endif

/* Construct ahw usage mask from image usage bits, see
 * 'AHardwareBuffer Usage Equivalence' in Vulkan spec.
 */
uint64_t
anv_ahw_usage_from_vk_usage(const VkImageCreateFlags vk_create,
                            const VkImageUsageFlags vk_usage)
{
   uint64_t ahw_usage = 0;
#if ANDROID_API_LEVEL >= 26
   if (vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (vk_usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (vk_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

   if (vk_create & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;

   if (vk_create & VK_IMAGE_CREATE_PROTECTED_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;

   /* No usage bits set - set at least one GPU usage. */
   if (ahw_usage == 0)
      ahw_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
#endif
   return ahw_usage;
}

/*
 * Called from anv_AllocateMemory when import AHardwareBuffer.
 */
VkResult
anv_import_ahw_memory(VkDevice device_h,
                      struct anv_device_memory *mem,
                      const VkImportAndroidHardwareBufferInfoANDROID *info)
{
#if ANDROID_API_LEVEL >= 26
   ANV_FROM_HANDLE(anv_device, device, device_h);

   /* Import from AHardwareBuffer to anv_device_memory. */
   const native_handle_t *handle =
      AHardwareBuffer_getNativeHandle(info->buffer);

   /* NOTE - We support buffers with only one handle but do not error on
    * multiple handle case. Reason is that we want to support YUV formats
    * where we have many logical planes but they all point to the same
    * buffer, like is the case with VK_FORMAT_G8_B8R8_2PLANE_420_UNORM.
    */
   int dma_buf = (handle && handle->numFds) ? handle->data[0] : -1;
   if (dma_buf < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkResult result = anv_device_import_bo(device, dma_buf, 0,
                                          0 /* client_address */,
                                          &mem->bo);
   assert(result == VK_SUCCESS);

   /* "If the vkAllocateMemory command succeeds, the implementation must
    * acquire a reference to the imported hardware buffer, which it must
    * release when the device memory object is freed. If the command fails,
    * the implementation must not retain a reference."
    */
   AHardwareBuffer_acquire(info->buffer);
   mem->ahw = info->buffer;

   return VK_SUCCESS;
#else
   return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}

VkResult
anv_create_ahw_memory(VkDevice device_h,
                      struct anv_device_memory *mem,
                      const VkMemoryAllocateInfo *pAllocateInfo)
{
#if ANDROID_API_LEVEL >= 26
   ANV_FROM_HANDLE(anv_device, dev, device_h);

   const VkMemoryDedicatedAllocateInfo *dedicated_info =
      vk_find_struct_const(pAllocateInfo->pNext,
                           MEMORY_DEDICATED_ALLOCATE_INFO);

   uint32_t w = 0;
   uint32_t h = 1;
   uint32_t layers = 1;
   uint32_t format = 0;
   uint64_t usage = 0;

   /* If caller passed dedicated information. */
   if (dedicated_info && dedicated_info->image) {
      ANV_FROM_HANDLE(anv_image, image, dedicated_info->image);
      w = image->extent.width;
      h = image->extent.height;
      layers = image->array_size;
      format = android_format_from_vk(image->vk_format);
      usage = anv_ahw_usage_from_vk_usage(image->create_flags, image->usage);
   } else if (dedicated_info && dedicated_info->buffer) {
      ANV_FROM_HANDLE(anv_buffer, buffer, dedicated_info->buffer);
      w = buffer->size;
      format = AHARDWAREBUFFER_FORMAT_BLOB;
      usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
   } else {
      w = pAllocateInfo->allocationSize;
      format = AHARDWAREBUFFER_FORMAT_BLOB;
      usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
   }

   struct AHardwareBuffer *ahw = NULL;
   struct AHardwareBuffer_Desc desc = {
      .width = w,
      .height = h,
      .layers = layers,
      .format = format,
      .usage = usage,
    };

   if (AHardwareBuffer_allocate(&desc, &ahw) != 0)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   mem->ahw = ahw;
   return VK_SUCCESS;
#else
   return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif

}

VkResult
anv_image_from_external(
   VkDevice device_h,
   const VkImageCreateInfo *base_info,
   const VkExternalMemoryImageCreateInfo *create_info,
   const VkAllocationCallbacks *alloc,
   VkImage *out_image_h)
{
#if ANDROID_API_LEVEL >= 26
   ANV_FROM_HANDLE(anv_device, device, device_h);

   const VkExternalFormatANDROID *ext_info =
      vk_find_struct_const(base_info->pNext, EXTERNAL_FORMAT_ANDROID);

   if (ext_info && ext_info->externalFormat != 0) {
      assert(base_info->format == VK_FORMAT_UNDEFINED);
      assert(base_info->imageType == VK_IMAGE_TYPE_2D);
      assert(base_info->usage == VK_IMAGE_USAGE_SAMPLED_BIT);
      assert(base_info->tiling == VK_IMAGE_TILING_OPTIMAL);
   }

   struct anv_image_create_info anv_info = {
      .vk_info = base_info,
      .isl_extra_usage_flags = ISL_SURF_USAGE_DISABLE_AUX_BIT,
      .external_format = true,
   };

   VkImage image_h;
   VkResult result = anv_image_create(device_h, &anv_info, alloc, &image_h);
   if (result != VK_SUCCESS)
      return result;

   *out_image_h = image_h;

   return VK_SUCCESS;
#else
   return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}


VkResult
anv_image_from_gralloc(VkDevice device_h,
                       const VkImageCreateInfo *base_info,
                       const VkNativeBufferANDROID *gralloc_info,
                       const VkAllocationCallbacks *alloc,
                       VkImage *out_image_h)

{
   ANV_FROM_HANDLE(anv_device, device, device_h);
   VkImage image_h = VK_NULL_HANDLE;
   struct anv_image *image = NULL;
   struct anv_bo *bo = NULL;
   VkResult result;

   struct anv_image_create_info anv_info = {
      .vk_info = base_info,
      .isl_extra_usage_flags = ISL_SURF_USAGE_DISABLE_AUX_BIT,
   };

   if (gralloc_info->handle->numFds != 1) {
      return vk_errorf(device, device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "VkNativeBufferANDROID::handle::numFds is %d, "
                       "expected 1", gralloc_info->handle->numFds);
   }

   /* Do not close the gralloc handle's dma_buf. The lifetime of the dma_buf
    * must exceed that of the gralloc handle, and we do not own the gralloc
    * handle.
    */
   int dma_buf = gralloc_info->handle->data[0];

   /* We need to set the WRITE flag on window system buffers so that GEM will
    * know we're writing to them and synchronize uses on other rings (for
    * example, if the display server uses the blitter ring).
    *
    * If this function fails and if the imported bo was resident in the cache,
    * we should avoid updating the bo's flags. Therefore, we defer updating
    * the flags until success is certain.
    *
    */
   result = anv_device_import_bo(device, dma_buf,
                                 ANV_BO_ALLOC_IMPLICIT_SYNC |
                                 ANV_BO_ALLOC_IMPLICIT_WRITE,
                                 0 /* client_address */,
                                 &bo);
   if (result != VK_SUCCESS) {
      return vk_errorf(device, device, result,
                       "failed to import dma-buf from VkNativeBufferANDROID");
   }

   int i915_tiling = anv_gem_get_tiling(device, bo->gem_handle);
   switch (i915_tiling) {
   case I915_TILING_NONE:
      anv_info.isl_tiling_flags = ISL_TILING_LINEAR_BIT;
      break;
   case I915_TILING_X:
      anv_info.isl_tiling_flags = ISL_TILING_X_BIT;
      break;
   case I915_TILING_Y:
      anv_info.isl_tiling_flags = ISL_TILING_Y0_BIT;
      break;
   case -1:
      result = vk_errorf(device, device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "DRM_IOCTL_I915_GEM_GET_TILING failed for "
                         "VkNativeBufferANDROID");
      goto fail_tiling;
   default:
      result = vk_errorf(device, device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "DRM_IOCTL_I915_GEM_GET_TILING returned unknown "
                         "tiling %d for VkNativeBufferANDROID", i915_tiling);
      goto fail_tiling;
   }

   enum isl_format format = anv_get_isl_format(&device->info,
                                               base_info->format,
                                               VK_IMAGE_ASPECT_COLOR_BIT,
                                               base_info->tiling);
   assert(format != ISL_FORMAT_UNSUPPORTED);

   anv_info.stride = gralloc_info->stride *
                     (isl_format_get_layout(format)->bpb / 8);

   result = anv_image_create(device_h, &anv_info, alloc, &image_h);
   image = anv_image_from_handle(image_h);
   if (result != VK_SUCCESS)
      goto fail_create;

   if (bo->size < image->size) {
      result = vk_errorf(device, device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "dma-buf from VkNativeBufferANDROID is too small for "
                         "VkImage: %"PRIu64"B < %"PRIu64"B",
                         bo->size, image->size);
      goto fail_size;
   }

   assert(image->n_planes == 1);
   assert(image->planes[0].address.offset == 0);

   image->planes[0].address.bo = bo;
   image->planes[0].bo_is_owned = true;

   /* Don't clobber the out-parameter until success is certain. */
   *out_image_h = image_h;

   return VK_SUCCESS;

 fail_size:
   anv_DestroyImage(device_h, image_h, alloc);
 fail_create:
 fail_tiling:
   anv_device_release_bo(device, bo);

   return result;
}

static VkResult
format_supported_with_usage(VkDevice device_h, VkFormat format,
                            VkImageUsageFlags imageUsage)
{
   ANV_FROM_HANDLE(anv_device, device, device_h);
   VkPhysicalDevice phys_dev_h = anv_physical_device_to_handle(device->physical);
   VkResult result;

   const VkPhysicalDeviceImageFormatInfo2 image_format_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = imageUsage,
   };

   VkImageFormatProperties2 image_format_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };

   /* Check that requested format and usage are supported. */
   result = anv_GetPhysicalDeviceImageFormatProperties2(phys_dev_h,
               &image_format_info, &image_format_props);
   if (result != VK_SUCCESS) {
      return vk_errorf(device, device, result,
                       "anv_GetPhysicalDeviceImageFormatProperties2 failed "
                       "inside %s", __func__);
   }
   return VK_SUCCESS;
}


static VkResult
setup_gralloc0_usage(struct anv_device *device, VkFormat format,
                     VkImageUsageFlags imageUsage, int *grallocUsage)
{
   /* WARNING: Android's libvulkan.so hardcodes the VkImageUsageFlags
    * returned to applications via VkSurfaceCapabilitiesKHR::supportedUsageFlags.
    * The relevant code in libvulkan/swapchain.cpp contains this fun comment:
    *
    *     TODO(jessehall): I think these are right, but haven't thought hard
    *     about it. Do we need to query the driver for support of any of
    *     these?
    *
    * Any disagreement between this function and the hardcoded
    * VkSurfaceCapabilitiesKHR:supportedUsageFlags causes tests
    * dEQP-VK.wsi.android.swapchain.*.image_usage to fail.
    */

   if (unmask32(&imageUsage, VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      *grallocUsage |= GRALLOC_USAGE_HW_RENDER;

   if (unmask32(&imageUsage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT |
                             VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      *grallocUsage |= GRALLOC_USAGE_HW_TEXTURE;

   /* All VkImageUsageFlags not explicitly checked here are unsupported for
    * gralloc swapchains.
    */
   if (imageUsage != 0) {
      return vk_errorf(device, device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "unsupported VkImageUsageFlags(0x%x) for gralloc "
                       "swapchain", imageUsage);
   }

   /* The below formats support GRALLOC_USAGE_HW_FB (that is, display
    * scanout). This short list of formats is univserally supported on Intel
    * but is incomplete.  The full set of supported formats is dependent on
    * kernel and hardware.
    *
    * FINISHME: Advertise all display-supported formats.
    */
   switch (format) {
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B5G6R5_UNORM_PACK16:
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
         *grallocUsage |= GRALLOC_USAGE_HW_FB |
                          GRALLOC_USAGE_HW_COMPOSER |
                          GRALLOC_USAGE_EXTERNAL_DISP;
         break;
      default:
         intel_logw("%s: unsupported format=%d", __func__, format);
   }

   if (*grallocUsage == 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   return VK_SUCCESS;
}

#if ANDROID_API_LEVEL >= 26
VkResult anv_GetSwapchainGrallocUsage2ANDROID(
    VkDevice            device_h,
    VkFormat            format,
    VkImageUsageFlags   imageUsage,
    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
    uint64_t*           grallocConsumerUsage,
    uint64_t*           grallocProducerUsage)
{
   ANV_FROM_HANDLE(anv_device, device, device_h);
   VkResult result;

   *grallocConsumerUsage = 0;
   *grallocProducerUsage = 0;
   intel_logd("%s: format=%d, usage=0x%x", __func__, format, imageUsage);

   result = format_supported_with_usage(device_h, format, imageUsage);
   if (result != VK_SUCCESS)
      return result;

   int32_t grallocUsage = 0;
   result = setup_gralloc0_usage(device, format, imageUsage, &grallocUsage);
   if (result != VK_SUCCESS)
      return result;

   /* Setup gralloc1 usage flags from gralloc0 flags. */

   if (grallocUsage & GRALLOC_USAGE_HW_RENDER) {
      *grallocProducerUsage |= GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET;
      *grallocConsumerUsage |= GRALLOC1_CONSUMER_USAGE_CLIENT_TARGET;
   }

   if (grallocUsage & GRALLOC_USAGE_HW_TEXTURE) {
      *grallocConsumerUsage |= GRALLOC1_CONSUMER_USAGE_GPU_TEXTURE;
   }

   if (grallocUsage & (GRALLOC_USAGE_HW_FB |
                       GRALLOC_USAGE_HW_COMPOSER |
                       GRALLOC_USAGE_EXTERNAL_DISP)) {
      *grallocProducerUsage |= GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET;
      *grallocConsumerUsage |= GRALLOC1_CONSUMER_USAGE_HWCOMPOSER;
   }

   return VK_SUCCESS;
}
#endif

VkResult anv_GetSwapchainGrallocUsageANDROID(
    VkDevice            device_h,
    VkFormat            format,
    VkImageUsageFlags   imageUsage,
    int*                grallocUsage)
{
   ANV_FROM_HANDLE(anv_device, device, device_h);
   VkResult result;

   *grallocUsage = 0;
   intel_logd("%s: format=%d, usage=0x%x", __func__, format, imageUsage);

   result = format_supported_with_usage(device_h, format, imageUsage);
   if (result != VK_SUCCESS)
      return result;

   return setup_gralloc0_usage(device, format, imageUsage, grallocUsage);
}

VkResult
anv_AcquireImageANDROID(
      VkDevice            device_h,
      VkImage             image_h,
      int                 nativeFenceFd,
      VkSemaphore         semaphore_h,
      VkFence             fence_h)
{
   ANV_FROM_HANDLE(anv_device, device, device_h);
   VkResult result = VK_SUCCESS;

   if (nativeFenceFd != -1) {
      /* As a simple, firstpass implementation of VK_ANDROID_native_buffer, we
       * block on the nativeFenceFd. This may introduce latency and is
       * definitiely inefficient, yet it's correct.
       *
       * FINISHME(chadv): Import the nativeFenceFd into the VkSemaphore and
       * VkFence.
       */
      if (sync_wait(nativeFenceFd, /*timeout*/ -1) < 0) {
         result = vk_errorf(device, device, VK_ERROR_DEVICE_LOST,
                            "%s: failed to wait on nativeFenceFd=%d",
                            __func__, nativeFenceFd);
      }

      /* From VK_ANDROID_native_buffer's pseudo spec
       * (https://source.android.com/devices/graphics/implement-vulkan):
       *
       *    The driver takes ownership of the fence fd and is responsible for
       *    closing it [...] even if vkAcquireImageANDROID fails and returns
       *    an error.
       */
      close(nativeFenceFd);

      if (result != VK_SUCCESS)
         return result;
   }

   if (semaphore_h || fence_h) {
      /* Thanks to implicit sync, the image is ready for GPU access.  But we
       * must still put the semaphore into the "submit" state; otherwise the
       * client may get unexpected behavior if the client later uses it as
       * a wait semaphore.
       *
       * Because we blocked above on the nativeFenceFd, the image is also
       * ready for foreign-device access (including CPU access). But we must
       * still signal the fence; otherwise the client may get unexpected
       * behavior if the client later waits on it.
       *
       * For some values of anv_semaphore_type, we must submit the semaphore
       * to execbuf in order to signal it.  Likewise for anv_fence_type.
       * Instead of open-coding here the signal operation for each
       * anv_semaphore_type and anv_fence_type, we piggy-back on
       * vkQueueSubmit.
       */
      const VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 0,
         .commandBufferCount = 0,
         .signalSemaphoreCount = (semaphore_h ? 1 : 0),
         .pSignalSemaphores = &semaphore_h,
      };

      result = anv_QueueSubmit(anv_queue_to_handle(&device->queue), 1,
                               &submit, fence_h);
      if (result != VK_SUCCESS) {
         return vk_errorf(device, device, result,
                          "anv_QueueSubmit failed inside %s", __func__);
      }
   }

   return VK_SUCCESS;
}

VkResult
anv_QueueSignalReleaseImageANDROID(
      VkQueue             queue,
      uint32_t            waitSemaphoreCount,
      const VkSemaphore*  pWaitSemaphores,
      VkImage             image,
      int*                pNativeFenceFd)
{
   VkResult result;

   if (waitSemaphoreCount == 0)
      goto done;

   result = anv_QueueSubmit(queue, 1,
      &(VkSubmitInfo) {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = pWaitSemaphores,
      },
      (VkFence) VK_NULL_HANDLE);
   if (result != VK_SUCCESS)
      return result;

 done:
   if (pNativeFenceFd) {
      /* We can rely implicit on sync because above we submitted all
       * semaphores to the queue.
       */
      *pNativeFenceFd = -1;
   }

   return VK_SUCCESS;
}
