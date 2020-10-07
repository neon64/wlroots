#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/egl.h>
#include <wlr/types/wlr_list.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "backend/drm/drm.h"
#include "util/signal.h"
#include <backtrace.h>
#include <include/wlr/backend/multi.h>

struct wlr_drm_backend *get_drm_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_drm(wlr_backend));
	return (struct wlr_drm_backend *)wlr_backend;
}

static bool backend_start(struct wlr_backend *backend) {
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(backend);
	scan_drm_connectors(drm);
	return true;
}

static int bt_callback(void *data, uintptr_t pc,
		const char *filename, int lineno,
		const char *function) {
	wlr_log(WLR_DEBUG, "Backtrace: %s in %s:%d", function, filename, lineno);
	return 0;
}

static void bt_error(void *data, const char *msg,
		int errnum) {
	wlr_log(WLR_ERROR, "Backtrace error%s", msg);
}

static void print_trace(void) {
	wlr_log(WLR_DEBUG, "Trying to obtain a backtrace");

	struct backtrace_state *s = backtrace_create_state(NULL, 0,bt_error, NULL);

	wlr_log(WLR_DEBUG, "Tried to obtain a backtrace");

	backtrace_full(s, 0, bt_callback, bt_error, NULL);
}

static void backend_destroy(struct wlr_backend *backend) {
	if (!backend) {
		return;
	}

	wlr_log(WLR_INFO, "Destroying DRM BACKEND!!!");

	struct wlr_drm_backend *drm = get_drm_backend_from_backend(backend);

	restore_drm_outputs(drm);

	struct wlr_drm_connector *conn, *next;
	wl_list_for_each_safe(conn, next, &drm->outputs, link) {
		wlr_output_destroy(&conn->output);
	}

	wlr_signal_emit_safe(&backend->events.destroy, backend);

	wl_list_remove(&drm->display_destroy.link);
	wl_list_remove(&drm->session_destroy.link);
	wl_list_remove(&drm->session_signal.link);
	wl_list_remove(&drm->drm_invalidated.link);
	wl_list_remove(&drm->add_gpu_signal.link);

	finish_drm_resources(drm);
	finish_drm_renderer(&drm->renderer);
	wlr_session_close_file(drm->session, drm->fd);
	wl_event_source_remove(drm->drm_event);
	free(drm);
}

static struct wlr_renderer *backend_get_renderer(
		struct wlr_backend *backend) {
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(backend);

	if (drm->parent) {
		return drm->parent->renderer.wlr_rend;
	} else {
		return drm->renderer.wlr_rend;
	}
}

static clockid_t backend_get_presentation_clock(struct wlr_backend *backend) {
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(backend);
	return drm->clock;
}

static struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_renderer = backend_get_renderer,
	.get_presentation_clock = backend_get_presentation_clock,
};

bool wlr_backend_is_drm(struct wlr_backend *b) {
	return b->impl == &backend_impl;
}

static void handle_add_gpu(struct wl_listener* listener, void *data) {
	struct wlr_drm_backend *drm =
			wl_container_of(listener, drm, add_gpu_signal);
	struct wlr_event_add_gpu *event = data;

	wlr_log(WLR_INFO, "parent drm fd is %d", drm->fd);

	wlr_log(WLR_INFO, "got handle_gpu signal with fd = %d", event->gpu_fd);

	// TODO:
	struct wlr_backend *child_drm = wlr_drm_backend_create(drm->display, drm->session,
						event->gpu_fd, &drm->backend, NULL);

	if (!child_drm) {
		wlr_log(WLR_ERROR, "Failed to open DRM device %d", event->gpu_fd);
		return;
	} else {
		wlr_log(WLR_INFO, "Successfully opened DRM device %d", event->gpu_fd);
	}

	fprintf(stderr, "is multi? %d\n\n\n", wlr_backend_is_multi(&drm->backend));

	if(!wlr_multi_backend_add(&drm->multi->backend, child_drm)) {
		wlr_log(WLR_INFO, "Failed to add to multi backend");
	} else {
		wlr_log(WLR_INFO, "Added to multi backend");
	}
}

static void session_signal(struct wl_listener *listener, void *data) {
	struct wlr_drm_backend *drm =
		wl_container_of(listener, drm, session_signal);
	struct wlr_session *session = data;

	if (session->active) {
		wlr_log(WLR_INFO, "DRM fd resumed");
		scan_drm_connectors(drm);

		struct wlr_drm_connector *conn;
		wl_list_for_each(conn, &drm->outputs, link){
			if (conn->output.enabled && conn->output.current_mode != NULL) {
				drm_connector_set_mode(conn, conn->output.current_mode);
			} else {
				drm_connector_set_mode(conn, NULL);
			}
		}
	} else {
		wlr_log(WLR_INFO, "DRM fd paused");
	}
}

static void drm_invalidated(struct wl_listener *listener, void *data) {
	struct wlr_drm_backend *drm =
		wl_container_of(listener, drm, drm_invalidated);

	char *name = drmGetDeviceNameFromFd2(drm->fd);
	wlr_log(WLR_DEBUG, "%s invalidated", name);


	size_t seen_len = wl_list_length(&drm->outputs);
	wlr_log(WLR_DEBUG, "%ld outputs before scan", seen_len);

	scan_drm_connectors(drm);

	// if all the drm connectors (outputs?) are disconnected, then we try to
	// destroy the whole drm backend so we can unload drivers etc...
	struct wlr_drm_connector *c = NULL;
	bool all_disconnected = true;
	wl_list_for_each(c, &drm->outputs, link) {
		if(c->state != WLR_DRM_CONN_DISCONNECTED) {
			all_disconnected = false;
		}
		wlr_log(WLR_INFO, "drm connector state: %s", c->output.name);
		wlr_log(WLR_INFO, "drm connector state: %d", c->state);
		wlr_log(WLR_INFO, "drm connector output mode: %p", c->desired_mode);
		wlr_log(WLR_INFO, "drm connector output mode: %d", c->output.enabled);
	}

	seen_len = wl_list_length(&drm->outputs);
	wlr_log(WLR_DEBUG, "%ld outputs after scan", seen_len);

	// TODO: only destroy backend if we find that all drm_connectors have been lost ?
	// i.e.: have unplugged all monitors

	wlr_log(WLR_INFO, "Should we destroy the DRM backend? %s - %d", name, all_disconnected);
	free(name);

	if(drm->parent != NULL) {
		name = drmGetDeviceNameFromFd2(drm->parent->fd);
		wlr_log(WLR_INFO, "Parent drm backend: %p %s", drm->parent, name);
		free(name);
	}

	print_trace();

	if(all_disconnected) {
		wlr_log(WLR_INFO, "Destroying DRM backend anyway... hehe");
		// this is what we added - to destroy the backend when drm is invalidated...
		wlr_backend_destroy(&drm->backend);
	}
}

static void handle_session_destroy(struct wl_listener *listener, void *data) {
	struct wlr_drm_backend *drm =
		wl_container_of(listener, drm, session_destroy);
	backend_destroy(&drm->backend);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_drm_backend *drm =
		wl_container_of(listener, drm, display_destroy);
	wlr_log(WLR_INFO, "ENTERING handle_display_destroy");

	backend_destroy(&drm->backend);
}

struct wlr_backend *wlr_drm_backend_create(struct wl_display *display,
		struct wlr_session *session, int gpu_fd, struct wlr_backend *parent,
		wlr_renderer_create_func_t create_renderer_func) {
	assert(display && session && gpu_fd >= 0);
	assert(!parent || wlr_backend_is_drm(parent));

	char *name = drmGetDeviceNameFromFd2(gpu_fd);
	drmVersion *version = drmGetVersion(gpu_fd);
	wlr_log(WLR_INFO, "Initializing DRM backend for %s (%s)", name, version->name);
	free(name);
	drmFreeVersion(version);

	struct wlr_drm_backend *drm = calloc(1, sizeof(struct wlr_drm_backend));
	if (!drm) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	wlr_backend_init(&drm->backend, &backend_impl);

	drm->multi = NULL;
	drm->session = session;
	wl_list_init(&drm->outputs);

	drm->fd = gpu_fd;
	if (parent != NULL) {
		drm->parent = get_drm_backend_from_backend(parent);
	}

	drm->drm_invalidated.notify = drm_invalidated;
	wlr_session_signal_add(session, gpu_fd, &drm->drm_invalidated);

	drm->display = display;
	struct wl_event_loop *event_loop = wl_display_get_event_loop(display);

	drm->drm_event = wl_event_loop_add_fd(event_loop, drm->fd,
		WL_EVENT_READABLE, handle_drm_event, NULL);
	if (!drm->drm_event) {
		wlr_log(WLR_ERROR, "Failed to create DRM event source");
		goto error_fd;
	}

	drm->session_signal.notify = session_signal;
	wl_signal_add(&session->session_signal, &drm->session_signal);

	drm->add_gpu_signal.notify = handle_add_gpu;
	wl_signal_add(&session->events.add_gpu, &drm->add_gpu_signal);

	if (!check_drm_features(drm)) {
		goto error_event;
	}

	if (!init_drm_resources(drm)) {
		goto error_event;
	}

	if (!init_drm_renderer(drm, &drm->renderer, create_renderer_func)) {
		wlr_log(WLR_ERROR, "Failed to initialize renderer");
		goto error_event;
	}

	drm->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &drm->session_destroy);

	drm->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &drm->display_destroy);

	return &drm->backend;

error_event:
	wl_list_remove(&drm->session_signal.link);
	wl_event_source_remove(drm->drm_event);
error_fd:
	wlr_session_close_file(drm->session, drm->fd);
	free(drm);
	return NULL;
}
