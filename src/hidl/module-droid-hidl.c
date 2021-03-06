/*
 * Copyright (C) 2019 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@jolla.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulse/mainloop-api.h>

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/start-child.h>

#include <droid/droid-util.h>

#include "common.h"
#include "module-droid-hidl-symdef.h"

PA_MODULE_AUTHOR("Juho Hämäläinen");
PA_MODULE_DESCRIPTION("Droid HIDL passthrough");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
        "module_id=<which droid hw module to load, default primary> "
        "helper=<spawn helper binary, default true>"
);

static const char* const valid_modargs[] = {
    "module_id",
    "helper",
    NULL,
};

#define DEFAULT_MODULE_ID   "primary"

#define HELPER_BINARY       HIDL_HELPER_LOCATION "/" HELPER_NAME
#define BUFFER_MAX          (512)

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_dbus_protocol* dbus_protocol;
    pa_droid_hw_module *hw_module;

    /* Helper */
    pid_t pid;
    int fd;
    pa_io_event *io_event;
};

static pa_log_level_t _log_level = PA_LOG_ERROR;

static bool log_level_debug(void) {
    if (PA_UNLIKELY(_log_level == PA_LOG_DEBUG))
        return true;
    return false;
}

static void hidl_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void hidl_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum hidl_passthrough_methods {
    HIDL_PASSTHROUGH_GET_PARAMETERS,
    HIDL_PASSTHROUGH_SET_PARAMETERS,
    HIDL_PASSTHROUGH_METHOD_MAX
};

static pa_dbus_arg_info get_parameters_args[] = {
    { "keys", "s", "in" }
};

static pa_dbus_arg_info set_parameters_args[] = {
    { "key_value_pairs", "s", "in" }
};

static pa_dbus_method_handler hidl_passthrough_method_handlers[HIDL_PASSTHROUGH_METHOD_MAX] = {
    [HIDL_PASSTHROUGH_GET_PARAMETERS] = {
        .method_name = HIDL_PASSTHROUGH_METHOD_GET_PARAMETERS,
        .arguments = get_parameters_args,
        .n_arguments = sizeof(get_parameters_args) / sizeof(get_parameters_args[0]),
        .receive_cb = hidl_get_parameters
    },
    [HIDL_PASSTHROUGH_SET_PARAMETERS] = {
        .method_name = HIDL_PASSTHROUGH_METHOD_SET_PARAMETERS,
        .arguments = set_parameters_args,
        .n_arguments = sizeof(set_parameters_args) / sizeof(set_parameters_args[0]),
        .receive_cb = hidl_set_parameters
    },
};

static pa_dbus_interface_info hidl_passthrough_info = {
    .name = HIDL_PASSTHROUGH_IFACE,
    .method_handlers = hidl_passthrough_method_handlers,
    .n_method_handlers = HIDL_PASSTHROUGH_METHOD_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

static void dbus_init(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->core);

    u->dbus_protocol = pa_dbus_protocol_get(u->core);

    pa_dbus_protocol_add_interface(u->dbus_protocol, HIDL_PASSTHROUGH_PATH, &hidl_passthrough_info, u);
    pa_dbus_protocol_register_extension(u->dbus_protocol, HIDL_PASSTHROUGH_IFACE);
}

static void dbus_done(struct userdata *u) {
    pa_assert(u);

    pa_dbus_protocol_unregister_extension(u->dbus_protocol, HIDL_PASSTHROUGH_IFACE);
    pa_dbus_protocol_remove_interface(u->dbus_protocol, HIDL_PASSTHROUGH_PATH, hidl_passthrough_info.name);
    pa_dbus_protocol_unref(u->dbus_protocol);
    u->dbus_protocol = NULL;
}

static void hidl_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct userdata *u;
    DBusMessage *reply;
    DBusError error;
    char *keys = NULL;
    char *key_value_pairs = NULL;

    pa_assert_se((u = userdata));
    dbus_error_init(&error);

    if (dbus_message_get_args(msg,
                              &error,
                              DBUS_TYPE_STRING,
                              &keys,
                              DBUS_TYPE_INVALID)) {

        pa_droid_hw_module_lock(u->hw_module);
        key_value_pairs = u->hw_module->device->get_parameters(u->hw_module->device, keys);
        pa_droid_hw_module_unlock(u->hw_module);

        pa_log_debug("get_parameters(\"%s\"): \"%s\"", keys, key_value_pairs ? key_value_pairs : "<null>");

        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply,
                                 DBUS_TYPE_STRING,
                                 &key_value_pairs,
                                 DBUS_TYPE_INVALID);

        pa_assert_se(dbus_connection_send(conn, reply, NULL));
        dbus_message_unref(reply);
        return;
    }

    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Fail: %s", error.message);
    dbus_error_free(&error);
}

static void hidl_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct userdata *u;
    DBusError error;
    char *key_value_pairs = NULL;
    int ret;

    pa_assert_se((u = userdata));
    dbus_error_init(&error);

    if (dbus_message_get_args(msg,
                              &error,
                              DBUS_TYPE_STRING,
                              &key_value_pairs,
                              DBUS_TYPE_INVALID)) {

        pa_log_debug("set_parameters(\"%s\")", key_value_pairs);

        pa_droid_hw_module_lock(u->hw_module);
        ret = u->hw_module->device->set_parameters(u->hw_module->device, key_value_pairs);
        pa_droid_hw_module_unlock(u->hw_module);

        if (ret != 0) {
            pa_log_warn("set_parameters(\"%s\") failed: %d", key_value_pairs, ret);
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Failed to set parameters.");
        } else {
            pa_dbus_send_empty_reply(conn, msg);
        }
        return;
    }

    pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "Fail: %s", error.message);
    dbus_error_free(&error);
}

static void io_free(struct userdata *u) {
    if (u->io_event) {
        u->core->mainloop->io_free(u->io_event);
        u->io_event = NULL;
    }

    if (u->fd >= 0) {
        pa_close(u->fd);
        u->fd = -1;
    }
}

static void io_event_cb(pa_mainloop_api*a, pa_io_event* e, int fd, pa_io_event_flags_t events, void *userdata) {
    struct userdata *u = userdata;
    char buffer[BUFFER_MAX];
    ssize_t r;

    pa_assert(u);

    if (events & PA_IO_EVENT_INPUT) {
        memset(buffer, 0, BUFFER_MAX);
        if ((r = pa_read(u->fd, buffer, BUFFER_MAX, NULL)) > 0) {
            if (log_level_debug())
                pa_log_debug("[" HELPER_NAME "] %s", buffer);
            else
                pa_log("[" HELPER_NAME "] %s", buffer);
        } else if (r < 0) {
            pa_log("failed read");
            io_free(u);
        }
    } else if (events & PA_IO_EVENT_HANGUP) {
        pa_log_debug("helper disappeared");
        io_free(u);
    } else if (events & PA_IO_EVENT_ERROR) {
        pa_log("io error");
        io_free(u);
    }
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    const char *module_id;
    bool helper = true;
    char *dbus_address = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    log_init(&_log_level);

    struct userdata *u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    m->userdata = u;
    u->pid = (pid_t) -1;
    u->fd = -1;
    u->io_event = NULL;

    module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);
    if (pa_modargs_get_value_boolean(ma, "helper", &helper) < 0) {
        pa_log("helper is boolean argument");
        goto fail;
    }

    if (!(u->hw_module = pa_droid_hw_module_get(u->core, NULL, module_id))) {
        pa_log("Couldn't get hw module %s, is module-droid-card loaded?", module_id);
        goto fail;
    }

    dbus_init(u);

    dbus_address = pa_get_dbus_address_from_server_type(u->core->server_type);

    if (helper) {
        if ((u->fd = pa_start_child_for_read(HELPER_BINARY,
                                             dbus_address, &u->pid)) < 0) {
            pa_log("Failed to spawn " HELPER_NAME);
            goto fail;
        }
        pa_xfree(dbus_address);

        pa_log_info("Helper running with pid %d", u->pid);

        u->io_event = u->core->mainloop->io_new(u->core->mainloop,
                                                u->fd,
                                                PA_IO_EVENT_INPUT | PA_IO_EVENT_ERROR | PA_IO_EVENT_HANGUP,
                                                io_event_cb,
                                                u);
    }

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(dbus_address);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if ((u = m->userdata)) {
        dbus_done(u);

        if (u->hw_module)
            pa_droid_hw_module_unref(u->hw_module);

        if (u->pid != (pid_t) -1) {
            kill(u->pid, SIGTERM);

            for (;;) {
                if (waitpid(u->pid, NULL, 0) >= 0)
                    break;

                if (errno != EINTR) {
                    pa_log("waitpid() failed: %s", pa_cstrerror(errno));
                    break;
                }
            }
        }

        io_free(u);

        pa_xfree(u);
    }
}
