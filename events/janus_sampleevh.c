/*! \file   janus_sampleevh.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus SampleEventHandler plugin
 * \details  This is a trivial event handler plugin for Janus, which is only
 * there to showcase how you can handle an event coming from the Janus core
 * or one of the plugins. This specific plugin forwards every event it receives
 * to a web server via an HTTP POST request, using libcurl.
 * 
 * \ingroup eventhandlers
 * \ref eventhandlers
 */

#include "eventhandler.h"

#include <curl/curl.h>

#include "../debug.h"
#include "../config.h"
#include "../mutex.h"
#include "../utils.h"


/* Plugin information */
#define JANUS_SAMPLEEVH_VERSION			1
#define JANUS_SAMPLEEVH_VERSION_STRING	"0.0.1"
#define JANUS_SAMPLEEVH_DESCRIPTION		"This is a trivial sample event handler plugin for Janus, which forwards events via HTTP POST."
#define JANUS_SAMPLEEVH_NAME			"JANUS SampleEventHandler plugin"
#define JANUS_SAMPLEEVH_AUTHOR			"Meetecho s.r.l."
#define JANUS_SAMPLEEVH_PACKAGE			"janus.eventhandler.sampleevh"

/* Plugin methods */
janus_eventhandler *create(void);
int janus_sampleevh_init(const char *config_path);
void janus_sampleevh_destroy(void);
int janus_sampleevh_get_api_compatibility(void);
int janus_sampleevh_get_version(void);
const char *janus_sampleevh_get_version_string(void);
const char *janus_sampleevh_get_description(void);
const char *janus_sampleevh_get_name(void);
const char *janus_sampleevh_get_author(void);
const char *janus_sampleevh_get_package(void);
void janus_sampleevh_incoming_event(json_t *event);

/* Event handler setup */
static janus_eventhandler janus_sampleevh =
	JANUS_EVENTHANDLER_INIT (
		.init = janus_sampleevh_init,
		.destroy = janus_sampleevh_destroy,

		.get_api_compatibility = janus_sampleevh_get_api_compatibility,
		.get_version = janus_sampleevh_get_version,
		.get_version_string = janus_sampleevh_get_version_string,
		.get_description = janus_sampleevh_get_description,
		.get_name = janus_sampleevh_get_name,
		.get_author = janus_sampleevh_get_author,
		.get_package = janus_sampleevh_get_package,
		
		.incoming_event = janus_sampleevh_incoming_event,

		.events_mask = JANUS_EVENT_TYPE_NONE
	);

/* Plugin creator */
janus_eventhandler *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_SAMPLEEVH_NAME);
	return &janus_sampleevh;
}


/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static GThread *handler_thread;
static void *janus_sampleevh_handler(void *data);

/* Queue of events to handle */
static GAsyncQueue *events = NULL;
static json_t exit_event;
static void janus_sampleevh_event_free(json_t *event) {
	if(!event || event == &exit_event)
		return;
	json_decref(event);
}

/* Web backend to send the events to */
static char *backend = NULL;
static size_t janus_sampleehv_write_data(void *buffer, size_t size, size_t nmemb, void *userp) {
	return size*nmemb;
}

/* Plugin implementation */
int janus_sampleevh_init(const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	gboolean enabled = FALSE;
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_SAMPLEEVH_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config != NULL) {
		/* Handle configuration */
		janus_config_print(config);

		/* Setup the sample event handler, if required */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "enabled");
		if(!item || !item->value || !janus_is_true(item->value)) {
			JANUS_LOG(LOG_WARN, "Sample event handler disabled (Janus API)\n");
		} else {
			/* Backend to send events to */
			item = janus_config_get_item_drilldown(config, "general", "backend");
			if(!item || !item->value || strstr(item->value, "http") != item->value) {
				JANUS_LOG(LOG_WARN, "Missing or invalid backend\n");
			} else {
				backend = g_strdup(item->value);
				/* Which events should we subscribe to? */
				item = janus_config_get_item_drilldown(config, "general", "events");
				if(item && item->value) {
					if(!strcasecmp(item->value, "none")) {
						/* Don't subscribe to anything at all */
						janus_flags_reset(&janus_sampleevh.events_mask);
					} else if(!strcasecmp(item->value, "all")) {
						/* Subscribe to everything */
						janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_ALL);
					} else {
						/* Check what we need to subscribe to */
						gchar **subscribe = g_strsplit(item->value, ",", -1);
						if(subscribe != NULL) {
							gchar *index = subscribe[0];
							if(index != NULL) {
								int i=0;
								while(index != NULL) {
									while(isspace(*index))
										index++;
									if(strlen(index)) {
										if(!strcasecmp(index, "sessions")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_SESSION);
										} else if(!strcasecmp(index, "handles")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_HANDLE);
										} else if(!strcasecmp(index, "jsep")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_JSEP);
										} else if(!strcasecmp(index, "webrtc")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_WEBRTC);
										} else if(!strcasecmp(index, "media")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_MEDIA);
										} else if(!strcasecmp(index, "plugins")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_PLUGIN);
										} else if(!strcasecmp(index, "transports")) {
											janus_flags_set(&janus_sampleevh.events_mask, JANUS_EVENT_TYPE_TRANSPORT);
										} else {
											JANUS_LOG(LOG_WARN, "Unknown event type '%s'\n", index);
										}
									}
									i++;
									index = subscribe[i];
								}
							}
							g_strfreev(subscribe);
						}
					}
				}
				/* Done */
				enabled = TRUE;
			}
		}
	}

	janus_config_destroy(config);
	config = NULL;
	if(!enabled) {
		JANUS_LOG(LOG_FATAL, "Sample event handler not enabled/needed, giving up...\n");
		return -1;	/* No point in keeping the plugin loaded */
	}
	JANUS_LOG(LOG_VERB, "Sample event handler configured: %s\n", backend);

	/* Initialize libcurl, needed for forwarding events via HTTP POST */
	curl_global_init(CURL_GLOBAL_ALL);

	/* Initialize the events queue */
	events = g_async_queue_new_full((GDestroyNotify) janus_sampleevh_event_free);
	
	g_atomic_int_set(&initialized, 1);

	/* Launch the thread that will handle incoming events */
	GError *error = NULL;
	handler_thread = g_thread_try_new("janus sampleevh handler", janus_sampleevh_handler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SampleEventHandler handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_SAMPLEEVH_NAME);
	return 0;
}

void janus_sampleevh_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(events, &exit_event);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}

	g_async_queue_unref(events);
	events = NULL;

	g_free(backend);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_SAMPLEEVH_NAME);
}

int janus_sampleevh_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_EVENTHANDLER_API_VERSION;
}

int janus_sampleevh_get_version(void) {
	return JANUS_SAMPLEEVH_VERSION;
}

const char *janus_sampleevh_get_version_string(void) {
	return JANUS_SAMPLEEVH_VERSION_STRING;
}

const char *janus_sampleevh_get_description(void) {
	return JANUS_SAMPLEEVH_DESCRIPTION;
}

const char *janus_sampleevh_get_name(void) {
	return JANUS_SAMPLEEVH_NAME;
}

const char *janus_sampleevh_get_author(void) {
	return JANUS_SAMPLEEVH_AUTHOR;
}

const char *janus_sampleevh_get_package(void) {
	return JANUS_SAMPLEEVH_PACKAGE;
}

void janus_sampleevh_incoming_event(json_t *event) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		/* Janus is closing or the plugin is: unref the event as we won't handle it */
		json_decref(event);
		return;
	}

	/* Do NOT handle the event here in this callback! Since Janus notifies you right
	 * away when something happens, these events are triggered from working threads and
	 * not some sort of message bus. As such, performing I/O or network operations in
	 * here could dangerously slow Janus down. Let's just reference and enqueue the event,
	 * and handle it in our own thread: the event contains a monotonic time indicator of
	 * when the event actually happened on this machine, so that, if relevant, we can compute
	 * any delay in the actual event processing ourselves. */
	json_incref(event);
	g_async_queue_push(events, event);

}


/* Thread to handle incoming events */
static void *janus_sampleevh_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining SampleEventHandler handler thread\n");
	json_t *event = NULL;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		event = g_async_queue_pop(events);
		if(event == NULL)
			continue;
		if(event == &exit_event)
			break;

		/* Handle event: just for fun, let's see how long it took for us to take care of this */
		json_t *created = json_object_get(event, "timestamp");
		if(created && json_is_integer(created)) {
			gint64 then = json_integer_value(created);
			gint64 now = janus_get_monotonic_time();
			JANUS_LOG(LOG_DBG, "Handled event after %"SCNu64" us\n", now-then);
		}

		/* Convert to string... */
		char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		/* ... and send via HTTP POST */
		CURLcode res;
		CURL *curl = curl_easy_init();
		if(curl == NULL) {
			JANUS_LOG(LOG_ERR, "Error initializing CURL context\n");
			goto done;
		}
		curl_easy_setopt(curl, CURLOPT_URL, backend);
		struct curl_slist *headers = NULL;
		headers = curl_slist_append(headers, "Accept: application/json");
		headers = curl_slist_append(headers, "Content-Type: application/json");
		headers = curl_slist_append(headers, "charsets: utf-8");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, event_text);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, janus_sampleehv_write_data);
		/* Send the request */
		res = curl_easy_perform(curl);
		if(res != CURLE_OK) {
			JANUS_LOG(LOG_ERR, "Couldn't relay event to the backend: %s\n", curl_easy_strerror(res));
		} else {
			JANUS_LOG(LOG_DBG, "Event sent!\n");
		}
done:
		/* Cleanup */
		curl_easy_cleanup(curl);
		g_free(event_text);
		/* Done, let's unref the event */
		json_decref(event);
	}
	JANUS_LOG(LOG_VERB, "Leaving SampleEventHandler handler thread\n");
	return NULL;
}
