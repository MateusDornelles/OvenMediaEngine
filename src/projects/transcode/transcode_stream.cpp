//==============================================================================
//
//  TranscodeStream
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include <unistd.h>

#include "transcode_stream.h"
#include "transcode_application.h"
#include "utilities.h"

#include <config/config_manager.h>

#define OV_LOG_TAG "TranscodeStream"

common::MediaCodecId GetCodecId(ov::String name)
{
	name.MakeUpper();

	// Video codecs
	if(name == "H264")
	{
		return common::MediaCodecId::H264;
	}
	else if(name == "VP8")
	{
		return common::MediaCodecId::Vp8;
	}
	else if(name == "VP9")
	{
		return common::MediaCodecId::Vp9;
	}

	// Audio codecs
	if(name == "FLV")
	{
		return common::MediaCodecId::Flv;
	}
	else if(name == "AAC")
	{
		return common::MediaCodecId::Aac;
	}
	else if(name == "MP3")
	{
		return common::MediaCodecId::Mp3;
	}
	else if(name == "OPUS")
	{
		return common::MediaCodecId::Opus;
	}

	return common::MediaCodecId::None;
}

int GetBitrate(ov::String bitrate)
{
	bitrate.MakeUpper();

	int multiplier = 1;

	if(bitrate.HasSuffix("K"))
	{
		multiplier = 1024;
	}
	else if(bitrate.HasSuffix("M"))
	{
		multiplier = 1024 * 1024;
	}

	return static_cast<int>(ov::Converter::ToFloat(bitrate) * multiplier);
}

TranscodeStream::TranscodeStream(const info::Application &application_info, std::shared_ptr<StreamInfo> stream_info, TranscodeApplication *parent)
	: _application_info(application_info)
{
	logtd("Created Transcode stream. name(%s)", stream_info->GetName().CStr());

	// 통계 정보 초기화
	_stats_decoded_frame_count = 0;

	// 부모 클래스
	_parent = parent;

	// 입력 스트림 정보
	_stream_info_input = stream_info;

	// Prepare decoders
	for(auto &track : _stream_info_input->GetTracks())
	{
		CreateDecoder(track.second->GetId());
	}

	auto encodes = _application_info.GetEncodes();

	for(const auto &encode : encodes)
	{
		if(encode.IsActive() == false)
		{
			continue;
		}

		//auto stream_name = encode.GetStreamName();
		ov::String stream_name = "";

		// Resolve stream name
		// ${OriginStreamName} => stream_info->GetName()
		stream_name = stream_name.Replace("${OriginStreamName}", stream_info->GetName());

		if(AddStreamInfoOutput(stream_name) == false)
		{
			continue;
		}

		auto video_profile = encode.GetVideoProfile();
		auto audio_profile = encode.GetAudioProfile();

		if((video_profile != nullptr) && (video_profile->IsActive()))
		{
			bool ret = AddContext(
				stream_name,
				GetCodecId(video_profile->GetCodec()),
				GetBitrate(video_profile->GetBitrate()),
				video_profile->GetWidth(),
				video_profile->GetHeight(),
				video_profile->GetFramerate()
			);
			if(ret == false)
			{
				continue;
			}
		}

		if((audio_profile != nullptr) && (audio_profile->IsActive()))
		{
			bool ret = AddContext(
				stream_name,
				GetCodecId(audio_profile->GetCodec()),
				GetBitrate(audio_profile->GetBitrate()),
				audio_profile->GetSamplerate()
			);
			if(ret == false)
			{
				continue;
			}
		}
	}

	///////////////////////////////////////////////////////
	// 트랜스코딩된 스트림을 생성함
	///////////////////////////////////////////////////////

	// Copy tracks (video & audio)
	for(auto &track : _stream_info_input->GetTracks())
	{
		// 기존 스트림의 미디어 트랙 정보
		auto &cur_track = track.second;
		CreateEncoders(cur_track);
	}

	// 패킷 저리 스레드 생성
	try
	{
		_kill_flag = false;

		_thread_decode = std::thread(&TranscodeStream::DecodeTask, this);
		_thread_filter = std::thread(&TranscodeStream::FilterTask, this);
		_thread_encode = std::thread(&TranscodeStream::EncodeTask, this);
	}
	catch(const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start transcode stream thread.");
	}

	logtd("Started transcode stream thread.");
}

TranscodeStream::~TranscodeStream()
{
	logtd("Destroyed Transcode Stream.  name(%s) id(%u)", _stream_info_input->GetName().CStr(), _stream_info_input->GetId());

	// 스레드가 종료가 안된경우를 확인해서 종료함
	if(_kill_flag != true)
	{
		Stop();
	}
}

void TranscodeStream::Stop()
{
	_kill_flag = true;

	logtd("wait for terminated trancode stream thread. kill_flag(%s)", _kill_flag ? "true" : "false");

	_queue.abort();
	_thread_decode.join();

	_queue_decoded.abort();

	_thread_filter.join();

	_queue_filterd.abort();

	_thread_encode.join();
}

std::shared_ptr<StreamInfo> TranscodeStream::GetStreamInfo()
{
	return _stream_info_input;
}

bool TranscodeStream::Push(std::unique_ptr<MediaPacket> packet)
{
	// logtd("Stage-1-1 : %f", (float)frame->GetPts());
	// 변경된 스트림을 큐에 넣음
	_queue.push(std::move(packet));

	return true;
}

// std::unique_ptr<MediaFrame> TranscodeStream::Pop()
// {
// 	return _queue.pop_unique();
// }

uint32_t TranscodeStream::GetBufferCount()
{
	return _queue.size();
}

void TranscodeStream::CreateDecoder(int32_t media_track_id)
{
	auto track = _stream_info_input->GetTrack(media_track_id);

	if(track == nullptr)
	{
		logte("media track allocation failed");

		return;
	}

	// create decoder for codec id
	_decoders[media_track_id] = std::move(TranscodeDecoder::CreateDecoder(track->GetCodecId()));
}

void TranscodeStream::CreateEncoder(std::shared_ptr<MediaTrack> media_track, std::shared_ptr<TranscodeContext> transcode_context)
{
	if(media_track == nullptr)
	{
		return;
	}

	// create encoder for codec id
	_encoders[media_track->GetId()] = std::move(TranscodeEncoder::CreateEncoder(media_track->GetCodecId(), transcode_context));
}

void TranscodeStream::ChangeOutputFormat(MediaFrame *buffer)
{
	if(buffer == nullptr)
	{
		logte("invalid media buffer");
		return;
	}

	int32_t track_id = buffer->GetTrackId();

	// 트랙 정보
	auto &track = _stream_info_input->GetTrack(track_id);
	if(track == nullptr)
	{
		logte("cannot find output media track. track_id(%d)", track_id);

		return;
	}

	CreateFilters(track, buffer);
}

TranscodeResult TranscodeStream::do_decode(int32_t track_id, std::unique_ptr<const MediaPacket> packet)
{
	////////////////////////////////////////////////////////
	// 1) 디코더에 전달함
	////////////////////////////////////////////////////////
	auto decoder_item = _decoders.find(track_id);

	if(decoder_item == _decoders.end())
	{
		return TranscodeResult::NoData;
	}

	TranscodeDecoder *decoder = decoder_item->second.get();
	decoder->SendBuffer(std::move(packet));

	while(true)
	{
		TranscodeResult result;
		auto ret_frame = decoder->RecvBuffer(&result);

		switch(result)
		{
			case TranscodeResult::FormatChanged:
				// output format change 이벤트가 발생하면...
				// filter 및 인코더를 여기서 다시 초기화 해줘야함.
				// 디코더에 의해서 포맷 정보가 새롭게 알게되거나, 변경됨을 나타내를 반환값

				// logte("changed output format");
				// 필터 컨테스트 생성
				// 인코더 커테스트 생성
				ret_frame->SetTrackId(track_id);

				ChangeOutputFormat(ret_frame.get());
				break;

			case TranscodeResult::DataReady:
				// 디코딩이 성공하면,
				ret_frame->SetTrackId(track_id);

				// 필터 단계로 전달
				// do_filter(track_id, std::move(ret_frame));

				_stats_decoded_frame_count++;

				if(_stats_decoded_frame_count % 300 == 0)
				{
					logtd("stats. rq(%d), dq(%d), fq(%d)", _queue.size(), _queue_decoded.size(), _queue_filterd.size());
				}

				_queue_decoded.push(std::move(ret_frame));
				break;

			default:
				// 에러, 또는 디코딩된 패킷이 없다면 종료
				return result;
		}
	}
}

TranscodeResult TranscodeStream::do_filter(int32_t track_id, std::unique_ptr<MediaFrame> frame)
{
	////////////////////////////////////////////////////////
	// 1) 디코더에 전달함
	////////////////////////////////////////////////////////
	auto filter = _filters.find(track_id);

	if(filter == _filters.end())
	{
		return TranscodeResult::NoData;
	}

	logd("TranscodeStream.Packet", "SendBuffer to do_filter()\n%s", ov::Dump(frame->GetBuffer(0), frame->GetBufferSize(0), 32).CStr());

	filter->second->SendBuffer(std::move(frame));

	while(true)
	{
		TranscodeResult result;
		auto ret_frame = filter->second->RecvBuffer(&result);

		// 에러, 또는 디코딩된 패킷이 없다면 종료
		switch(result)
		{
			case TranscodeResult::DataReady:
				ret_frame->SetTrackId(track_id);

				logd("Transcode.Packet", "Received from filter:\n%s", ov::Dump(ret_frame->GetBuffer(0), ret_frame->GetBufferSize(0), 32).CStr());

				// logtd("filtered frame. track_id(%d), pts(%.0f)", ret_frame->GetTrackId(), (float)ret_frame->GetPts());

				_queue_filterd.push(std::move(ret_frame));
				// do_encode(track_id, std::move(ret_frame));
				break;

			default:
				return result;
		}
	}

	return TranscodeResult::DataReady;
}

TranscodeResult TranscodeStream::do_encode(int32_t track_id, std::unique_ptr<const MediaFrame> frame)
{
	if(_encoders.find(track_id) == _encoders.end())
	{
		return TranscodeResult::NoData;
	}

	////////////////////////////////////////////////////////
	// 2) 인코더에 전달
	////////////////////////////////////////////////////////

	auto &encoder = _encoders[track_id];
	encoder->SendBuffer(std::move(frame));

	while(true)
	{
		TranscodeResult result;
		auto ret_packet = encoder->RecvBuffer(&result);

		if(static_cast<int>(result) < 0)
		{
			return result;
		}

		if(result == TranscodeResult::DataReady)
		{
			ret_packet->SetTrackId(track_id);

			// logtd("encoded packet. track_id(%d), pts(%.0f)", ret_packet->GetTrackId(), (float)ret_packet->GetPts());

			////////////////////////////////////////////////////////
			// 3) 미디어 라우더에 전달
			////////////////////////////////////////////////////////
			SendFrame(std::move(ret_packet));
		}
	}

	return TranscodeResult::DataReady;
}

// 디코딩 & 인코딩 스레드
void TranscodeStream::DecodeTask()
{
	CreateStreams();

	logtd("Started transcode stream decode thread");

	while(!_kill_flag)
	{
		// 큐에 있는 인코딩된 패킷을 읽어옴
		auto packet = _queue.pop_unique();
		if(packet == nullptr)
		{
			// logtw("invliad media buffer");
			continue;
		}

		// 패킷의 트랙 아이디를 조회
		int32_t track_id = packet->GetTrackId();

		do_decode(track_id, std::move(packet));
	}

	// 스트림 삭제 전송
	DeleteStreams();

	logtd("Terminated transcode stream decode thread");
}

void TranscodeStream::FilterTask()
{
	logtd("Transcode filter thread is started");

	while(!_kill_flag)
	{
		// 큐에 있는 인코딩된 패킷을 읽어옴
		auto frame = _queue_decoded.pop_unique();
		if(frame == nullptr)
		{
			// logtw("invliad media buffer");
			continue;
		}

		DoFilters(std::move(frame));
	}

	logtd("Transcode filter thread is terminated");
}

void TranscodeStream::EncodeTask()
{
	logtd("Started transcode stream encode thread");

	while(!_kill_flag)
	{
		// 큐에 있는 인코딩된 패킷을 읽어옴
		auto frame = _queue_filterd.pop_unique();
		if(frame == nullptr)
		{
			// logtw("invliad media buffer");
			continue;
		}

		// 패킷의 트랙 아이디를 조회
		int32_t track_id = frame->GetTrackId();

		// logtd("Stage-1-2 : %f", (float)frame->GetPts());
		do_encode(track_id, std::move(frame));
	}

	logtd("Terminated transcode stream encode thread");
}

bool TranscodeStream::AddStreamInfoOutput(ov::String stream_name)
{
	auto stream_info_output = std::make_shared<StreamInfo>();
	stream_info_output->SetName(stream_name);

	if(_stream_info_outputs.insert(
		std::make_pair(stream_name, stream_info_output)).second == false)
	{
		logtw("The stream [%s] already exists", stream_name.CStr());
		return false;
	}
	return true;
}

void TranscodeStream::CreateStreams()
{
	for(auto &iter : _stream_info_outputs)
	{
		_parent->CreateStream(iter.second);
	}
}

void TranscodeStream::DeleteStreams()
{
	for(auto &iter : _stream_info_outputs)
	{
		_parent->DeleteStream(iter.second);
	}
}

void TranscodeStream::SendFrame(std::unique_ptr<MediaPacket> packet)
{
	uint8_t track_id = packet->GetTrackId();

	auto item = _contexts.find(track_id);
	auto stream_name = item->second->GetStreamName();

	for(auto &iter : _stream_info_outputs)
	{
		if(stream_name != iter.first)
		{
			continue;
		}
		_parent->SendFrame(iter.second, std::move(packet));
	}
}

void TranscodeStream::CreateEncoders(std::shared_ptr<MediaTrack> media_track)
{
	for(auto &iter : _contexts)
	{
		if((media_track->GetMediaType() == common::MediaType::Video && iter.second->GetMediaType() == common::MediaType::Audio) ||
		   media_track->GetMediaType() == common::MediaType::Audio && iter.second->GetMediaType() == common::MediaType::Video)
		{
			continue;
		}

		auto new_track = std::make_shared<MediaTrack>();
		new_track->SetId(iter.first);
		new_track->SetMediaType(media_track->GetMediaType());
		new_track->SetCodecId(iter.second->GetCodecId());
		new_track->SetTimeBase(iter.second->GetTimeBase().GetNum(), iter.second->GetTimeBase().GetDen());

		if(media_track->GetMediaType() == common::MediaType::Video)
		{
			new_track->SetWidth(iter.second->GetVideoWidth());
			new_track->SetHeight(iter.second->GetVideoHeight());
			new_track->SetFrameRate(iter.second->GetFrameRate());

		}
		else if(media_track->GetMediaType() == common::MediaType::Audio)
		{
			new_track->SetSampleRate(iter.second->GetAudioSampleRate());
			new_track->GetSample().SetFormat(iter.second->GetAudioSample().GetFormat());
			new_track->GetChannel().SetLayout(iter.second->GetAudioChannel().GetLayout());
		}
		else
		{
			OV_ASSERT2(false);
			continue;
		}

		auto stream_name = iter.second->GetStreamName();
		auto item = _stream_info_outputs.find(stream_name);
		if(item == _stream_info_outputs.end())
		{
			OV_ASSERT2(false);
			return;
		}
		item->second->AddTrack(new_track);

		// av_log_set_level(AV_LOG_DEBUG);
		CreateEncoder(new_track, iter.second);
	}
}

void TranscodeStream::CreateFilters(std::shared_ptr<MediaTrack> media_track, MediaFrame *buffer)
{
	for(auto &iter : _contexts)
	{
		if((media_track->GetMediaType() == common::MediaType::Video && iter.second->GetMediaType() == common::MediaType::Audio) ||
		   media_track->GetMediaType() == common::MediaType::Audio && iter.second->GetMediaType() == common::MediaType::Video)
		{
			continue;
		}

		if(media_track->GetMediaType() == common::MediaType::Video)
		{
			media_track->SetWidth(media_track->GetWidth());
			media_track->SetHeight(media_track->GetHeight());
		}
		else if(media_track->GetMediaType() == common::MediaType::Audio)
		{
			media_track->SetSampleRate(buffer->GetSampleRate());
			media_track->GetSample().SetFormat(buffer->GetFormat<common::AudioSample::Format>());
			media_track->GetChannel().SetLayout(buffer->GetChannelLayout());
		}
		else
		{
			OV_ASSERT2(false);
			continue;
		}
		_filters[iter.first] = std::make_unique<TranscodeFilter>(media_track, iter.second);
	}
}

void TranscodeStream::DoFilters(std::unique_ptr<MediaFrame> frame)
{
	// 패킷의 트랙 아이디를 조회
	int32_t track_id = frame->GetTrackId();

	for(auto &iter: _contexts)
	{
		if((track_id == (uint32_t)common::MediaType::Video && iter.second->GetMediaType() == common::MediaType::Audio) ||
		   track_id == (uint32_t)common::MediaType::Audio && iter.second->GetMediaType() == common::MediaType::Video)
		{
			continue;
		}

		auto frame_clone = frame->CloneFrame();
		if(frame_clone == nullptr)
		{
			logte("FilterTask -> Unknown Frame");
			continue;
		}
		do_filter(iter.first, std::move(frame_clone));
	}
}

bool TranscodeStream::AddContext(
	ov::String &stream_name,
	common::MediaCodecId codec_id,
	int bitrate,
	int width,
	int height,
	float framerate)
{
	// 96-127 dynamic : RTP Payload Types for standard audio and video encodings
	// 0x60 ~ 0x6F
	if(_last_track_video >= 0x70)
	{
		logte("The number of video encoders that can be supported is 16");
		return false;
	}
	_contexts[_last_track_video++] = std::make_shared<TranscodeContext>(stream_name, codec_id, bitrate, width, height, framerate);

	return true;
}

bool TranscodeStream::AddContext(
	ov::String &stream_name,
	common::MediaCodecId codec_id,
	int bitrate,
	float samplerate)
{
	// 96-127 dynamic : RTP Payload Types for standard audio and video encodings
	// 0x70 ~ 0x7F
	if(_last_track_audio > 0x7F)
	{
		logte("The number of audio encoders that can be supported is 16");
		return false;
	}
	_contexts[_last_track_audio++] = std::make_shared<TranscodeContext>(stream_name, codec_id, bitrate, samplerate);

	return true;
}