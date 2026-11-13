/*
 * ivi_stubs.c — link-time stand-ins for the firmware's IVI client libraries
 * (libwayland-ivi-client.so.0, libwayland-ivi-shell-client.so.1). These libraries are
 * proprietary and exist only on the car, so the Dockerfile builds empty libraries with
 * the same sonames and symbols just for the linker. On the car the real ones are loaded.
 *
 * Only the names matter: the executable reaches these objects through its GOT, and the
 * dynamic linker binds them to the real library at startup.
 */
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;
    int event_count;
    const void *events;
};

#ifdef IVI_SHELL
struct wl_interface wl_ivi_shell_interface;
struct wl_interface wl_ivi_animation_group_interface;
#else
struct wl_interface wl_ivi_interface;
#endif
