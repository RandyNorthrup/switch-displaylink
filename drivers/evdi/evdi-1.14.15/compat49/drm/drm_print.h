#ifndef _EVDI_COMPAT_DRM_PRINT_H
#define _EVDI_COMPAT_DRM_PRINT_H
/* drm/drm_print.h did not exist in Linux 4.9; DRM_DEBUG/DRM_ERROR live in drmP.h */
#include <drm/drmP.h>
#endif
