#define _XOPEN_SOURCE 500
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dbus/dbus.h>
#include "swaybar/tray/dbus.h"
#include "list.h"
#include "log.h"

static list_t *items = NULL;
static list_t *hosts = NULL;
static list_t *object_path_items = NULL;

/**
 * Describes the function of the StatusNotifierWatcher
 * See https://freedesktop.org/wiki/Specifications/StatusNotifierItem/StatusNotifierWatcher/
 *
 * We also implement KDE's special snowflake protocol, it's like this but with
 * all occurrences 'freedesktop' replaced with 'kde'. There is no KDE introspect.
 *
 * We _also_ support registering items by object path (even though this is a
 * huge pain in the ass). Hosts that would like to subscribe to these items have
 * to go through the `org.swaywm.LessSuckyStatusNotifierWatcher` interface.
 */
static const char *interface_xml =
	"<!DOCTYPE node PUBLIC '-//freedesktop//DTD D-BUS Object Introspection 1.0//EN'"
	"'http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd'>"
	"<node>"
	"  <interface name='org.freedesktop.DBus.Introspectable'>"
	"    <method name='Introspect'>"
	"       <arg name='xml_data' direction='out' type='s'/>"
	"    </method>"
	"  </interface>"
	"  <interface name='org.freedesktop.DBus.Properties'>"
	"    <method name='Get'>"
	"       <arg name='interface' direction='in' type='s'/>"
	"       <arg name='propname' direction='in' type='s'/>"
	"       <arg name='value' direction='out' type='v'/>"
	"    </method>"
	"    <method name='Set'>"
	"       <arg name='interface' direction='in' type='s'/>"
	"       <arg name='propname' direction='in' type='s'/>"
	"       <arg name='value' direction='in' type='v'/>"
	"    </method>"
	"    <method name='GetAll'>"
	"       <arg name='interface' direction='in' type='s'/>"
	"       <arg name='props' direction='out' type='a{sv}'/>"
	"    </method>"
	"  </interface>"
	"  <interface name='org.freedesktop.StatusNotifierWatcher'>"
	"    <method name='RegisterStatusNotifierItem'>"
	"      <arg type='s' name='service' direction='in'/>"
	"    </method>"
	"    <method name='RegisterStatusNotifierHost'>"
	"      <arg type='s' name='service' direction='in'/>"
	"    </method>"
	"    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
	"    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
	"    <property name='ProtocolVersion' type='i' access='read'/>"
	"    <signal name='StatusNotifierItemRegistered'>"
	"      <arg type='s' name='service' direction='out'/>"
	"    </signal>"
	"    <signal name='StatusNotifierItemUnregistered'>"
	"      <arg type='s' name='service' direction='out'/>"
	"    </signal>"
	"    <signal name='StatusNotifierHostRegistered'>"
	"      <arg type='' name='service' direction='out'/>"
	"    </signal>"
	"  </interface>"
	"  <interface name='org.swaywm.LessSuckyStatusNotifierWatcher'>"
	"    <property name='RegisteredObjectPathItems' type='a(os)' access='read'/>"
	"    <signal name='ObjPathItemRegistered'>"
	"      <arg type='os' name='service' direction='out'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

struct ObjPathItem {
	char *obj_path;
	char *unique_name;
};

static void free_obj_path_item(struct ObjPathItem *item) {
	if (!item) {
		return;
	}
	free(item->unique_name);
	free(item->obj_path);
	free(item);
}
static struct ObjPathItem *create_obj_path_item(const char *unique_name, const char *obj_path) {
	struct ObjPathItem *item = malloc(sizeof(struct ObjPathItem));
	if (!item) {
		return NULL;
	}
	item->unique_name = strdup(unique_name);
	item->obj_path = strdup(obj_path);
	if (!item->unique_name || !item->obj_path) {
		free_obj_path_item(item);
		return NULL;
	}
	return item;
}
/**
 * NOTE: This compare function does have ordering, this is because it has to
 * comapre two strings.
 */
static int obj_path_item_cmp(const void *_item1, const void *_item2) {
	const struct ObjPathItem *item1 = _item1;
	const struct ObjPathItem *item2 = _item2;
	if (strcmp(item1->unique_name,item2->unique_name) == 0 &&
			strcmp(item1->obj_path,item2->obj_path) == 0) {
		return 0;
	}
	return -1;
}
static int obj_path_unique_name_cmp(const void *_item, const void *_unique_name) {
	const struct ObjPathItem *item = _item;
	const char *unique_name = _unique_name;
	return strcmp(item->unique_name, unique_name);
}

static void host_registered_signal(DBusConnection *connection) {
	// Send one signal for each protocol
	DBusMessage *signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierHostRegistered");

	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);


	signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierHostRegistered");

	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);
}
static void item_registered_signal(DBusConnection *connection, const char *name) {
	DBusMessage *signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierItemRegistered");
	dbus_message_append_args(signal,
			DBUS_TYPE_STRING, &name,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);

	signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierItemRegistered");
	dbus_message_append_args(signal,
			DBUS_TYPE_STRING, &name,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);
}
static void item_unregistered_signal(DBusConnection *connection, const char *name) {
	DBusMessage *signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierItemUnregistered");
	dbus_message_append_args(signal,
			DBUS_TYPE_STRING, &name,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);

	signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierItemUnregistered");
	dbus_message_append_args(signal,
			DBUS_TYPE_STRING, &name,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);
}

static void obj_path_item_registered_signal(DBusConnection *connection, const struct ObjPathItem *item) {
	DBusMessage *signal = dbus_message_new_signal(
			"/StatusNotifierWatcher",
			"org.swaywm.LessSuckyStatusNotifierWatcher",
			"ObjPathItemRegistered");
	dbus_message_append_args(signal,
			DBUS_TYPE_OBJECT_PATH, &item->obj_path,
			DBUS_TYPE_STRING, &item->unique_name,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, signal, NULL);
	dbus_message_unref(signal);
}

static void respond_to_introspect(DBusConnection *connection, DBusMessage *request) {
	DBusMessage *reply;

	reply = dbus_message_new_method_return(request);
	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &interface_xml,
			DBUS_TYPE_INVALID);
	dbus_connection_send(connection, reply, NULL);
	dbus_message_unref(reply);
}

static void register_item(DBusConnection *connection, DBusMessage *message) {
	DBusError error;
	char *name;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID)) {
		sway_log(L_ERROR, "Error parsing method args: %s", error.message);
	}

	sway_log(L_INFO, "RegisterStatusNotifierItem called with \"%s\"", name);

	// Don't add duplicate or not real item
	if (!dbus_validate_bus_name(name, NULL)) {
		if (dbus_validate_path(name, NULL)) {
			// Item is registered by object path
			struct ObjPathItem *item =
				create_obj_path_item(dbus_message_get_sender(message), name);

			// Add ObjPathItem
			if (list_seq_find(object_path_items, obj_path_item_cmp, item) != -1) {
				free_obj_path_item(item);
				return;
			}
			list_add(object_path_items, item);
			obj_path_item_registered_signal(connection, item);
			return;
		} else {
			sway_log(L_INFO, "This item is not valid, we cannot keep track of it.");
			return;
		}
	} else {

		if (list_seq_find(items, (int (*)(const void *, const void *))strcmp, name) != -1) {
			return;
		}
		if (!dbus_bus_name_has_owner(connection, name, &error)) {
			return;
		}

		list_add(items, strdup(name));
		item_registered_signal(connection, name);
	}

	// It's silly, but clients want a reply for this function
	DBusMessage *reply = dbus_message_new_method_return(message);
	dbus_connection_send(connection, reply, NULL);
	dbus_message_unref(reply);
}

static void register_host(DBusConnection *connection, DBusMessage *message) {
	DBusError error;
	char *name;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID)) {
		sway_log(L_ERROR, "Error parsing method args: %s", error.message);
	}

	sway_log(L_INFO, "RegisterStatusNotifierHost called with \"%s\"", name);

	// Don't add duplicate or not real host
	if (!dbus_validate_bus_name(name, NULL)) {
		sway_log(L_INFO, "This item is not valid, we cannot keep track of it.");
		return;
	}


	if (list_seq_find(hosts, (int (*)(const void *, const void *))strcmp, name) != -1) {
		return;
	}
	if (!dbus_bus_name_has_owner(connection, name, &error)) {
		return;
	}

	list_add(hosts, strdup(name));
	host_registered_signal(connection);
}

static void get_property(DBusConnection *connection, DBusMessage *message) {
	DBusError error;
	char *interface;
	char *property;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_STRING, &interface,
				DBUS_TYPE_STRING, &property,
				DBUS_TYPE_INVALID)) {
		sway_log(L_ERROR, "Error parsing prop args: %s", error.message);
		return;
	}

	if (strcmp(property, "RegisteredStatusNotifierItems") == 0) {
		sway_log(L_INFO, "Replying with items");
		DBusMessage *reply;
		reply = dbus_message_new_method_return(message);
		DBusMessageIter iter;
		DBusMessageIter sub;
		DBusMessageIter subsub;

		dbus_message_iter_init_append(reply, &iter);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
				"as", &sub);
		dbus_message_iter_open_container(&sub, DBUS_TYPE_ARRAY,
				"s", &subsub);

		for (int i = 0; i < items->length; ++i) {
			dbus_message_iter_append_basic(&subsub,
					DBUS_TYPE_STRING, &items->items[i]);
		}

		dbus_message_iter_close_container(&sub, &subsub);
		dbus_message_iter_close_container(&iter, &sub);

		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
	} else if (strcmp(property, "IsStatusNotifierHostRegistered") == 0) {
		DBusMessage *reply;
		DBusMessageIter iter;
		DBusMessageIter sub;
		int registered = (hosts == NULL || hosts->length == 0) ? 0 : 1;

		reply = dbus_message_new_method_return(message);

		dbus_message_iter_init_append(reply, &iter);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
				"b", &sub);
		dbus_message_iter_append_basic(&sub,
				DBUS_TYPE_BOOLEAN, &registered);

		dbus_message_iter_close_container(&iter, &sub);

		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
	} else if (strcmp(property, "ProtocolVersion") == 0) {
		DBusMessage *reply;
		DBusMessageIter iter;
		DBusMessageIter sub;
		const int version = 0;

		reply = dbus_message_new_method_return(message);

		dbus_message_iter_init_append(reply, &iter);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
				"i", &sub);
		dbus_message_iter_append_basic(&sub,
				DBUS_TYPE_INT32, &version);

		dbus_message_iter_close_container(&iter, &sub);
		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
	} else if (strcmp(property, "RegisteredObjectPathItems") == 0) {
		sway_log(L_INFO, "Replying with ObjPathItems");
		DBusMessage *reply;
		reply = dbus_message_new_method_return(message);
		DBusMessageIter iter;
		DBusMessageIter variant;
		DBusMessageIter array;
		DBusMessageIter dstruct;

		dbus_message_iter_init_append(reply, &iter);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
				"a(os)", &variant);
		dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
				"(os)", &array);

		for (int i = 0; i < object_path_items->length; ++i) {
			struct ObjPathItem *item = object_path_items->items[i];

			dbus_message_iter_open_container(&array,
					DBUS_TYPE_STRUCT, NULL, &dstruct);

			dbus_message_iter_append_basic(&dstruct,
					DBUS_TYPE_OBJECT_PATH, &item->obj_path);
			dbus_message_iter_append_basic(&dstruct,
					DBUS_TYPE_STRING, &item->unique_name);

			dbus_message_iter_close_container(&array, &dstruct);
		}

		dbus_message_iter_close_container(&variant, &array);
		dbus_message_iter_close_container(&iter, &variant);

		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
	}
}

static void set_property(DBusConnection *connection, DBusMessage *message) {
	// All properties are read only and we don't allow new properties
	return;
}

// TODO clean me up please or get rid of me
// also add LessSuckyStatusNotifierWatcher props
static void get_all(DBusConnection *connection, DBusMessage *message) {
	DBusMessage *reply;
	reply = dbus_message_new_method_return(message);
	DBusMessageIter iter; /* a{v} */
	DBusMessageIter arr;
	DBusMessageIter dict;
	DBusMessageIter sub;
	DBusMessageIter subsub;
	int registered = (hosts == NULL || hosts->length == 0) ? 0 : 1;
	const int version = 0;
	const char *prop;

	// Could clean this up with a function for each prop
	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			"{sv}", &arr);

	prop = "RegisteredStatusNotifierItems";
	dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY,
			NULL, &dict);
	dbus_message_iter_append_basic(&dict,
			DBUS_TYPE_STRING, &prop);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
			"as", &sub);
	dbus_message_iter_open_container(&sub, DBUS_TYPE_ARRAY,
			"s", &subsub);
	for (int i = 0; i < items->length; ++i) {
		dbus_message_iter_append_basic(&subsub,
				DBUS_TYPE_STRING, &items->items[i]);
	}
	dbus_message_iter_close_container(&sub, &subsub);
	dbus_message_iter_close_container(&dict, &sub);
	dbus_message_iter_close_container(&arr, &dict);

	prop = "IsStatusNotifierHostRegistered";
	dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY,
			NULL, &dict);
	dbus_message_iter_append_basic(&dict,
			DBUS_TYPE_STRING, &prop);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
			"b", &sub);
	dbus_message_iter_append_basic(&sub,
			DBUS_TYPE_BOOLEAN, &registered);
	dbus_message_iter_close_container(&dict, &sub);
	dbus_message_iter_close_container(&arr, &dict);

	prop = "ProtocolVersion";
	dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY,
			NULL, &dict);
	dbus_message_iter_append_basic(&dict,
			DBUS_TYPE_STRING, &prop);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT,
			"i", &sub);
	dbus_message_iter_append_basic(&sub,
			DBUS_TYPE_INT32, &version);
	dbus_message_iter_close_container(&dict, &sub);
	dbus_message_iter_close_container(&arr, &dict);

	dbus_message_iter_close_container(&iter, &arr);

	dbus_connection_send(connection, reply, NULL);
	dbus_message_unref(reply);
}

static DBusHandlerResult message_handler(DBusConnection *connection, 
		DBusMessage *message, void *data) {
	const char *interface_name = dbus_message_get_interface(message);
	const char *member_name = dbus_message_get_member(message);

	// In order of the xml above
	if (strcmp(interface_name, "org.freedesktop.DBus.Introspectable") == 0 &&
			strcmp(member_name, "Introspect") == 0) {
		// We don't have an introspect for KDE
		respond_to_introspect(connection, message);
		return DBUS_HANDLER_RESULT_HANDLED;
	} else if (strcmp(interface_name, "org.freedesktop.DBus.Properties") == 0) {
		if (strcmp(member_name, "Get") == 0) {
			get_property(connection, message);
			return DBUS_HANDLER_RESULT_HANDLED;
		} else if (strcmp(member_name, "Set") == 0) {
			set_property(connection, message);
			return DBUS_HANDLER_RESULT_HANDLED;
		} else if (strcmp(member_name, "GetAll") == 0) {
			get_all(connection, message);
			return DBUS_HANDLER_RESULT_HANDLED;
		} else {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
	} else if (strcmp(interface_name, "org.freedesktop.StatusNotifierWatcher") == 0 ||
			strcmp(interface_name, "org.kde.StatusNotifierWatcher") == 0) {
		if (strcmp(member_name, "RegisterStatusNotifierItem") == 0) {
			register_item(connection, message);
			return DBUS_HANDLER_RESULT_HANDLED;
		} else if (strcmp(member_name, "RegisterStatusNotifierHost") == 0) {
			register_host(connection, message);
			return DBUS_HANDLER_RESULT_HANDLED;
		} else {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult signal_handler(DBusConnection *connection,
		DBusMessage *message, void *_data) {
	if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
		// Only eat the message if it is name that we are watching
		const char *name;
		const char *old_owner;
		const char *new_owner;
		int index;
		bool found_obj_path_item = false;

		if (!dbus_message_get_args(message, NULL,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_STRING, &old_owner,
				DBUS_TYPE_STRING, &new_owner,
				DBUS_TYPE_INVALID)) {
			sway_log(L_ERROR, "Error getting LostName args");
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		if (strcmp(new_owner, "") != 0) {
			// Name is not lost
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		if ((index = list_seq_find(items, (int (*)(const void *, const void *))strcmp, name)) != -1) {
			sway_log(L_INFO, "Status Notifier Item lost %s", name);
			free(items->items[index]);
			list_del(items, index);
			item_unregistered_signal(connection, name);

			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if ((index = list_seq_find(hosts, (int (*)(const void *, const void *))strcmp, name)) != -1) {
			sway_log(L_INFO, "Status Notifier Host lost %s", name);
			free(hosts->items[index]);
			list_del(hosts, index);

			return DBUS_HANDLER_RESULT_HANDLED;
		}
		while ((index = list_seq_find(object_path_items, obj_path_unique_name_cmp, name)) != -1) {
			found_obj_path_item = true;
			struct ObjPathItem *item = object_path_items->items[index];
			sway_log(L_INFO, "ObjPathItem lost %s", item->obj_path);
			list_del(object_path_items, index);
			free_obj_path_item(item);
		}
		if (found_obj_path_item) {
			item_unregistered_signal(connection, name);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable vtable = {
	.message_function = message_handler,
	.unregister_function = NULL,
};

int init_sni_watcher() {
	DBusError error;
	dbus_error_init(&error);
	if (!conn) {
		sway_log(L_ERROR, "Connection is null, cannot initiate StatusNotifierWatcher");
		return -1;
	}

	items = create_list();
	hosts = create_list();
	object_path_items = create_list();

	int status = dbus_bus_request_name(conn, "org.freedesktop.StatusNotifierWatcher",
			DBUS_NAME_FLAG_REPLACE_EXISTING,
			&error);
	if (status == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		sway_log(L_DEBUG, "Got watcher name");
	} else if (status == DBUS_REQUEST_NAME_REPLY_IN_QUEUE) {
		sway_log(L_INFO, "Could not get watcher name, it may start later");
	}
	if (dbus_error_is_set(&error)) {
		sway_log(L_ERROR, "dbus err getting watcher name: %s", error.message);
		return -1;
	}

	status = dbus_bus_request_name(conn, "org.kde.StatusNotifierWatcher",
			DBUS_NAME_FLAG_REPLACE_EXISTING,
			&error);
	if (status == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		sway_log(L_DEBUG, "Got kde watcher name");
	} else if (status == DBUS_REQUEST_NAME_REPLY_IN_QUEUE) {
		sway_log(L_INFO, "Could not get kde watcher name, it may start later");
	}
	if (dbus_error_is_set(&error)) {
		sway_log(L_ERROR, "dbus err getting kde watcher name: %s", error.message);
		return -1;
	}

	dbus_connection_try_register_object_path(conn,
			"/StatusNotifierWatcher",
			&vtable, NULL, &error);
	if (dbus_error_is_set(&error)) {
		sway_log(L_ERROR, "dbus_err: %s", error.message);
		return -1;
	}

	dbus_bus_add_match(conn,
			"type='signal',\
			sender='org.freedesktop.DBus',\
			interface='org.freedesktop.DBus',\
			member='NameOwnerChanged'",
			&error);

	if (dbus_error_is_set(&error)) {
		sway_log(L_ERROR, "DBus error getting match args: %s", error.message);
	}

	dbus_connection_add_filter(conn, signal_handler, NULL, NULL);
	return 0;
}
