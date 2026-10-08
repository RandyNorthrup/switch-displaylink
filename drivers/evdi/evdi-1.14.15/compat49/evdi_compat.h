#ifndef EVDI_COMPAT49_H
#define EVDI_COMPAT49_H
#include <linux/version.h>
#include <linux/dma-buf.h>
#include <drm/drmP.h>

/* ref/unref -> get/put renames (get/put introduced ~4.12/4.15) */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#define drm_framebuffer_get   drm_framebuffer_reference
#define drm_framebuffer_put   drm_framebuffer_unreference
#define drm_gem_object_get    drm_gem_object_reference
#endif
#ifndef drm_gem_object_put_unlocked
#define drm_gem_object_put_unlocked drm_gem_object_unreference_unlocked
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
#define drm_dev_put drm_dev_unref
#endif

/* drm_dev_unplug absent in 4.9 -> unregister is the correct teardown */
#define drm_dev_unplug drm_dev_unregister

/* drm_atomic_helper_shutdown absent pre-4.15; evdi's own teardown covers it */
#define drm_atomic_helper_shutdown(dev) do { (void)(dev); } while (0)

/* drm_ioctl_kernel added in 4.13 (only used by 32-bit compat path) */
static inline int drm_ioctl_kernel(struct file *file, drm_ioctl_t *func,
				   void *kdata, u32 flags)
{
	struct drm_file *file_priv = file->private_data;
	struct drm_device *dev = file_priv->minor->dev;
	(void)flags;
	return func(dev, kdata, file_priv);
}

/* drm_gem_framebuffer_helper shims (header absent pre-4.14) */
struct drm_plane;
struct drm_plane_state;
struct drm_framebuffer;
static inline int drm_gem_fb_prepare_fb(struct drm_plane *plane,
					struct drm_plane_state *state)
{ (void)plane; (void)state; return 0; }
static inline int drm_gem_fb_begin_cpu_access(struct drm_framebuffer *fb,
					      enum dma_data_direction dir)
{ (void)fb; (void)dir; return 0; }
static inline void drm_gem_fb_end_cpu_access(struct drm_framebuffer *fb,
					     enum dma_data_direction dir)
{ (void)fb; (void)dir; }

/* --- wave 2 --- */
#include <linux/vmalloc.h>
#include <linux/slab.h>

#ifndef DRM_MODESET_ACQUIRE_INTERRUPTIBLE
#define DRM_MODESET_ACQUIRE_INTERRUPTIBLE 0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
#define drm_atomic_state_put drm_atomic_state_free
#endif

/* drm_gem_object_put (unlocked kref) absent pre-4.12 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#define drm_gem_object_put drm_gem_object_unreference_unlocked
#endif

/* kvmalloc_array absent pre-4.18 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	void *p = kmalloc_array(n, size, flags | __GFP_NOWARN | __GFP_NORETRY);

	if (!p)
		p = vmalloc(n * size);
	return p;
}
#endif

#endif
