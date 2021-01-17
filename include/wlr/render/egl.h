/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_EGL_H
#define WLR_RENDER_EGL_H

#ifndef MESA_EGL_NO_X11_HEADERS
#define MESA_EGL_NO_X11_HEADERS
#endif
#ifndef EGL_NO_X11
#define EGL_NO_X11
#endif
#ifndef EGL_NO_PLATFORM_SPECIFIC_TYPES
#define EGL_NO_PLATFORM_SPECIFIC_TYPES
#endif

#include <wlr/config.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pixman.h>
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_format_set.h>

struct wlr_egl_context {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw_surface;
	EGLSurface read_surface;
};

struct wlr_egl {
	EGLDisplay display;
	EGLConfig config; // may be EGL_NO_CONFIG
	EGLContext context;
	EGLDeviceEXT device; // may be EGL_NO_DEVICE_EXT
	struct gbm_device *gbm_device;

	struct {
		// Display extensions
		bool bind_wayland_display_wl;
		bool image_base_khr;
		bool image_dma_buf_export_mesa;
		bool image_dmabuf_import_ext;
		bool image_dmabuf_import_modifiers_ext;

		// Device extensions
		bool device_drm_ext;
	} exts;

	struct {
		PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
		PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT;
		PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
		PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
		PFNEGLQUERYWAYLANDBUFFERWL eglQueryWaylandBufferWL;
		PFNEGLBINDWAYLANDDISPLAYWL eglBindWaylandDisplayWL;
		PFNEGLUNBINDWAYLANDDISPLAYWL eglUnbindWaylandDisplayWL;
		PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT;
		PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
		PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;
		PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
		PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR;
		PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT;
		PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT;
		PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT;
	} procs;

	struct wl_display *wl_display;

	struct wlr_drm_format_set dmabuf_texture_formats;
	struct wlr_drm_format_set dmabuf_render_formats;
};

/**
 * Initializes an EGL context for the given platform and drm file descriptor.
 * Will attempt to load all possibly required api functions.
 *
 * If config_attribs is NULL, the EGL config is not created.
 */
struct wlr_egl *wlr_egl_create(EGLenum platform, int drm_fd,
	const EGLint *config_attribs);

/**
 * Frees all related EGL resources, makes the context not-current and
 * unbinds a bound wayland display.
 */
void wlr_egl_destroy(struct wlr_egl *egl);

/**
 * Binds the given display to the EGL instance.
 * This will allow clients to create EGL surfaces from wayland ones and render
 * to it.
 */
bool wlr_egl_bind_display(struct wlr_egl *egl, struct wl_display *local_display);

/**
 * Returns a surface for the given native window
 * The window must match the remote display the wlr_egl was created with.
 */
EGLSurface wlr_egl_create_surface(struct wlr_egl *egl, void *window);

/**
 * Creates an EGL image from the given wl_drm buffer resource.
 */
EGLImageKHR wlr_egl_create_image_from_wl_drm(struct wlr_egl *egl,
	struct wl_resource *data, EGLint *fmt, int *width, int *height,
	bool *inverted_y);

/**
 * Creates an EGL image from the given dmabuf attributes. Check usability
 * of the dmabuf with wlr_egl_check_import_dmabuf once first.
 */
EGLImageKHR wlr_egl_create_image_from_dmabuf(struct wlr_egl *egl,
	struct wlr_dmabuf_attributes *attributes, bool *external_only);

/**
 * Get DMA-BUF formats suitable for sampling usage.
 */
const struct wlr_drm_format_set *wlr_egl_get_dmabuf_texture_formats(
	struct wlr_egl *egl);
/**
 * Get DMA-BUF formats suitable for rendering usage.
 */
const struct wlr_drm_format_set *wlr_egl_get_dmabuf_render_formats(
	struct wlr_egl *egl);

bool wlr_egl_export_image_to_dmabuf(struct wlr_egl *egl, EGLImageKHR image,
	int32_t width, int32_t height, uint32_t flags,
	struct wlr_dmabuf_attributes *attribs);

/**
 * Destroys an EGL image created with the given wlr_egl.
 */
bool wlr_egl_destroy_image(struct wlr_egl *egl, EGLImageKHR image);

/**
 * Make the EGL context current.
 *
 * Callers are expected to clear the current context when they are done by
 * calling wlr_egl_unset_current.
 */
bool wlr_egl_make_current(struct wlr_egl *egl);

bool wlr_egl_unset_current(struct wlr_egl *egl);

bool wlr_egl_is_current(struct wlr_egl *egl);

/**
 * Save the current EGL context to the structure provided in the argument.
 *
 * This includes display, context, draw surface and read surface.
 */
void wlr_egl_save_context(struct wlr_egl_context *context);

/**
 * Restore EGL context that was previously saved using wlr_egl_save_current().
 */
bool wlr_egl_restore_context(struct wlr_egl_context *context);

bool wlr_egl_destroy_surface(struct wlr_egl *egl, EGLSurface surface);

int wlr_egl_dup_drm_fd(struct wlr_egl *egl);

#endif
