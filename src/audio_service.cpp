/* @@@LICENSE
*
* Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#include "audio_service.h"
#include "luna_service_utils.h"
#include "utils.h"

#define SAMPLE_PATH		"/usr/share/systemsounds"

#define VOLUME_STEP		11

extern GMainLoop *event_loop;
static GSList *sample_list = NULL;

struct audio_service {
	LSHandle *handle;
	pa_glib_mainloop *pa_mainloop;
	pa_context *context;
	bool context_initialized;
	int volume;
	int new_volume;
	int mute;
	int new_mute;
	char *default_sink_name;
	int default_sink_index;
	bool in_call;
	bool speaker_mode;
	bool mic_mute;
};

struct play_feedback_data {
	struct audio_service *service;
	LSHandle *handle;
	LSMessage *message;
	char *name;
	char *sink;
	bool play;
	pa_stream *sample_stream;
	unsigned int sample_length;
	unsigned int stream_written;
	int fd;
};

static bool get_status_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool set_volume_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool set_mute_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool play_feedback_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool volume_up_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool volume_down_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool set_call_mode_cb(LSHandle *handle, LSMessage *message, void *user_data);
static bool set_mic_mute_cb(LSHandle *handle, LSMessage *message, void *user_data);

static LSMethod audio_service_methods[]  = {
	{ "getStatus", get_status_cb },
	{ "setVolume", set_volume_cb },
	{ "setMute", set_mute_cb },
	{ "playFeedback", play_feedback_cb },
	{ "volumeUp", volume_up_cb },
	{ "volumeDown", volume_down_cb },
	{ "setCallMode", set_call_mode_cb },
	{ "setMicMute", set_mic_mute_cb },
	{ NULL, NULL }
};

static void play_feedback_data_free(struct play_feedback_data *pfd)
{
	if (pfd->fd > 0)
		close(pfd->fd);

	LSMessageUnref(pfd->message);

	g_free(pfd->name);
	g_free(pfd);
}

static void play_feedback_sample(struct play_feedback_data *pfd)
{
	pa_operation *op;
	pa_proplist *proplist;
	char *sink = pfd->sink;

	if (!pfd->play) {
		luna_service_message_reply_success(pfd->handle, pfd->message);
		return;
	}

	if (!sink)
		sink = pfd->service->default_sink_name;

	if (!sink) {
		luna_service_message_reply_custom_error(pfd->handle, pfd->message, "No sink found to play sample on");
		return;
	}

	/* make sure we're running as event to enable ducking */
	proplist = pa_proplist_new();
	pa_proplist_setf(proplist, PA_PROP_MEDIA_ROLE, "event");

	op = pa_context_play_sample_with_proplist(pfd->service->context, pfd->name, sink, PA_VOLUME_NORM, proplist, NULL, NULL);
	if (op)
		pa_operation_unref(op);

	luna_service_message_reply_success(pfd->handle, pfd->message);
}

static void preload_stream_state_cb(pa_stream *stream, void *user_data)
{
	struct play_feedback_data *pfd = (struct play_feedback_data*) user_data;

	switch (pa_stream_get_state(stream)) {
	case PA_STREAM_CREATING:
	case PA_STREAM_READY:
		return;
	case PA_STREAM_TERMINATED:
		g_message("Successfully uploaded sample %s to pulseaudio", pfd->name);
		sample_list = g_slist_append(sample_list, g_strdup(pfd->name));
		play_feedback_sample(pfd);
		play_feedback_data_free(pfd);
		break;
	case PA_STREAM_FAILED:
	default:
		g_warning("Failed to upload sample %s", pfd->name);
		luna_service_message_reply_custom_error(pfd->handle, pfd->message,
												"Failed to upload sample to pulseaudio");
		play_feedback_data_free(pfd);
		break;
	}
}

static void preload_stream_write_cb(pa_stream *stream, size_t length, void *user_data)
{
	struct play_feedback_data *pfd = (struct play_feedback_data*) user_data;
	void *buffer;
	ssize_t bread;

	buffer = pa_xmalloc(pfd->sample_length);

	bread = read(pfd->fd, buffer, pfd->sample_length);
	pfd->stream_written += bread;

	pa_stream_write(stream, buffer, bread, pa_xfree, 0, PA_SEEK_RELATIVE);

	if (pfd->stream_written == pfd->sample_length) {
		pa_stream_set_write_callback(stream, NULL, NULL);
		pa_stream_finish_upload(stream);
	}
}

static bool preload_sample(struct audio_service *service, struct play_feedback_data *pfd)
{
	bool result = false;
	struct stat st;
	pa_sample_spec spec;
	char *sample_path;

	if (!pfd || !pfd->name)
		return false;

	if (g_slist_find(sample_list, pfd->name)) {
		play_feedback_sample(pfd);
		play_feedback_data_free(pfd);
		return true;
	}

	sample_path = g_strdup_printf("%s/%s.pcm", SAMPLE_PATH, pfd->name);

	if (stat(sample_path, &st) != 0)
		goto cleanup;

	pfd->sample_length = st.st_size;

	spec.format = PA_SAMPLE_S16LE;
	spec.rate = 44100;
	spec.channels = 1;

	pfd->fd = open(sample_path, O_RDONLY);
	if (pfd->fd < 0)
		goto cleanup;

	pfd->sample_stream = pa_stream_new(service->context, pfd->name, &spec, NULL);
	if (!pfd->sample_stream)
		goto cleanup;

	pa_stream_set_state_callback(pfd->sample_stream, preload_stream_state_cb, pfd);
	pa_stream_set_write_callback(pfd->sample_stream, preload_stream_write_cb, pfd);
	pa_stream_connect_upload(pfd->sample_stream, pfd->sample_length);

	result = true;

cleanup:
	g_free(sample_path);

	return result;
}

static bool play_feedback_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	struct play_feedback_data *pfd;
	const char *payload;
	jvalue_ref parsed_obj;
	char *name, *sink;
	bool play;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	name = luna_service_message_get_string(parsed_obj, "name", NULL);
	if (!name) {
		luna_service_message_reply_custom_error(handle, message, "Invalid parameters: name parameter is required");
		goto cleanup;
	}

	play = luna_service_message_get_boolean(parsed_obj, "play", true);

	sink = luna_service_message_get_string(parsed_obj, "sink", NULL);

	pfd = g_new0(struct play_feedback_data, 1);
	pfd->service = service;
	pfd->handle = handle;
	pfd->message = message;
	pfd->name = name;
	pfd->sink = sink;
	pfd->play = play;

	LSMessageRef(message);

	if (!preload_sample(service, pfd)) {
		luna_service_message_reply_custom_error(handle, message, "Could not preload sample");
		LSMessageUnref(message);
		g_free(pfd);
		g_free(name);
		g_free(sink);
		goto cleanup;
	}

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

static bool get_status_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	jvalue_ref reply_obj = NULL;
	bool subscribed = false;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("volume"), jnumber_create_f64(service->volume));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("mute"), jboolean_create(service->mute));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("inCall"), jboolean_create(service->in_call));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("speakerMode"), jboolean_create(service->speaker_mode));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("micMute"), jboolean_create(service->mic_mute));
	if (subscribed)
		jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	if (!luna_service_message_validate_and_send(handle, message, reply_obj))
		goto cleanup;

cleanup:
	if (!jis_null(reply_obj))
		j_release(&reply_obj);

	return true;
}

static void notify_status_subscribers(struct audio_service *service)
{
	jvalue_ref reply_obj = NULL;

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("volume"), jnumber_create_f64(service->volume));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("mute"), jboolean_create(service->mute));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	luna_service_post_subscription(service->handle, "/", "getStatus", reply_obj);

	j_release(&reply_obj);
}

static void set_volume_success_cb(pa_context *context, int success, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;
	struct audio_service *service = (struct audio_service*) req->user_data;

	if (!success) {
		luna_service_message_reply_custom_error(req->handle, req->message, "Could not mute/unmute default sink");
		goto cleanup;
	}

	service->volume = service->new_volume;

	notify_status_subscribers(service);

	luna_service_message_reply_success(req->handle, req->message);

cleanup:
	luna_service_req_data_free(req);
}

static void set_volume(struct audio_service *service, int new_volume, void *user_data)
{
	pa_cvolume volume;
	pa_operation *op;

	service->new_volume = new_volume;

	pa_cvolume_set(&volume, 1, (service->new_volume * (double) (PA_VOLUME_NORM / 100)));
	op = pa_context_set_sink_volume_by_name(service->context, service->default_sink_name, &volume, set_volume_success_cb, user_data);
	pa_operation_unref(op);
}

static bool volume_up_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	struct luna_service_req_data *req;
	int normalized_volume;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	normalized_volume = (service->volume / VOLUME_STEP) * VOLUME_STEP;
	if (normalized_volume >= 99)
		goto done;
	else if (normalized_volume >= 88) /* because VOLUME_STEP is 11, this adjustment is needed to get from 88 to 100 */
		++normalized_volume;


	req = luna_service_req_data_new(handle, message);
	req->user_data = service;

	set_volume(service, normalized_volume + VOLUME_STEP, req);

	return true;

done:
	luna_service_message_reply_success(handle, message);

	return true;
}

static bool volume_down_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	struct luna_service_req_data *req;
	int normalized_volume;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	normalized_volume = ((service->volume + VOLUME_STEP - 1) / VOLUME_STEP) * VOLUME_STEP;
	if (normalized_volume >= 100) /* If service->volume is 100, we'd be at 110. Adjust */
		normalized_volume = 99;
	else if (normalized_volume == 0)
		goto done;

	req = luna_service_req_data_new(handle, message);
	req->user_data = service;

	set_volume(service, normalized_volume - VOLUME_STEP, req);

	return true;

done:
	luna_service_message_reply_success(handle, message);

	return true;
}

static bool set_volume_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	const char *payload;
	jvalue_ref parsed_obj = NULL;
	jvalue_ref volume_obj = NULL;
	struct luna_service_req_data *req;
	int new_volume = 0;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	if (!jobject_get_exists(parsed_obj, J_CSTR_TO_BUF("volume"), &volume_obj) ||
		!jis_number(volume_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	jnumber_get_i32(volume_obj, &new_volume);

	if (new_volume < 0 || new_volume > 100) {
		luna_service_message_reply_custom_error(handle, message, "Volume out of range. Must be in [0;100]");
		goto cleanup;
	}

	if (service->new_volume == service->volume) {
		luna_service_message_reply_custom_error(handle, message,
			"Provided volume doesn't differ from current one");
		goto cleanup;
	}

	req = luna_service_req_data_new(handle, message);
	req->user_data = service;

	set_volume(service, service->new_volume, req);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

static void set_mute_success_cb(pa_context *context, int success, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;
	struct audio_service *service = (struct audio_service*) req->user_data;

	if (!success) {
		luna_service_message_reply_custom_error(req->handle, req->message, "Could not mute/unmute default sink");
		goto cleanup;
	}

	service->mute = service->new_mute;

	notify_status_subscribers(service);

	luna_service_message_reply_success(req->handle, req->message);

cleanup:
	luna_service_req_data_free(req);
}

static bool set_mute_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	const char *payload;
	jvalue_ref parsed_obj = NULL;
	struct luna_service_req_data *req;
	pa_operation *op;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	service->new_mute = (int) luna_service_message_get_boolean(parsed_obj, "mute", (bool) service->mute);

	if (service->new_mute == service->mute) {
		luna_service_message_reply_success(handle, message);
		goto cleanup;
	}

	req = luna_service_req_data_new(handle, message);
	req->user_data = service;

	op = pa_context_set_sink_mute_by_name(service->context, service->default_sink_name, service->new_mute, set_mute_success_cb, req);
	pa_operation_unref(op);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

static void finish_set_call_mode(bool success, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;

	if (success)
		luna_service_message_reply_success(req->handle, req->message);
	else
		luna_service_message_reply_error_internal(req->handle, req->message);

	luna_service_req_data_free(req);
}

static void cm_source_port_set_cb(pa_context *context, int success, void *user_data)
{
	finish_set_call_mode(true, user_data);
}

static void cm_sourceinfo_cb(pa_context *context, const pa_source_info *info, int is_last, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;
	struct audio_service *service = (struct audio_service*) req->user_data;
	pa_source_port_info *builtin_mic = NULL, *headset = NULL;
	pa_source_port_info *preferred = NULL;
	const char *name_to_set = NULL;
	const char *value_to_set = NULL;
	unsigned int i;
	pa_operation *op;
	static bool needreply = true;

	if (is_last) {
		if (needreply)
			finish_set_call_mode(false, user_data);
		needreply = true;
		return;
	}

	if (info->monitor_of_sink != PA_INVALID_INDEX)
		return;  /* Not the right source */

	for (i = 0; i < info->n_ports; i++) {
		if (!strcmp(info->ports[i]->name, "input-builtin_mic"))
			builtin_mic = info->ports[i];
		if (!strcmp(info->ports[i]->name, "input-wired_headset") &&
				info->ports[i]->available != PA_PORT_AVAILABLE_NO)
			headset = info->ports[i];
	}

	if (!builtin_mic)
		return; /* Not the right source */

	preferred = headset ? headset : builtin_mic;

	if (preferred && preferred != info->active_port) {
		name_to_set = info->name;
		value_to_set = preferred->name;
	}

	if (!!info->mute != !!service->mic_mute)
		name_to_set = info->name;

	if (name_to_set) {
		op = pa_context_set_source_port_by_name(service->context, name_to_set, value_to_set, cm_source_port_set_cb, req);
		pa_operation_unref(op);
	}
	else {
		finish_set_call_mode(false, req);
	}
	needreply = false;
}

static void cm_sink_port_set_cb(pa_context *context, int success, void *user_data)
{
	pa_operation *op;
	op = pa_context_get_source_info_list(context, cm_sourceinfo_cb, user_data);
	pa_operation_unref(op);
}

static void cm_sinkinfo_cb(pa_context *context, const pa_sink_info *info, int is_last, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;
	struct audio_service *service = (struct audio_service*) req->user_data;
	pa_sink_port_info *earpiece = NULL, *speaker = NULL, *headphones = NULL;
	pa_sink_port_info *highest = NULL, *preferred = NULL;
	pa_operation *op;
	unsigned int i;
	static bool needreply = true;

	if (is_last) {
		if (needreply)
			finish_set_call_mode(false, user_data);
		needreply = true;
		return;
	}

	for (i = 0; i < info->n_ports; i++) {
		if (!highest || info->ports[i]->priority > highest->priority) {
			if (info->ports[i]->available != PA_PORT_AVAILABLE_NO)
				highest = info->ports[i];
		}
		if (!strcmp(info->ports[i]->name, "output-earpiece"))
			earpiece = info->ports[i];
		if (!strcmp(info->ports[i]->name, "output-speaker"))
			speaker = info->ports[i];
		if (!strcmp(info->ports[i]->name, "output-wired_headset") &&
				info->ports[i]->available != PA_PORT_AVAILABLE_NO)
			headphones = info->ports[i];
		if (!strcmp(info->ports[i]->name, "output-wired_headphone") &&
				info->ports[i]->available != PA_PORT_AVAILABLE_NO)
			headphones = info->ports[i];
	}

	if (!earpiece)
		return; /* Not the right sink */

	/* TODO: When on ringtone and headphones are plugged in, people want output
	   through *both* headphones and speaker, but when on call with speaker mode,
	   people want *just* speaker, not including headphones. */
	if (service->speaker_mode)
		preferred = speaker;
	else if (service->in_call)
		preferred = headphones ? headphones : earpiece;

	if (!preferred)
		preferred = highest;

	if (preferred && preferred != info->active_port) {
		op = pa_context_set_sink_port_by_name(service->context, info->name, preferred->name, cm_sink_port_set_cb, req);
		pa_operation_unref(op);
	}
	else {
		op = pa_context_get_source_info_list(context, cm_sourceinfo_cb, user_data);
		pa_operation_unref(op);
	}
	needreply = false;
}

static void cm_card_profile_set_cb(pa_context *context, int success, void *user_data)
{
	pa_operation *op;
	op = pa_context_get_sink_info_list(context, cm_sinkinfo_cb, user_data);
	pa_operation_unref(op);
}

static void cm_cardinfo_cb(pa_context *context, const pa_card_info *info, int is_last, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;
	struct audio_service *service = (struct audio_service*) req->user_data;
	pa_card_profile_info *voice_call = NULL, *highest = NULL;
	const char *name_to_set = NULL;
	const char *value_to_set = NULL;
	pa_operation *op;
	unsigned int i;
	static bool needreply = true;

	if (is_last) {
		if (needreply)
			finish_set_call_mode(false, user_data);
		needreply = true;
		return;
	}

	for (i = 0; i < info->n_profiles; i++) {
		if (!highest || info->profiles[i].priority > highest->priority)
			highest = &info->profiles[i];
		if (!strcmp(info->profiles[i].name, "voicecall"))
			voice_call = &info->profiles[i];
	}

	if (!voice_call)
		return; /* Not the right card */

	if (service->in_call && (voice_call != info->active_profile)) {
		name_to_set = info->name;
		value_to_set = voice_call->name;
	}
	else if (!service->in_call && (voice_call == info->active_profile)) {
		name_to_set = info->name;
		value_to_set = highest->name;
	}

	if (name_to_set) {
		op = pa_context_set_card_profile_by_name(service->context, name_to_set, value_to_set,
											cm_card_profile_set_cb, req);
		pa_operation_unref(op);
	}
	else {
		op = pa_context_get_sink_info_list(context, cm_sinkinfo_cb, user_data);
		pa_operation_unref(op);
	}
	needreply = false;
}

static bool set_call_mode_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	const char *payload;
	jvalue_ref parsed_obj = NULL;
	struct luna_service_req_data *req;
	pa_operation *op;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	service->in_call = luna_service_message_get_boolean(parsed_obj, "inCall", service->in_call);
	service->speaker_mode = luna_service_message_get_boolean(parsed_obj, "speakerMode", service->in_call);

	req = luna_service_req_data_new(handle, message);
	req->user_data = service;

	op = pa_context_get_card_info_list(service->context, cm_cardinfo_cb, req);
	pa_operation_unref(op);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

static void finish_set_mic_mute(bool success, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;

	if (success)
		luna_service_message_reply_success(req->handle, req->message);
	else
		luna_service_message_reply_error_internal(req->handle, req->message);

	luna_service_req_data_free(req);
}

static void mm_set_source_mute_cb(pa_context *context, int success, void *user_data)
{
	finish_set_mic_mute(true, user_data);
}

static void mm_sourceinfo_cb(pa_context *context, const pa_source_info *info, int is_last, void *user_data)
{
	struct luna_service_req_data *req = (struct luna_service_req_data*) user_data;
	struct audio_service *service = (struct audio_service*) req->user_data;
	pa_source_port_info *builtin_mic = NULL, *headset = NULL;
	pa_source_port_info *preferred = NULL;
	const char *name_to_set = NULL;
	unsigned int i;
	pa_operation *op;
	static bool needreply = true;

	if (is_last) {
		if (needreply)
			finish_set_mic_mute(false, user_data);
		needreply = true;
		return;
	}

	if (info->monitor_of_sink != PA_INVALID_INDEX)
		return;  /* Not the right source */

	for (i = 0; i < info->n_ports; i++) {
		if (!strcmp(info->ports[i]->name, "input-builtin_mic"))
			builtin_mic = info->ports[i];
		if (!strcmp(info->ports[i]->name, "input-wired_headset") &&
				info->ports[i]->available != PA_PORT_AVAILABLE_NO)
			headset = info->ports[i];
	}

	if (!builtin_mic)
		return; /* Not the right source */

	preferred = headset ? headset : builtin_mic;

	if (preferred && preferred != info->active_port)
		name_to_set = info->name;

	if (!!info->mute != !!service->mic_mute)
		name_to_set = info->name;

	if (name_to_set) {
		op = pa_context_set_source_mute_by_name(service->context, name_to_set, service->mic_mute, mm_set_source_mute_cb, req);
		pa_operation_unref(op);
	}
	else {
		finish_set_mic_mute(false, req);
	}
	needreply = false;
}

static bool set_mic_mute_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;
	const char *payload;
	jvalue_ref parsed_obj = NULL;
	struct luna_service_req_data *req;
	pa_operation *op;

	if (!service->context_initialized) {
		luna_service_message_reply_custom_error(handle, message, "Not yet initialized");
		return true;
	}

	payload = LSMessageGetPayload(message);
	parsed_obj = luna_service_message_parse_and_validate(payload);
	if (jis_null(parsed_obj)) {
		luna_service_message_reply_error_bad_json(handle, message);
		goto cleanup;
	}

	service->mic_mute = luna_service_message_get_boolean(parsed_obj, "micMute", service->mic_mute);

	req = luna_service_req_data_new(handle, message);
	req->user_data = service;

	op = pa_context_get_source_info_list(service->context, mm_sourceinfo_cb, req);
	pa_operation_unref(op);

cleanup:
	if (!jis_null(parsed_obj))
		j_release(&parsed_obj);

	return true;
}

static void default_sink_info_cb(pa_context *context, const pa_sink_info *info, int eol, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;

	if (info == NULL)
		return;

	if (service->mute != info->mute)
		service->mute = info->mute;

	int current_volume = (info->volume.values[0] / (PA_VOLUME_NORM / 100));
	if (service->volume != current_volume)
		service->volume = current_volume;

	service->default_sink_index = info->index;
}

static void server_info_cb(pa_context *context, const pa_server_info *info, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;

	if (info == NULL)
		return;

	g_free(service->default_sink_name);
	service->default_sink_name = g_strdup(info->default_sink_name);

	pa_context_get_sink_info_by_name(service->context, info->default_sink_name, default_sink_info_cb, service);
}

static void update_properties(struct audio_service *service)
{
	pa_context_get_server_info(service->context, server_info_cb, service);
}

static void context_subscribe_cb(pa_context *context, pa_subscription_event_type_t type, uint32_t idx, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;

	if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_CARD &&
		(type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE) {
		/* listen for card plug/unplug events */
		/* FIXME */
	}
	else if ((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_SINK) {
		update_properties(service);
	}
}

static void context_state_cb(pa_context *context, void *user_data)
{
	struct audio_service *service = (struct audio_service*) user_data;

	if (!service->context_initialized) {
		switch (pa_context_get_state(service->context)) {
		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
			break;
		case PA_CONTEXT_READY:
			g_message("Successfully established connection to pulseaudio context");
			service->context_initialized = true;
			break;
		case PA_CONTEXT_TERMINATED:
			g_warning("Connection of our context was terminated from pulseaudio");
			break;
		case PA_CONTEXT_FAILED:
			g_warning("Failed to establish a connection to pulseaudio");
			break;
		default:
			break;
		}

		if (service->context_initialized) {
			pa_context_set_subscribe_callback(service->context, context_subscribe_cb, service);
			pa_context_subscribe(service->context, PA_SUBSCRIPTION_MASK_CARD, NULL, service);
			update_properties(service);
		}
	}
}

struct audio_service* audio_service_create()
{
	struct audio_service *service;
	LSError error;
	pa_mainloop_api *mainloop_api;
	char name[100];

	service = g_try_new0(struct audio_service, 1);
	if (!service)
		return NULL;

	LSErrorInit(&error);

	if (!LSRegisterPubPriv("org.webosports.audio", &service->handle, false, &error)) {
		g_warning("Failed to register the luna service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSRegisterCategory(service->handle, "/", audio_service_methods,
			NULL, NULL, &error)) {
		g_warning("Could not register service category: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSCategorySetData(service->handle, "/", service, &error)) {
		g_warning("Could not set daa for service category: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSGmainAttach(service->handle, event_loop, &error)) {
		g_warning("Could not attach service handle to mainloop: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	service->pa_mainloop = pa_glib_mainloop_new(g_main_context_default());
	mainloop_api = pa_glib_mainloop_get_api(service->pa_mainloop);

	snprintf(name, 100, "AudioServiceContext:%i", getpid());
	service->context = pa_context_new(mainloop_api, name);
	service->context_initialized = false;
	pa_context_set_state_callback(service->context, context_state_cb, service);

	if (pa_context_connect(service->context, NULL, (pa_context_flags_t) 0, NULL) < 0) {
		g_warning("Failed to connect to PulseAudio");
		pa_context_unref(service->context);
		pa_glib_mainloop_free(service->pa_mainloop);
		goto error;
	}

	sample_list = g_slist_alloc();

	return service;

error:
	if (service->handle != NULL) {
		LSUnregister(service->handle, &error);
		LSErrorFree(&error);
	}

	g_free(service);

	return NULL;
}

void audio_service_free(struct audio_service *service)
{
	LSError error;

	LSErrorInit(&error);

	if (service->handle != NULL && LSUnregister(service->handle, &error) < 0) {
		g_warning("Could not unregister service: %s", error.message);
		LSErrorFree(&error);
	}

	g_slist_free_full(sample_list, g_free);

	g_free(service->default_sink_name);
	g_free(service);
}

// vim:ts=4:sw=4:noexpandtab
