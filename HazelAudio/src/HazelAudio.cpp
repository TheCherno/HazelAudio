#include "HazelAudio.h"
#include <stdio.h>
#include <stdlib.h>

#include <thread>
#include <filesystem>

#include "AL/al.h"
#include "AL/alext.h"
#include "alhelpers.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"

#include "vorbis/codec.h"
#include "vorbis/vorbisfile.h"

namespace Hazel {

	static mp3dec_t s_Mp3d;

	static uint8_t* s_AudioScratchBuffer;
	static uint32_t s_AudioScratchBufferSize = 10 * 1024 * 1024; // 10mb initially

	// Currently supported file formats
	enum class AudioFileFormat
	{
		None = 0,
		Ogg,
		MP3
	};

	static AudioFileFormat GetFileFormat(const std::string& filename)
	{
		std::filesystem::path path = filename;
		std::string extension = path.extension().string();

		if (extension == ".ogg")  return AudioFileFormat::Ogg;
		if (extension == ".mp3")  return AudioFileFormat::MP3;
		
		return AudioFileFormat::None;
	}

	AudioSource Audio::LoadAudioSourceOgg(const std::string& filename)
	{
		FILE* f = fopen(filename.c_str(), "rb");

		OggVorbis_File vf;
		if (ov_open_callbacks(f, &vf, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0)
			std::cout << "Could not open ogg stream\n";

		// Useful info
		vorbis_info* vi = ov_info(&vf, -1);
		auto sampleRate = vi->rate;
		auto channels = vi->channels;
		uint64_t samples = ov_pcm_total(&vf, -1);
		float trackLength = (float)samples / (float)sampleRate; // in seconds
		uint32_t bufferSize = 2 * channels * samples; // 2 bytes per sample (I'm guessing...)

		std::cout << "File Info - " << filename << ":\n";
		std::cout << "  Channels: " << channels << std::endl;
		std::cout << "  Sample Rate: " << sampleRate << std::endl;
		std::cout << "  Expected size: " << bufferSize << std::endl;

		// TODO: Replace with Hazel::Buffer
		if (s_AudioScratchBufferSize < bufferSize)
		{
			s_AudioScratchBufferSize = bufferSize;
			delete[] s_AudioScratchBuffer;
			s_AudioScratchBuffer = new uint8_t[s_AudioScratchBufferSize];
		}

		uint8_t* oggBuffer = s_AudioScratchBuffer;
		uint8_t* bufferPtr = oggBuffer;
		int eof = 0;
		while (!eof)
		{
			int current_section;
			long length = ov_read(&vf, (char*)bufferPtr, 4096, 0, 2, 1, &current_section);
			bufferPtr += length;
			if (length == 0)
			{
				/* EOF */
				eof = 1;
			}
			else if (length < 0)
			{
				if (length == OV_EBADLINK)
				{
					fprintf(stderr, "Corrupt bitstream section! Exiting.\n");
					exit(1);
				}

				/* some other error in the stream.  Not a problem, just reporting it in
				   case we (the app) cares.  In this case, we don't. */
			}
		}

		uint32_t size = bufferPtr - oggBuffer;
		// assert bufferSize == size

		std::cout << "Read " << size << " bytes\n";

		// Release file
		ov_clear(&vf);
		fclose(f);

		ALuint buffer;
		alGenBuffers(1, &buffer);
		alBufferData(buffer, AL_FORMAT_STEREO16, oggBuffer, size, sampleRate);

		AudioSource result = { buffer, true, trackLength };
		alGenSources(1, &result.m_SourceHandle);
		alSourcei(result.m_SourceHandle, AL_BUFFER, buffer);

		if (alGetError() != AL_NO_ERROR)
			std::cout << "Failed to setup sound source" << std::endl;

		return result;
	}

	AudioSource Audio::LoadAudioSourceMP3(const std::string& filename)
	{
		mp3dec_file_info_t info;
		int loadResult = mp3dec_load(&s_Mp3d, filename.c_str(), &info, NULL, NULL);
		uint32_t size = info.samples * sizeof(mp3d_sample_t);

		ALuint buffer;
		alGenBuffers(1, &buffer);
		alBufferData(buffer, AL_FORMAT_STEREO16, info.buffer, size, 44100);

		float lengthSeconds = size / (info.avg_bitrate_kbps * 1024.0f);
		AudioSource result = { buffer, true, lengthSeconds };
		alGenSources(1, &result.m_SourceHandle);
		alSourcei(result.m_SourceHandle, AL_BUFFER, buffer);

		if (alGetError() != AL_NO_ERROR)
			std::cout << "Failed to setup sound source" << std::endl;

		return result;
	}

	void Audio::Init()
	{
		if (InitAL(nullptr, 0) != 0)
		{
			std::cout << "Audio device error!\n";
		}

		mp3dec_init(&s_Mp3d);

		s_AudioScratchBuffer = new uint8_t[s_AudioScratchBufferSize];

		// Init listener
		ALfloat listenerPos[] = { 0.0,0.0,0.0 };
		ALfloat listenerVel[] = { 0.0,0.0,0.0 };
		ALfloat listenerOri[] = { 0.0,0.0,-1.0, 0.0,1.0,0.0 };
		alListenerfv(AL_POSITION, listenerPos);
		alListenerfv(AL_VELOCITY, listenerVel);
		alListenerfv(AL_ORIENTATION, listenerOri);
	}

	AudioSource Audio::LoadAudioSource(const std::string& filename)
	{
		auto format = GetFileFormat(filename);
		switch (format)
		{
			case AudioFileFormat::Ogg:  return LoadAudioSourceOgg(filename);
			case AudioFileFormat::MP3:  return LoadAudioSourceMP3(filename);
		}

		// Loading failed or unsupported file type
		return { 0, false, 0 };
	}
	
	void Audio::Play(const AudioSource& audioSource)
	{
		// Play the sound until it finishes
		alSourcePlay(audioSource.m_SourceHandle);

		// TODO: current playback time and playback finished callback
		// eg.
		// ALfloat offset;
		// alGetSourcei(audioSource.m_SourceHandle, AL_SOURCE_STATE, &s_PlayState);
		// ALenum s_PlayState;
		// alGetSourcef(audioSource.m_SourceHandle, AL_SEC_OFFSET, &offset);
	}

	AudioSource::AudioSource(uint32_t handle, bool loaded, float length)
		: m_BufferHandle(handle), m_Loaded(loaded), m_TotalDuration(length)
	{
	}

	AudioSource::~AudioSource()
	{
		// TODO: free openal audio source?
	}

	void AudioSource::SetPosition(float x, float y, float z)
	{
		//alSource3f(source, AL_VELOCITY, 0, 0, 0);

		m_Position[0] = x;
		m_Position[1] = y;
		m_Position[2] = z;

		alSourcefv(m_SourceHandle, AL_POSITION, m_Position);
	}

	void AudioSource::SetGain(float gain)
	{
		m_Gain = gain;

		alSourcef(m_SourceHandle, AL_GAIN, gain);
	}

	void AudioSource::SetPitch(float pitch)
	{
		m_Pitch = pitch;

		alSourcef(m_SourceHandle, AL_PITCH, pitch);
	}

	void AudioSource::SetSpatial(bool spatial)
	{
		m_Spatial = spatial;

		alSourcei(m_SourceHandle, AL_SOURCE_SPATIALIZE_SOFT, spatial ? AL_TRUE : AL_FALSE);
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
	}

	void AudioSource::SetLoop(bool loop)
	{
		m_Loop = loop;

		alSourcei(m_SourceHandle, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
	}

	std::pair<uint32_t, uint32_t> AudioSource::GetLengthMinutesAndSeconds() const
	{
		return { (uint32_t)(m_TotalDuration / 60.0f), (uint32_t)m_TotalDuration % 60 };
	}

	AudioSource AudioSource::LoadFromFile(const std::string& file, bool spatial)
	{
		AudioSource result = Audio::LoadAudioSource(file);
		result.SetSpatial(spatial);
		return result;
	}

}
