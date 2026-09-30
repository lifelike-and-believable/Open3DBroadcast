// Copyright Lifelike & Believable. All Rights Reserved.

#include "LiveKitFfiApi.h"
#include "WebRTCUtils.h"

bool FLkFfiApi::IsComplete() const
{
	return lk_free_str
		&& lk_client_create
		&& lk_client_destroy
		&& lk_client_set_data_callback
		&& lk_client_set_data_callback_ex
		&& lk_client_set_audio_callback_ex
		&& lk_set_connection_callback
		&& lk_connect_with_role_async
		&& lk_disconnect
		&& lk_refresh_token
		&& lk_set_audio_publish_options
		&& lk_set_audio_output_format
		&& lk_audio_track_create
		&& lk_audio_track_destroy
		&& lk_audio_track_publish_pcm_i16
		&& lk_send_data_ex
		&& lk_set_default_data_labels
		&& lk_set_log_level;
}

FString FLkFfiApi::TakeMessage(const LkResult& Result) const
{
	if (!Result.message)
	{
		return FString();
	}

	const FString Message = WebRTCUtils::FromAnsi(Result.message);
	if (lk_free_str)
	{
		lk_free_str(const_cast<char*>(Result.message));
	}
	return Message;
}

namespace
{
	FLkFfiApi BuildLinkedLkFfiApi()
	{
		// Taking a function's address does not load the delay-loaded DLL; the first call does.
		FLkFfiApi Api;
		Api.lk_free_str = &::lk_free_str;
		Api.lk_client_create = &::lk_client_create;
		Api.lk_client_destroy = &::lk_client_destroy;
		Api.lk_client_set_data_callback = &::lk_client_set_data_callback;
		Api.lk_client_set_data_callback_ex = &::lk_client_set_data_callback_ex;
		Api.lk_client_set_audio_callback_ex = &::lk_client_set_audio_callback_ex;
		Api.lk_set_connection_callback = &::lk_set_connection_callback;
		Api.lk_connect_with_role_async = &::lk_connect_with_role_async;
		Api.lk_disconnect = &::lk_disconnect;
		Api.lk_refresh_token = &::lk_refresh_token;
		Api.lk_set_audio_publish_options = &::lk_set_audio_publish_options;
		Api.lk_set_audio_output_format = &::lk_set_audio_output_format;
		Api.lk_audio_track_create = &::lk_audio_track_create;
		Api.lk_audio_track_destroy = &::lk_audio_track_destroy;
		Api.lk_audio_track_publish_pcm_i16 = &::lk_audio_track_publish_pcm_i16;
		Api.lk_send_data_ex = &::lk_send_data_ex;
		Api.lk_set_default_data_labels = &::lk_set_default_data_labels;
		Api.lk_set_log_level = &::lk_set_log_level;
		return Api;
	}
}

const FLkFfiApi& GetLinkedLkFfiApi()
{
	static const FLkFfiApi Api = BuildLinkedLkFfiApi();
	return Api;
}
