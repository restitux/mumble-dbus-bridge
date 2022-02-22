#include "MumblePlugin_v_1_0_x.h"
#include "PluginComponents_v_1_0_x.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <systemd/sd-bus-vtable.h>
#include <systemd/sd-bus.h>

// Plugin state
struct MumbleAPI_v_1_0_x mumbleAPI;
mumble_plugin_id_t ownID;

#define LOG_BUF_SIZE 256

// Wrapper function to use the plugin logging API like printf
mumble_error_t mumble_log(const char *format, ...) {
    char *buf = malloc(LOG_BUF_SIZE);
    va_list args;
    va_start(args, format);
    vsnprintf(buf, LOG_BUF_SIZE - 1, format, args);
    buf[LOG_BUF_SIZE - 1] = '\n';
    va_end(args);

    int ret = mumbleAPI.log(ownID, buf);
    free(buf);
    return ret;
}

static int method_SetMute(sd_bus_message *m, void *userdata,
                          sd_bus_error *ret_error) {
    int r;
    bool mute_status;

    r = sd_bus_message_read(m, "b", &mute_status);
    if (r < 0) {
        mumble_log("Failed to parse parameters: %s", strerror(-r));
    }

    mumble_log("Setting mute status to %s", mute_status ? "true" : "false");
    mumbleAPI.requestLocalUserMute(ownID, mute_status);

    return sd_bus_reply_method_return(m, "");
}

static int method_ToggleMute(sd_bus_message *m, void *userdata,
                             sd_bus_error *ret_error) {
    // Get current mute status

    bool mute_status;
    mumbleAPI.isLocalUserMuted(ownID, &mute_status);

    mumble_log("Setting mute status to %s", !mute_status ? "true" : "false");
    mumbleAPI.requestLocalUserMute(ownID, !mute_status);

    return sd_bus_reply_method_return(m, "");
}

// DBus vtable for defining exposed methods
static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SetMute", "(b)", "", method_SetMute,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ToggleMute", "", "", method_ToggleMute,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

/*
** This thread handles monitoring the dbus connection and calling into
*Mumble
** It also checks for the termination signal sent from Mumble when the
*plugin is
** unloaded.
*/
void *dbus_thread(sd_bus *bus) {
    for (;;) {
        int r;
        /* Process requests */
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            mumble_log("Failed to process bus: %s", strerror(-r));
            return NULL;
        }
        if (r > 0) /* we processed a request, try to process another one,
                      right-away */
            continue;

        // TODO: swap out for proper event loop
        /* Wait for the next request to process */
        r = sd_bus_wait(bus, (uint64_t)-1);
        if (r < 0) {
            mumble_log("Failed to wait on bus: %s\n", strerror(-r));
            return NULL;
        }
    }
}

mumble_error_t mumble_init(mumble_plugin_id_t pluginID) {
    ownID = pluginID;

    mumble_log("Staring Mumble DBus Plugin");

    // Setup sdbus
    int r;

    sd_bus_slot *slot = NULL;
    sd_bus *bus = NULL;

    // Connect to session bus
    r = sd_bus_open_user(&bus);
    if (r < 0) {
        mumble_log("Failed to connect to session bus: %s", strerror(-r));
        return MUMBLE_EC_GENERIC_ERROR;
    }

    // Install the vtable object
    r = sd_bus_add_object_vtable(bus, &slot,
                                 "/xyz/ohea/mumble_dbus", /* object path */
                                 "xyz.ohea.mumble_dbus", vtable, NULL);
    if (r < 0) {
        mumble_log("Failed to issue method call: %s\n", strerror(-r));

        return MUMBLE_EC_GENERIC_ERROR;
    }

    // Request our name
    r = sd_bus_request_name(bus, "xyz.ohea.mumble_dbus",
                            SD_BUS_NAME_ALLOW_REPLACEMENT |
                                SD_BUS_NAME_REPLACE_EXISTING);
    if (r < 0) {
        mumble_log("Failed to acquire service name: %s\n", strerror(-r));
        return MUMBLE_EC_GENERIC_ERROR;
    }

    // Launch DBus main loop in another thread
    pthread_t dbus_thread_ID;
    pthread_create(&dbus_thread_ID, NULL, dbus_thread, bus);

    return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
    // TODO: add signaling to stop dbus main thread
    if (mumbleAPI.log(ownID, "Goodbye Mumble") != MUMBLE_STATUS_OK) {
        // Logging failed -> usually you'd probably want to log things like
        // this in your plugin's logging system (if there is any)
    }
}

struct MumbleStringWrapper mumble_getName() {
    static const char *name = "Mumble DBus Adapter";

    struct MumbleStringWrapper wrapper;
    wrapper.data = name;
    wrapper.size = strlen(name);
    wrapper.needsReleasing = false;

    return wrapper;
}

mumble_version_t mumble_getAPIVersion() {
    // This constant will always hold the API version  that fits the
    // included header files
    return MUMBLE_PLUGIN_API_VERSION;
}

void mumble_registerAPIFunctions(void *apiStruct) {
    // Provided mumble_getAPIVersion returns MUMBLE_PLUGIN_API_VERSION, this
    // cast will make sure that the passed pointer will be cast to the
    // proper type
    mumbleAPI = MUMBLE_API_CAST(apiStruct);
}

void mumble_releaseResource(const void *pointer) {
    // As we never pass a resource to Mumble that needs releasing, this
    // function should never get called
    printf("Called mumble_releaseResource but expected that this never gets "
           "called -> Aborting");
    abort();
}

// Below functions are not strictly necessary but every halfway serious
// plugin should implement them nonetheless

mumble_version_t mumble_getVersion() {
    mumble_version_t version;
    version.major = 1;
    version.minor = 0;
    version.patch = 0;

    return version;
}

struct MumbleStringWrapper mumble_getAuthor() {
    static const char *author = "restitux <restitux@ohea.xyz>";

    struct MumbleStringWrapper wrapper;
    wrapper.data = author;
    wrapper.size = strlen(author);
    wrapper.needsReleasing = false;

    return wrapper;
}

struct MumbleStringWrapper mumble_getDescription() {
    static const char *description =
        "A plugin to allow manipulating Mumble via DBus";

    struct MumbleStringWrapper wrapper;
    wrapper.data = description;
    wrapper.size = strlen(description);
    wrapper.needsReleasing = false;

    return wrapper;
}
