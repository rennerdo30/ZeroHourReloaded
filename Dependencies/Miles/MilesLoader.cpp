/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "MilesLoader.h"
#include "mss/mss.h"

// mss32.dll exports decorated __stdcall names, which only exist in the 32 bit Windows build.
#if defined(_WIN32) && !defined(_WIN64)
#define MILES_LOADER_SUPPORTED 1
#else
#define MILES_LOADER_SUPPORTED 0
#endif

#if !MILES_LOADER_SUPPORTED && defined(ZHR_USE_RUST_AUDIO)
#define ZHR_RUST_AUDIO_BACKEND 1
#include "zhr_ffi.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>
#else
#define ZHR_RUST_AUDIO_BACKEND 0
#endif


namespace
{

#if ZHR_RUST_AUDIO_BACKEND

constexpr uintptr_t kListenerSentinel = 0x10000000u;
constexpr uintptr_t kProviderSentinel = 1u;

bool RustAudioLoaded = false;
DIG_DRIVER g_digDriver = {};
char g_providerName[] = "Miles Fast 2D Positional Audio";
char g_emptyError[] = "";

std::mutex g_audioMutex;
AIL_file_open_callback g_fileOpenCb = nullptr;
AIL_file_close_callback g_fileCloseCb = nullptr;
AIL_file_seek_callback g_fileSeekCb = nullptr;
AIL_file_read_callback g_fileReadCb = nullptr;

std::unordered_map<uint32_t, AIL_sample_callback> g_sampleEosCbs;
std::unordered_map<uint32_t, AIL_3dsample_callback> g_sample3dEosCbs;
std::unordered_map<uint32_t, AIL_stream_callback> g_streamEosCbs;

static uint32_t ptrToVoiceId(const void* h)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(h);
	if (v == 0 || v == kListenerSentinel)
		return 0;
	return static_cast<uint32_t>(v);
}

static size_t detectRiffSize(const void* data)
{
	if (!data)
		return 0;
	const uint8_t* p = static_cast<const uint8_t*>(data);
	if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F')
	{
		uint32_t sz = 0;
		std::memcpy(&sz, p + 4, 4);
		if (sz >= 12 && sz < 256u * 1024u * 1024u)
			return static_cast<size_t>(sz) + 8u;
	}
	return 0;
}

static const char* extensionHintFromName(const char* name)
{
	if (!name)
		return nullptr;
	const char* dot = std::strrchr(name, '.');
	if (!dot || dot[1] == '\0')
		return nullptr;
	return dot + 1;
}

static bool readStreamFileBytes(const char* filename, std::vector<uint8_t>& out)
{
	out.clear();
	if (!filename || filename[0] == '\0')
		return false;

	AIL_file_open_callback openCb = nullptr;
	AIL_file_close_callback closeCb = nullptr;
	AIL_file_seek_callback seekCb = nullptr;
	AIL_file_read_callback readCb = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_audioMutex);
		openCb = g_fileOpenCb;
		closeCb = g_fileCloseCb;
		seekCb = g_fileSeekCb;
		readCb = g_fileReadCb;
	}

	if (openCb && closeCb && seekCb && readCb)
	{
		void* handle = nullptr;
		if (openCb(filename, &handle) != 0 && handle != nullptr)
		{
			S32 len = seekCb(handle, 0, AIL_FILE_SEEK_END);
			seekCb(handle, 0, AIL_FILE_SEEK_BEGIN);
			if (len > 0 && len < 256 * 1024 * 1024)
			{
				out.resize(static_cast<size_t>(len));
				U32 got = readCb(handle, out.data(), static_cast<U32>(len));
				out.resize(got);
			}
			closeCb(handle);
			return !out.empty();
		}
	}

	std::ifstream f(filename, std::ios::binary | std::ios::ate);
	if (!f)
		return false;
	std::streamsize sz = f.tellg();
	if (sz <= 0 || sz > 256 * 1024 * 1024)
		return false;
	f.seekg(0, std::ios::beg);
	out.resize(static_cast<size_t>(sz));
	if (!f.read(reinterpret_cast<char*>(out.data()), sz))
	{
		out.clear();
		return false;
	}
	return true;
}

static void flushCompletedCallbacks()
{
	uint32_t voiceId = 0;
	uint32_t kind = 0;
	while (zhr_audio_pop_completed(&voiceId, &kind) != 0)
	{
		AIL_sample_callback cb2d = nullptr;
		AIL_3dsample_callback cb3d = nullptr;
		AIL_stream_callback cbStream = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_audioMutex);
			if (kind == 0)
			{
				auto it = g_sampleEosCbs.find(voiceId);
				if (it != g_sampleEosCbs.end())
					cb2d = it->second;
			}
			else if (kind == 1)
			{
				auto it = g_sample3dEosCbs.find(voiceId);
				if (it != g_sample3dEosCbs.end())
					cb3d = it->second;
			}
			else if (kind == 2)
			{
				auto it = g_streamEosCbs.find(voiceId);
				if (it != g_streamEosCbs.end())
					cbStream = it->second;
			}
		}

		if (cb2d)
			cb2d(reinterpret_cast<HSAMPLE>(static_cast<uintptr_t>(voiceId)));
		else if (cb3d)
			cb3d(reinterpret_cast<H3DSAMPLE>(static_cast<uintptr_t>(voiceId)));
		else if (cbStream)
			cbStream(reinterpret_cast<HSTREAM>(static_cast<uintptr_t>(voiceId)));
	}
}

#endif // ZHR_RUST_AUDIO_BACKEND

typedef F32 (__stdcall *AIL_3D_sample_volume_t)(H3DSAMPLE sample);
typedef void (__stdcall *AIL_set_3D_sample_volume_t)(H3DSAMPLE sample, F32 volume);
typedef void (__stdcall *AIL_end_3D_sample_t)(H3DSAMPLE sample);
typedef void (__stdcall *AIL_resume_3D_sample_t)(H3DSAMPLE sample);
typedef void (__stdcall *AIL_stop_3D_sample_t)(H3DSAMPLE sample);
typedef void (__stdcall *AIL_start_3D_sample_t)(H3DSAMPLE sample);
typedef U32 (__stdcall *AIL_3D_sample_loop_count_t)(H3DSAMPLE sample);
typedef void (__stdcall *AIL_set_3D_sample_offset_t)(H3DSAMPLE sample, U32 offset);
typedef U32 (__stdcall *AIL_3D_sample_length_t)(H3DSAMPLE sample);
typedef U32 (__stdcall *AIL_3D_sample_offset_t)(H3DSAMPLE sample);
typedef S32 (__stdcall *AIL_3D_sample_playback_rate_t)(H3DSAMPLE sample);
typedef void (__stdcall *AIL_set_3D_sample_playback_rate_t)(H3DSAMPLE sample, S32 playback_rate);
typedef S32 (__stdcall *AIL_set_3D_sample_file_t)(H3DSAMPLE sample, const void* file_image);
typedef HPROVIDER (__stdcall *AIL_set_sample_processor_t)(HSAMPLE sample, SAMPLESTAGE pipeline_stage, HPROVIDER provider);
typedef void (__stdcall *AIL_set_filter_sample_preference_t)(HSAMPLE sample, const char* name, const void* val);
typedef void (__stdcall *AIL_release_sample_handle_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_close_3D_provider_t)(HPROVIDER lib);
typedef S32 (__stdcall *AIL_set_preference_t)(U32 number, S32 value);
typedef S32 (__stdcall *AIL_waveOutOpen_t)(HDIGDRIVER* driver, LPHWAVEOUT* waveout, S32 id, LPWAVEFORMAT format);
typedef void (__stdcall *AIL_waveOutClose_t)(HDIGDRIVER driver);
typedef void (__stdcall *AIL_set_3D_sample_loop_count_t)(H3DSAMPLE sample, U32 count);
typedef void (__stdcall *AIL_set_stream_playback_rate_t)(HSTREAM stream, S32 rate);
typedef S32 (__stdcall *AIL_stream_playback_rate_t)(HSTREAM stream);
typedef void (__stdcall *AIL_stream_ms_position_t)(HSTREAM sample, S32* total_milliseconds, S32* current_milliseconds);
typedef void (__stdcall *AIL_set_stream_ms_position_t)(HSTREAM stream, S32 pos);
typedef S32 (__stdcall *AIL_stream_loop_count_t)(HSTREAM stream);
typedef void (__stdcall *AIL_set_stream_loop_block_t)(HSTREAM stream, S32 loop_start, S32 loop_end);
typedef void (__stdcall *AIL_set_stream_loop_count_t)(HSTREAM stream, S32 count);
typedef void (__stdcall *AIL_close_stream_t)(HSTREAM stream);
typedef void (__stdcall *AIL_pause_stream_t)(HSTREAM stream, S32 onoff);
typedef AIL_stream_callback (__stdcall *AIL_register_stream_callback_t)(HSTREAM stream, AIL_stream_callback callback);
typedef AIL_3dsample_callback (__stdcall *AIL_register_3D_EOS_callback_t)(H3DSAMPLE sample, AIL_3dsample_callback EOS);
typedef AIL_sample_callback (__stdcall *AIL_register_EOS_callback_t)(HSAMPLE sample, AIL_sample_callback EOS);
typedef void (__stdcall *AIL_start_stream_t)(HSTREAM stream);
typedef void (__stdcall *AIL_set_sample_playback_rate_t)(HSAMPLE sample, S32 playback_rate);
typedef S32 (__stdcall *AIL_sample_playback_rate_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_sample_ms_position_t)(HSAMPLE sample, S32* total_ms, S32* current_ms);
typedef void (__stdcall *AIL_set_sample_ms_position_t)(HSAMPLE sample, S32 pos);
typedef S32 (__stdcall *AIL_sample_loop_count_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_set_sample_loop_count_t)(HSAMPLE sample, S32 count);
typedef void (__stdcall *AIL_end_sample_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_resume_sample_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_stop_sample_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_start_sample_t)(HSAMPLE sample);
typedef void (__stdcall *AIL_init_sample_t)(HSAMPLE sample);
typedef S32 (__stdcall *AIL_set_named_sample_file_t)(HSAMPLE sample, const char* file_name, const void* file_image, S32 file_size, S32 block);
typedef void (__stdcall *AIL_set_3D_sample_effects_level_t)(H3DSAMPLE sample, F32 effect_level);
typedef void (__stdcall *AIL_set_3D_sample_distances_t)(H3DSAMPLE sample, F32 max_dist, F32 min_dist);
typedef void (__stdcall *AIL_set_3D_velocity_vector_t)(H3DPOBJECT obj, F32 x, F32 y, F32 z);
typedef void (__stdcall *AIL_set_3D_position_t)(H3DPOBJECT obj, F32 X, F32 Y, F32 Z);
typedef void (__stdcall *AIL_set_3D_orientation_t)(H3DPOBJECT obj, F32 X_face, F32 Y_face, F32 Z_face, F32 X_up, F32 Y_up, F32 Z_up);
typedef S32 (__stdcall *AIL_WAV_info_t)(const void* data, AILSOUNDINFO* info);
typedef void (__stdcall *AIL_stop_timer_t)(HTIMER timer);
typedef void (__stdcall *AIL_release_timer_handle_t)(HTIMER timer);
typedef void (__stdcall *AIL_shutdown_t)(void);
typedef S32 (__stdcall *AIL_enumerate_filters_t)(HPROENUM* next, HPROVIDER* dest, char** name);
typedef void (__stdcall *AIL_set_file_callbacks_t)(AIL_file_open_callback opencb, AIL_file_close_callback closecb, AIL_file_seek_callback seekcb, AIL_file_read_callback readcb);
typedef void (__stdcall *AIL_release_3D_sample_handle_t)(H3DSAMPLE sample);
typedef H3DSAMPLE (__stdcall *AIL_allocate_3D_sample_handle_t)(HPROVIDER lib);
typedef void (__stdcall *AIL_set_3D_user_data_t)(H3DPOBJECT obj, U32 index, S32 value);
typedef void (__stdcall *AIL_unlock_t)(void);
typedef void (__stdcall *AIL_unlock_mutex_t)(void);
typedef void (__stdcall *AIL_lock_t)(void);
typedef void (__stdcall *AIL_lock_mutex_t)(void);
typedef void (__stdcall *AIL_set_3D_speaker_type_t)(HPROVIDER lib, S32 speaker_type);
typedef void (__stdcall *AIL_close_3D_listener_t)(H3DPOBJECT listener);
typedef S32 (__stdcall *AIL_enumerate_3D_providers_t)(HPROENUM* next, HPROVIDER* dest, char** name);
typedef M3DRESULT (__stdcall *AIL_open_3D_provider_t)(HPROVIDER lib);
typedef char* (__stdcall *AIL_last_error_t)(void);
typedef H3DPOBJECT (__stdcall *AIL_open_3D_listener_t)(HPROVIDER lib);
typedef S32 (__stdcall *AIL_3D_user_data_t)(H3DPOBJECT obj, U32 index);
typedef S32 (__stdcall *AIL_sample_user_data_t)(HSAMPLE sample, U32 index);
typedef HSAMPLE (__stdcall *AIL_allocate_sample_handle_t)(HDIGDRIVER dig);
typedef void (__stdcall *AIL_set_sample_user_data_t)(HSAMPLE sample, U32 index, S32 value);
typedef S32 (__stdcall *AIL_decompress_ADPCM_t)(const AILSOUNDINFO *info, void **outdata, U32 *outsize);
typedef void (__stdcall *AIL_get_DirectSound_info_t)(HSAMPLE sample, AILLPDIRECTSOUND *lplpDS, AILLPDIRECTSOUNDBUFFER *lplpDSB);
typedef void (__stdcall *AIL_mem_free_lock_t)(void *ptr);
typedef HSTREAM (__stdcall *AIL_open_stream_t)(HDIGDRIVER dig, const char *filename, S32 stream_mem);
typedef S32 (__stdcall *AIL_startup_t)(void);
typedef void (__stdcall *AIL_quick_unload_t)(HAUDIO audio);
typedef HAUDIO (__stdcall *AIL_quick_load_and_play_t)(const char *filename, U32 loop_count, S32 wait_request);
typedef void (__stdcall *AIL_quick_set_volume_t)(HAUDIO audio, F32 volume, F32 extravol);
typedef S32 (__stdcall *AIL_quick_startup_t)(S32 use_digital, S32 use_MIDI, U32 output_rate, S32 output_bits, S32 output_channels);
typedef void (__stdcall *AIL_quick_handles_t)(HDIGDRIVER *pdig, HMDIDRIVER *pmdi, HDLSDEVICE *pdls);
typedef void (__stdcall *AIL_sample_volume_pan_t)(HSAMPLE sample, F32 *volume, F32 *pan);
typedef void (__stdcall *AIL_set_3D_sample_occlusion_t)(H3DSAMPLE sample, F32 occlusion);
typedef char * (__stdcall *AIL_set_redist_directory_t)(const char *dir);
typedef S32 (__stdcall *AIL_set_sample_file_t)(HSAMPLE sample, const void *file_image, S32 block);
typedef void (__stdcall *AIL_set_sample_volume_pan_t)(HSAMPLE sample, F32 volume, F32 pan);
typedef void (__stdcall *AIL_set_stream_volume_pan_t)(HSTREAM stream, F32 volume, F32 pan);
typedef void (__stdcall *AIL_stream_volume_pan_t)(HSTREAM stream, F32 *volume, F32 *pan);
typedef U32 (__stdcall *AIL_get_timer_highest_delay_t)(void);

AIL_3D_sample_volume_t AIL_3D_sample_volumePtr = nullptr;
AIL_set_3D_sample_volume_t AIL_set_3D_sample_volumePtr = nullptr;
AIL_end_3D_sample_t AIL_end_3D_samplePtr = nullptr;
AIL_resume_3D_sample_t AIL_resume_3D_samplePtr = nullptr;
AIL_stop_3D_sample_t AIL_stop_3D_samplePtr = nullptr;
AIL_start_3D_sample_t AIL_start_3D_samplePtr = nullptr;
AIL_3D_sample_loop_count_t AIL_3D_sample_loop_countPtr = nullptr;
AIL_set_3D_sample_offset_t AIL_set_3D_sample_offsetPtr = nullptr;
AIL_3D_sample_length_t AIL_3D_sample_lengthPtr = nullptr;
AIL_3D_sample_offset_t AIL_3D_sample_offsetPtr = nullptr;
AIL_3D_sample_playback_rate_t AIL_3D_sample_playback_ratePtr = nullptr;
AIL_set_3D_sample_playback_rate_t AIL_set_3D_sample_playback_ratePtr = nullptr;
AIL_set_3D_sample_file_t AIL_set_3D_sample_filePtr = nullptr;
AIL_set_sample_processor_t AIL_set_sample_processorPtr = nullptr;
AIL_set_filter_sample_preference_t AIL_set_filter_sample_preferencePtr = nullptr;
AIL_release_sample_handle_t AIL_release_sample_handlePtr = nullptr;
AIL_close_3D_provider_t AIL_close_3D_providerPtr = nullptr;
AIL_set_preference_t AIL_set_preferencePtr = nullptr;
AIL_waveOutOpen_t AIL_waveOutOpenPtr = nullptr;
AIL_waveOutClose_t AIL_waveOutClosePtr = nullptr;
AIL_set_3D_sample_loop_count_t AIL_set_3D_sample_loop_countPtr = nullptr;
AIL_set_stream_playback_rate_t AIL_set_stream_playback_ratePtr = nullptr;
AIL_stream_playback_rate_t AIL_stream_playback_ratePtr = nullptr;
AIL_stream_ms_position_t AIL_stream_ms_positionPtr = nullptr;
AIL_set_stream_ms_position_t AIL_set_stream_ms_positionPtr = nullptr;
AIL_stream_loop_count_t AIL_stream_loop_countPtr = nullptr;
AIL_set_stream_loop_block_t AIL_set_stream_loop_blockPtr = nullptr;
AIL_set_stream_loop_count_t AIL_set_stream_loop_countPtr = nullptr;
AIL_close_stream_t AIL_close_streamPtr = nullptr;
AIL_pause_stream_t AIL_pause_streamPtr = nullptr;
AIL_register_stream_callback_t AIL_register_stream_callbackPtr = nullptr;
AIL_register_3D_EOS_callback_t AIL_register_3D_EOS_callbackPtr = nullptr;
AIL_register_EOS_callback_t AIL_register_EOS_callbackPtr = nullptr;
AIL_start_stream_t AIL_start_streamPtr = nullptr;
AIL_set_sample_playback_rate_t AIL_set_sample_playback_ratePtr = nullptr;
AIL_sample_playback_rate_t AIL_sample_playback_ratePtr = nullptr;
AIL_sample_ms_position_t AIL_sample_ms_positionPtr = nullptr;
AIL_set_sample_ms_position_t AIL_set_sample_ms_positionPtr = nullptr;
AIL_sample_loop_count_t AIL_sample_loop_countPtr = nullptr;
AIL_set_sample_loop_count_t AIL_set_sample_loop_countPtr = nullptr;
AIL_end_sample_t AIL_end_samplePtr = nullptr;
AIL_resume_sample_t AIL_resume_samplePtr = nullptr;
AIL_stop_sample_t AIL_stop_samplePtr = nullptr;
AIL_start_sample_t AIL_start_samplePtr = nullptr;
AIL_init_sample_t AIL_init_samplePtr = nullptr;
AIL_set_named_sample_file_t AIL_set_named_sample_filePtr = nullptr;
AIL_set_3D_sample_effects_level_t AIL_set_3D_sample_effects_levelPtr = nullptr;
AIL_set_3D_sample_distances_t AIL_set_3D_sample_distancesPtr = nullptr;
AIL_set_3D_velocity_vector_t AIL_set_3D_velocity_vectorPtr = nullptr;
AIL_set_3D_position_t AIL_set_3D_positionPtr = nullptr;
AIL_set_3D_orientation_t AIL_set_3D_orientationPtr = nullptr;
AIL_WAV_info_t AIL_WAV_infoPtr = nullptr;
AIL_stop_timer_t AIL_stop_timerPtr = nullptr;
AIL_release_timer_handle_t AIL_release_timer_handlePtr = nullptr;
AIL_shutdown_t AIL_shutdownPtr = nullptr;
AIL_enumerate_filters_t AIL_enumerate_filtersPtr = nullptr;
AIL_set_file_callbacks_t AIL_set_file_callbacksPtr = nullptr;
AIL_release_3D_sample_handle_t AIL_release_3D_sample_handlePtr = nullptr;
AIL_allocate_3D_sample_handle_t AIL_allocate_3D_sample_handlePtr = nullptr;
AIL_set_3D_user_data_t AIL_set_3D_user_dataPtr = nullptr;
AIL_unlock_t AIL_unlockPtr = nullptr;
AIL_unlock_mutex_t AIL_unlock_mutexPtr = nullptr;
AIL_lock_t AIL_lockPtr = nullptr;
AIL_lock_mutex_t AIL_lock_mutexPtr = nullptr;
AIL_set_3D_speaker_type_t AIL_set_3D_speaker_typePtr = nullptr;
AIL_close_3D_listener_t AIL_close_3D_listenerPtr = nullptr;
AIL_enumerate_3D_providers_t AIL_enumerate_3D_providersPtr = nullptr;
AIL_open_3D_provider_t AIL_open_3D_providerPtr = nullptr;
AIL_last_error_t AIL_last_errorPtr = nullptr;
AIL_open_3D_listener_t AIL_open_3D_listenerPtr = nullptr;
AIL_3D_user_data_t AIL_3D_user_dataPtr = nullptr;
AIL_sample_user_data_t AIL_sample_user_dataPtr = nullptr;
AIL_allocate_sample_handle_t AIL_allocate_sample_handlePtr = nullptr;
AIL_set_sample_user_data_t AIL_set_sample_user_dataPtr = nullptr;
AIL_decompress_ADPCM_t AIL_decompress_ADPCMPtr = nullptr;
AIL_get_DirectSound_info_t AIL_get_DirectSound_infoPtr = nullptr;
AIL_mem_free_lock_t AIL_mem_free_lockPtr = nullptr;
AIL_open_stream_t AIL_open_streamPtr = nullptr;
AIL_startup_t AIL_startupPtr = nullptr;
AIL_quick_unload_t AIL_quick_unloadPtr = nullptr;
AIL_quick_load_and_play_t AIL_quick_load_and_playPtr = nullptr;
AIL_quick_set_volume_t AIL_quick_set_volumePtr = nullptr;
AIL_quick_startup_t AIL_quick_startupPtr = nullptr;
AIL_quick_handles_t AIL_quick_handlesPtr = nullptr;
AIL_sample_volume_pan_t AIL_sample_volume_panPtr = nullptr;
AIL_set_3D_sample_occlusion_t AIL_set_3D_sample_occlusionPtr = nullptr;
AIL_set_redist_directory_t AIL_set_redist_directoryPtr = nullptr;
AIL_set_sample_file_t AIL_set_sample_filePtr = nullptr;
AIL_set_sample_volume_pan_t AIL_set_sample_volume_panPtr = nullptr;
AIL_set_stream_volume_pan_t AIL_set_stream_volume_panPtr = nullptr;
AIL_stream_volume_pan_t AIL_stream_volume_panPtr = nullptr;
AIL_get_timer_highest_delay_t AIL_get_timer_highest_delayPtr = nullptr;

int ReferenceCount = 0;
bool Failed = false;
unsigned long LastError = 0;

#if MILES_LOADER_SUPPORTED
HMODULE Module = HMODULE(nullptr);

// Resolves one export into its matching function pointer. The exported names are decorated, so
// they carry the __stdcall argument byte count and must be spelled out verbatim.
#define MILES_RESOLVE(name, decorated) \
	name##Ptr = reinterpret_cast<name##_t>(::GetProcAddress(Module, decorated))

static void resolveAll()
{
	MILES_RESOLVE(AIL_3D_sample_volume, "_AIL_3D_sample_volume@4");
	MILES_RESOLVE(AIL_set_3D_sample_volume, "_AIL_set_3D_sample_volume@8");
	MILES_RESOLVE(AIL_end_3D_sample, "_AIL_end_3D_sample@4");
	MILES_RESOLVE(AIL_resume_3D_sample, "_AIL_resume_3D_sample@4");
	MILES_RESOLVE(AIL_stop_3D_sample, "_AIL_stop_3D_sample@4");
	MILES_RESOLVE(AIL_start_3D_sample, "_AIL_start_3D_sample@4");
	MILES_RESOLVE(AIL_3D_sample_loop_count, "_AIL_3D_sample_loop_count@4");
	MILES_RESOLVE(AIL_set_3D_sample_offset, "_AIL_set_3D_sample_offset@8");
	MILES_RESOLVE(AIL_3D_sample_length, "_AIL_3D_sample_length@4");
	MILES_RESOLVE(AIL_3D_sample_offset, "_AIL_3D_sample_offset@4");
	MILES_RESOLVE(AIL_3D_sample_playback_rate, "_AIL_3D_sample_playback_rate@4");
	MILES_RESOLVE(AIL_set_3D_sample_playback_rate, "_AIL_set_3D_sample_playback_rate@8");
	MILES_RESOLVE(AIL_set_3D_sample_file, "_AIL_set_3D_sample_file@8");
	MILES_RESOLVE(AIL_set_sample_processor, "_AIL_set_sample_processor@12");
	MILES_RESOLVE(AIL_set_filter_sample_preference, "_AIL_set_filter_sample_preference@12");
	MILES_RESOLVE(AIL_release_sample_handle, "_AIL_release_sample_handle@4");
	MILES_RESOLVE(AIL_close_3D_provider, "_AIL_close_3D_provider@4");
	MILES_RESOLVE(AIL_set_preference, "_AIL_set_preference@8");
	MILES_RESOLVE(AIL_waveOutOpen, "_AIL_waveOutOpen@16");
	MILES_RESOLVE(AIL_waveOutClose, "_AIL_waveOutClose@4");
	MILES_RESOLVE(AIL_set_3D_sample_loop_count, "_AIL_set_3D_sample_loop_count@8");
	MILES_RESOLVE(AIL_set_stream_playback_rate, "_AIL_set_stream_playback_rate@8");
	MILES_RESOLVE(AIL_stream_playback_rate, "_AIL_stream_playback_rate@4");
	MILES_RESOLVE(AIL_stream_ms_position, "_AIL_stream_ms_position@12");
	MILES_RESOLVE(AIL_set_stream_ms_position, "_AIL_set_stream_ms_position@8");
	MILES_RESOLVE(AIL_stream_loop_count, "_AIL_stream_loop_count@4");
	MILES_RESOLVE(AIL_set_stream_loop_block, "_AIL_set_stream_loop_block@12");
	MILES_RESOLVE(AIL_set_stream_loop_count, "_AIL_set_stream_loop_count@8");
	MILES_RESOLVE(AIL_close_stream, "_AIL_close_stream@4");
	MILES_RESOLVE(AIL_pause_stream, "_AIL_pause_stream@8");
	MILES_RESOLVE(AIL_register_stream_callback, "_AIL_register_stream_callback@8");
	MILES_RESOLVE(AIL_register_3D_EOS_callback, "_AIL_register_3D_EOS_callback@8");
	MILES_RESOLVE(AIL_register_EOS_callback, "_AIL_register_EOS_callback@8");
	MILES_RESOLVE(AIL_start_stream, "_AIL_start_stream@4");
	MILES_RESOLVE(AIL_set_sample_playback_rate, "_AIL_set_sample_playback_rate@8");
	MILES_RESOLVE(AIL_sample_playback_rate, "_AIL_sample_playback_rate@4");
	MILES_RESOLVE(AIL_sample_ms_position, "_AIL_sample_ms_position@12");
	MILES_RESOLVE(AIL_set_sample_ms_position, "_AIL_set_sample_ms_position@8");
	MILES_RESOLVE(AIL_sample_loop_count, "_AIL_sample_loop_count@4");
	MILES_RESOLVE(AIL_set_sample_loop_count, "_AIL_set_sample_loop_count@8");
	MILES_RESOLVE(AIL_end_sample, "_AIL_end_sample@4");
	MILES_RESOLVE(AIL_resume_sample, "_AIL_resume_sample@4");
	MILES_RESOLVE(AIL_stop_sample, "_AIL_stop_sample@4");
	MILES_RESOLVE(AIL_start_sample, "_AIL_start_sample@4");
	MILES_RESOLVE(AIL_init_sample, "_AIL_init_sample@4");
	MILES_RESOLVE(AIL_set_named_sample_file, "_AIL_set_named_sample_file@20");
	MILES_RESOLVE(AIL_set_3D_sample_effects_level, "_AIL_set_3D_sample_effects_level@8");
	MILES_RESOLVE(AIL_set_3D_sample_distances, "_AIL_set_3D_sample_distances@12");
	MILES_RESOLVE(AIL_set_3D_velocity_vector, "_AIL_set_3D_velocity_vector@16");
	MILES_RESOLVE(AIL_set_3D_position, "_AIL_set_3D_position@16");
	MILES_RESOLVE(AIL_set_3D_orientation, "_AIL_set_3D_orientation@28");
	MILES_RESOLVE(AIL_WAV_info, "_AIL_WAV_info@8");
	MILES_RESOLVE(AIL_stop_timer, "_AIL_stop_timer@4");
	MILES_RESOLVE(AIL_release_timer_handle, "_AIL_release_timer_handle@4");
	MILES_RESOLVE(AIL_shutdown, "_AIL_shutdown@0");
	MILES_RESOLVE(AIL_enumerate_filters, "_AIL_enumerate_filters@12");
	MILES_RESOLVE(AIL_set_file_callbacks, "_AIL_set_file_callbacks@16");
	MILES_RESOLVE(AIL_release_3D_sample_handle, "_AIL_release_3D_sample_handle@4");
	MILES_RESOLVE(AIL_allocate_3D_sample_handle, "_AIL_allocate_3D_sample_handle@4");
	MILES_RESOLVE(AIL_set_3D_user_data, "_AIL_set_3D_user_data@12");
	MILES_RESOLVE(AIL_unlock, "_AIL_unlock@0");
	MILES_RESOLVE(AIL_unlock_mutex, "_AIL_unlock_mutex@0");
	MILES_RESOLVE(AIL_lock, "_AIL_lock@0");
	MILES_RESOLVE(AIL_lock_mutex, "_AIL_lock_mutex@0");
	MILES_RESOLVE(AIL_set_3D_speaker_type, "_AIL_set_3D_speaker_type@8");
	MILES_RESOLVE(AIL_close_3D_listener, "_AIL_close_3D_listener@4");
	MILES_RESOLVE(AIL_enumerate_3D_providers, "_AIL_enumerate_3D_providers@12");
	MILES_RESOLVE(AIL_open_3D_provider, "_AIL_open_3D_provider@4");
	MILES_RESOLVE(AIL_last_error, "_AIL_last_error@0");
	MILES_RESOLVE(AIL_open_3D_listener, "_AIL_open_3D_listener@4");
	MILES_RESOLVE(AIL_3D_user_data, "_AIL_3D_user_data@8");
	MILES_RESOLVE(AIL_sample_user_data, "_AIL_sample_user_data@8");
	MILES_RESOLVE(AIL_allocate_sample_handle, "_AIL_allocate_sample_handle@4");
	MILES_RESOLVE(AIL_set_sample_user_data, "_AIL_set_sample_user_data@12");
	MILES_RESOLVE(AIL_decompress_ADPCM, "_AIL_decompress_ADPCM@12");
	MILES_RESOLVE(AIL_get_DirectSound_info, "_AIL_get_DirectSound_info@12");
	MILES_RESOLVE(AIL_mem_free_lock, "_AIL_mem_free_lock@4");
	MILES_RESOLVE(AIL_open_stream, "_AIL_open_stream@12");
	MILES_RESOLVE(AIL_startup, "_AIL_startup@0");
	MILES_RESOLVE(AIL_quick_unload, "_AIL_quick_unload@4");
	MILES_RESOLVE(AIL_quick_load_and_play, "_AIL_quick_load_and_play@12");
	MILES_RESOLVE(AIL_quick_set_volume, "_AIL_quick_set_volume@12");
	MILES_RESOLVE(AIL_quick_startup, "_AIL_quick_startup@20");
	MILES_RESOLVE(AIL_quick_handles, "_AIL_quick_handles@12");
	MILES_RESOLVE(AIL_sample_volume_pan, "_AIL_sample_volume_pan@12");
	MILES_RESOLVE(AIL_set_3D_sample_occlusion, "_AIL_set_3D_sample_occlusion@8");
	MILES_RESOLVE(AIL_set_redist_directory, "_AIL_set_redist_directory@4");
	MILES_RESOLVE(AIL_set_sample_file, "_AIL_set_sample_file@12");
	MILES_RESOLVE(AIL_set_sample_volume_pan, "_AIL_set_sample_volume_pan@12");
	MILES_RESOLVE(AIL_set_stream_volume_pan, "_AIL_set_stream_volume_pan@12");
	MILES_RESOLVE(AIL_stream_volume_pan, "_AIL_stream_volume_pan@12");
	MILES_RESOLVE(AIL_get_timer_highest_delay, "_AIL_get_timer_highest_delay@0");
}
#undef MILES_RESOLVE

static void freeResources()
{
	if (Module != HMODULE(nullptr)) {
		::FreeLibrary(Module);
		Module = HMODULE(nullptr);
	}

	AIL_3D_sample_volumePtr = nullptr;
	AIL_set_3D_sample_volumePtr = nullptr;
	AIL_end_3D_samplePtr = nullptr;
	AIL_resume_3D_samplePtr = nullptr;
	AIL_stop_3D_samplePtr = nullptr;
	AIL_start_3D_samplePtr = nullptr;
	AIL_3D_sample_loop_countPtr = nullptr;
	AIL_set_3D_sample_offsetPtr = nullptr;
	AIL_3D_sample_lengthPtr = nullptr;
	AIL_3D_sample_offsetPtr = nullptr;
	AIL_3D_sample_playback_ratePtr = nullptr;
	AIL_set_3D_sample_playback_ratePtr = nullptr;
	AIL_set_3D_sample_filePtr = nullptr;
	AIL_set_sample_processorPtr = nullptr;
	AIL_set_filter_sample_preferencePtr = nullptr;
	AIL_release_sample_handlePtr = nullptr;
	AIL_close_3D_providerPtr = nullptr;
	AIL_set_preferencePtr = nullptr;
	AIL_waveOutOpenPtr = nullptr;
	AIL_waveOutClosePtr = nullptr;
	AIL_set_3D_sample_loop_countPtr = nullptr;
	AIL_set_stream_playback_ratePtr = nullptr;
	AIL_stream_playback_ratePtr = nullptr;
	AIL_stream_ms_positionPtr = nullptr;
	AIL_set_stream_ms_positionPtr = nullptr;
	AIL_stream_loop_countPtr = nullptr;
	AIL_set_stream_loop_blockPtr = nullptr;
	AIL_set_stream_loop_countPtr = nullptr;
	AIL_close_streamPtr = nullptr;
	AIL_pause_streamPtr = nullptr;
	AIL_register_stream_callbackPtr = nullptr;
	AIL_register_3D_EOS_callbackPtr = nullptr;
	AIL_register_EOS_callbackPtr = nullptr;
	AIL_start_streamPtr = nullptr;
	AIL_set_sample_playback_ratePtr = nullptr;
	AIL_sample_playback_ratePtr = nullptr;
	AIL_sample_ms_positionPtr = nullptr;
	AIL_set_sample_ms_positionPtr = nullptr;
	AIL_sample_loop_countPtr = nullptr;
	AIL_set_sample_loop_countPtr = nullptr;
	AIL_end_samplePtr = nullptr;
	AIL_resume_samplePtr = nullptr;
	AIL_stop_samplePtr = nullptr;
	AIL_start_samplePtr = nullptr;
	AIL_init_samplePtr = nullptr;
	AIL_set_named_sample_filePtr = nullptr;
	AIL_set_3D_sample_effects_levelPtr = nullptr;
	AIL_set_3D_sample_distancesPtr = nullptr;
	AIL_set_3D_velocity_vectorPtr = nullptr;
	AIL_set_3D_positionPtr = nullptr;
	AIL_set_3D_orientationPtr = nullptr;
	AIL_WAV_infoPtr = nullptr;
	AIL_stop_timerPtr = nullptr;
	AIL_release_timer_handlePtr = nullptr;
	AIL_shutdownPtr = nullptr;
	AIL_enumerate_filtersPtr = nullptr;
	AIL_set_file_callbacksPtr = nullptr;
	AIL_release_3D_sample_handlePtr = nullptr;
	AIL_allocate_3D_sample_handlePtr = nullptr;
	AIL_set_3D_user_dataPtr = nullptr;
	AIL_unlockPtr = nullptr;
	AIL_unlock_mutexPtr = nullptr;
	AIL_lockPtr = nullptr;
	AIL_lock_mutexPtr = nullptr;
	AIL_set_3D_speaker_typePtr = nullptr;
	AIL_close_3D_listenerPtr = nullptr;
	AIL_enumerate_3D_providersPtr = nullptr;
	AIL_open_3D_providerPtr = nullptr;
	AIL_last_errorPtr = nullptr;
	AIL_open_3D_listenerPtr = nullptr;
	AIL_3D_user_dataPtr = nullptr;
	AIL_sample_user_dataPtr = nullptr;
	AIL_allocate_sample_handlePtr = nullptr;
	AIL_set_sample_user_dataPtr = nullptr;
	AIL_decompress_ADPCMPtr = nullptr;
	AIL_get_DirectSound_infoPtr = nullptr;
	AIL_mem_free_lockPtr = nullptr;
	AIL_open_streamPtr = nullptr;
	AIL_startupPtr = nullptr;
	AIL_quick_unloadPtr = nullptr;
	AIL_quick_load_and_playPtr = nullptr;
	AIL_quick_set_volumePtr = nullptr;
	AIL_quick_startupPtr = nullptr;
	AIL_quick_handlesPtr = nullptr;
	AIL_sample_volume_panPtr = nullptr;
	AIL_set_3D_sample_occlusionPtr = nullptr;
	AIL_set_redist_directoryPtr = nullptr;
	AIL_set_sample_filePtr = nullptr;
	AIL_set_sample_volume_panPtr = nullptr;
	AIL_set_stream_volume_panPtr = nullptr;
	AIL_stream_volume_panPtr = nullptr;
	AIL_get_timer_highest_delayPtr = nullptr;
}
#endif // MILES_LOADER_SUPPORTED

} // namespace


bool MilesLoader::isLoaded()
{
#if MILES_LOADER_SUPPORTED
	return Module != HMODULE(nullptr);
#elif ZHR_RUST_AUDIO_BACKEND
	return RustAudioLoaded;
#else
	return false;
#endif
}


bool MilesLoader::isFailed()
{
	return Failed;
}


unsigned long MilesLoader::getLastError()
{
	return LastError;
}


bool MilesLoader::load()
{
	// Always increment the reference count.
	++ReferenceCount;

	// Optimization: return early if it failed before.
	if (Failed)
		return false;

	// Return early if someone else already loaded it.
	if (ReferenceCount > 1)
		return true;

#if MILES_LOADER_SUPPORTED
	// Load mss32.dll by name, so that the usual module search order applies.
	Module = ::LoadLibraryA("mss32.dll");
	if (Module == HMODULE(nullptr)) {
		LastError = ::GetLastError();
		Failed = true;
		return false;
	}

	resolveAll();
	return true;
#elif ZHR_RUST_AUDIO_BACKEND
	if (zhr_audio_init(false) != 0) {
		RustAudioLoaded = true;
		return true;
	}
	Failed = true;
	return false;
#else
	Failed = true;
	return false;
#endif
}


void MilesLoader::unload()
{
	if (ReferenceCount > 0)
		--ReferenceCount;

	if (ReferenceCount > 0)
		return;

#if MILES_LOADER_SUPPORTED
	freeResources();
#elif ZHR_RUST_AUDIO_BACKEND
	if (RustAudioLoaded) {
		zhr_audio_shutdown();
		std::lock_guard<std::mutex> lock(g_audioMutex);
		g_sampleEosCbs.clear();
		g_sample3dEosCbs.clear();
		g_streamEosCbs.clear();
		RustAudioLoaded = false;
	}
#endif
	Failed = false;
	LastError = 0;
}


// The Miles functions below stand in for the imports of mss32.dll. On 64-bit / cross-platform
// builds, they route directly into ZeroHourReloaded's native Rust audio mixer (`zhr-audio`).


F32 __stdcall AIL_3D_sample_volume(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	float vol = 0.0f;
	zhr_audio_get_volume_pan(ptrToVoiceId(sample), &vol, nullptr);
	return vol;
#else
	return AIL_3D_sample_volumePtr != nullptr ? AIL_3D_sample_volumePtr(sample) : 0.0f;
#endif
}

void __stdcall AIL_set_3D_sample_volume(H3DSAMPLE sample, F32 volume)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_volume_pan(ptrToVoiceId(sample), volume, 0.5f);
#else
	if (AIL_set_3D_sample_volumePtr != nullptr)
		AIL_set_3D_sample_volumePtr(sample, volume);
#endif
}

void __stdcall AIL_end_3D_sample(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_stop(ptrToVoiceId(sample));
#else
	if (AIL_end_3D_samplePtr != nullptr)
		AIL_end_3D_samplePtr(sample);
#endif
}

void __stdcall AIL_resume_3D_sample(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_pause(ptrToVoiceId(sample), false);
#else
	if (AIL_resume_3D_samplePtr != nullptr)
		AIL_resume_3D_samplePtr(sample);
#endif
}

void __stdcall AIL_stop_3D_sample(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_stop(ptrToVoiceId(sample));
#else
	if (AIL_stop_3D_samplePtr != nullptr)
		AIL_stop_3D_samplePtr(sample);
#endif
}

void __stdcall AIL_start_3D_sample(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_start(ptrToVoiceId(sample));
#else
	if (AIL_start_3D_samplePtr != nullptr)
		AIL_start_3D_samplePtr(sample);
#endif
}

U32 __stdcall AIL_3D_sample_loop_count(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<U32>(zhr_audio_get_loop_count(ptrToVoiceId(sample)));
#else
	return AIL_3D_sample_loop_countPtr != nullptr ? AIL_3D_sample_loop_countPtr(sample) : 0;
#endif
}

void __stdcall AIL_set_3D_sample_offset(H3DSAMPLE sample, U32 offset)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_ms_position(ptrToVoiceId(sample), static_cast<uint32_t>(offset));
#else
	if (AIL_set_3D_sample_offsetPtr != nullptr)
		AIL_set_3D_sample_offsetPtr(sample, offset);
#endif
}

U32 __stdcall AIL_3D_sample_length(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t total = 0;
	zhr_audio_get_ms_position(ptrToVoiceId(sample), &total, nullptr);
	return static_cast<U32>(total);
#else
	return AIL_3D_sample_lengthPtr != nullptr ? AIL_3D_sample_lengthPtr(sample) : 0;
#endif
}

U32 __stdcall AIL_3D_sample_offset(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t cur = 0;
	zhr_audio_get_ms_position(ptrToVoiceId(sample), nullptr, &cur);
	return static_cast<U32>(cur);
#else
	return AIL_3D_sample_offsetPtr != nullptr ? AIL_3D_sample_offsetPtr(sample) : 0;
#endif
}

S32 __stdcall AIL_3D_sample_playback_rate(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_playback_rate(ptrToVoiceId(sample)));
#else
	return AIL_3D_sample_playback_ratePtr != nullptr ? AIL_3D_sample_playback_ratePtr(sample) : 0;
#endif
}

void __stdcall AIL_set_3D_sample_playback_rate(H3DSAMPLE sample, S32 playback_rate)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (playback_rate > 0)
		zhr_audio_set_playback_rate(ptrToVoiceId(sample), static_cast<uint32_t>(playback_rate));
#else
	if (AIL_set_3D_sample_playback_ratePtr != nullptr)
		AIL_set_3D_sample_playback_ratePtr(sample, playback_rate);
#endif
}

S32 __stdcall AIL_set_3D_sample_file(H3DSAMPLE sample, const void* file_image)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(sample);
	size_t sz = detectRiffSize(file_image);
	if (id == 0 || sz == 0)
		return 0;
	return zhr_audio_load_bytes(id, static_cast<const uint8_t*>(file_image), sz, "wav");
#else
	return AIL_set_3D_sample_filePtr != nullptr ? AIL_set_3D_sample_filePtr(sample, file_image) : 0;
#endif
}

HPROVIDER __stdcall AIL_set_sample_processor(HSAMPLE sample, SAMPLESTAGE pipeline_stage, HPROVIDER provider)
{
	return AIL_set_sample_processorPtr != nullptr
		? AIL_set_sample_processorPtr(sample, pipeline_stage, provider)
		: nullptr;
}

void __stdcall AIL_set_filter_sample_preference(HSAMPLE sample, const char* name, const void* val)
{
	if (AIL_set_filter_sample_preferencePtr != nullptr)
		AIL_set_filter_sample_preferencePtr(sample, name, val);
}

void __stdcall AIL_release_sample_handle(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(sample);
	if (id != 0) {
		{
			std::lock_guard<std::mutex> lock(g_audioMutex);
			g_sampleEosCbs.erase(id);
		}
		zhr_audio_release_voice(id);
	}
#else
	if (AIL_release_sample_handlePtr != nullptr)
		AIL_release_sample_handlePtr(sample);
#endif
}

void __stdcall AIL_close_3D_provider(HPROVIDER lib)
{
	if (AIL_close_3D_providerPtr != nullptr)
		AIL_close_3D_providerPtr(lib);
}

S32 __stdcall AIL_set_preference(U32 number, S32 value)
{
	return AIL_set_preferencePtr != nullptr ? AIL_set_preferencePtr(number, value) : 0;
}

S32 __stdcall AIL_waveOutOpen(HDIGDRIVER* driver, LPHWAVEOUT* waveout, S32 id, LPWAVEFORMAT format)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)id;
	(void)format;
	zhr_audio_init(false);
	if (driver != nullptr)
		*driver = &g_digDriver;
	if (waveout != nullptr)
		*waveout = nullptr;
	return 0;
#else
	if (AIL_waveOutOpenPtr != nullptr)
		return AIL_waveOutOpenPtr(driver, waveout, id, format);

	if (driver != nullptr)
		*driver = nullptr;
	if (waveout != nullptr)
		*waveout = nullptr;
	return 1;
#endif
}

void __stdcall AIL_waveOutClose(HDIGDRIVER driver)
{
	if (AIL_waveOutClosePtr != nullptr)
		AIL_waveOutClosePtr(driver);
}

void __stdcall AIL_set_3D_sample_loop_count(H3DSAMPLE sample, U32 count)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_loop_count(ptrToVoiceId(sample), static_cast<uint32_t>(count));
#else
	if (AIL_set_3D_sample_loop_countPtr != nullptr)
		AIL_set_3D_sample_loop_countPtr(sample, count);
#endif
}

void __stdcall AIL_set_stream_playback_rate(HSTREAM stream, S32 rate)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (rate > 0)
		zhr_audio_set_playback_rate(ptrToVoiceId(stream), static_cast<uint32_t>(rate));
#else
	if (AIL_set_stream_playback_ratePtr != nullptr)
		AIL_set_stream_playback_ratePtr(stream, rate);
#endif
}

S32 __stdcall AIL_stream_playback_rate(HSTREAM stream)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_playback_rate(ptrToVoiceId(stream)));
#else
	return AIL_stream_playback_ratePtr != nullptr ? AIL_stream_playback_ratePtr(stream) : 0;
#endif
}

void __stdcall AIL_stream_ms_position(HSTREAM sample, S32* total_milliseconds, S32* current_milliseconds)
{
#if ZHR_RUST_AUDIO_BACKEND
	flushCompletedCallbacks();
	uint32_t total = 0;
	uint32_t cur = 0;
	zhr_audio_get_ms_position(ptrToVoiceId(sample), &total, &cur);
	if (total_milliseconds != nullptr)
		*total_milliseconds = static_cast<S32>(total);
	if (current_milliseconds != nullptr)
		*current_milliseconds = static_cast<S32>(cur);
#else
	if (AIL_stream_ms_positionPtr != nullptr)
	{
		AIL_stream_ms_positionPtr(sample, total_milliseconds, current_milliseconds);
		return;
	}

	if (total_milliseconds != nullptr)
		*total_milliseconds = 0;
	if (current_milliseconds != nullptr)
		*current_milliseconds = 0;
#endif
}

void __stdcall AIL_set_stream_ms_position(HSTREAM stream, S32 pos)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_ms_position(ptrToVoiceId(stream), pos > 0 ? static_cast<uint32_t>(pos) : 0u);
#else
	if (AIL_set_stream_ms_positionPtr != nullptr)
		AIL_set_stream_ms_positionPtr(stream, pos);
#endif
}

S32 __stdcall AIL_stream_loop_count(HSTREAM stream)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_loop_count(ptrToVoiceId(stream)));
#else
	return AIL_stream_loop_countPtr != nullptr ? AIL_stream_loop_countPtr(stream) : 0;
#endif
}

void __stdcall AIL_set_stream_loop_block(HSTREAM stream, S32 loop_start, S32 loop_end)
{
	if (AIL_set_stream_loop_blockPtr != nullptr)
		AIL_set_stream_loop_blockPtr(stream, loop_start, loop_end);
}

void __stdcall AIL_set_stream_loop_count(HSTREAM stream, S32 count)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_loop_count(ptrToVoiceId(stream), count >= 0 ? static_cast<uint32_t>(count) : 0u);
#else
	if (AIL_set_stream_loop_countPtr != nullptr)
		AIL_set_stream_loop_countPtr(stream, count);
#endif
}

void __stdcall AIL_close_stream(HSTREAM stream)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(stream);
	if (id != 0) {
		{
			std::lock_guard<std::mutex> lock(g_audioMutex);
			g_streamEosCbs.erase(id);
		}
		zhr_audio_release_voice(id);
	}
#else
	if (AIL_close_streamPtr != nullptr)
		AIL_close_streamPtr(stream);
#endif
}

void __stdcall AIL_pause_stream(HSTREAM stream, S32 onoff)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_pause(ptrToVoiceId(stream), onoff != 0);
#else
	if (AIL_pause_streamPtr != nullptr)
		AIL_pause_streamPtr(stream, onoff);
#endif
}

AIL_stream_callback __stdcall AIL_register_stream_callback(HSTREAM stream, AIL_stream_callback callback)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(stream);
	if (id == 0)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_audioMutex);
	AIL_stream_callback prev = g_streamEosCbs[id];
	if (callback != nullptr)
		g_streamEosCbs[id] = callback;
	else
		g_streamEosCbs.erase(id);
	return prev;
#else
	return AIL_register_stream_callbackPtr != nullptr ? AIL_register_stream_callbackPtr(stream, callback) : nullptr;
#endif
}

AIL_3dsample_callback __stdcall AIL_register_3D_EOS_callback(H3DSAMPLE sample, AIL_3dsample_callback EOS)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(sample);
	if (id == 0)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_audioMutex);
	AIL_3dsample_callback prev = g_sample3dEosCbs[id];
	if (EOS != nullptr)
		g_sample3dEosCbs[id] = EOS;
	else
		g_sample3dEosCbs.erase(id);
	return prev;
#else
	return AIL_register_3D_EOS_callbackPtr != nullptr ? AIL_register_3D_EOS_callbackPtr(sample, EOS) : nullptr;
#endif
}

AIL_sample_callback __stdcall AIL_register_EOS_callback(HSAMPLE sample, AIL_sample_callback EOS)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(sample);
	if (id == 0)
		return nullptr;
	std::lock_guard<std::mutex> lock(g_audioMutex);
	AIL_sample_callback prev = g_sampleEosCbs[id];
	if (EOS != nullptr)
		g_sampleEosCbs[id] = EOS;
	else
		g_sampleEosCbs.erase(id);
	return prev;
#else
	return AIL_register_EOS_callbackPtr != nullptr ? AIL_register_EOS_callbackPtr(sample, EOS) : nullptr;
#endif
}

void __stdcall AIL_start_stream(HSTREAM stream)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_start(ptrToVoiceId(stream));
#else
	if (AIL_start_streamPtr != nullptr)
		AIL_start_streamPtr(stream);
#endif
}

void __stdcall AIL_set_sample_playback_rate(HSAMPLE sample, S32 playback_rate)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (playback_rate > 0)
		zhr_audio_set_playback_rate(ptrToVoiceId(sample), static_cast<uint32_t>(playback_rate));
#else
	if (AIL_set_sample_playback_ratePtr != nullptr)
		AIL_set_sample_playback_ratePtr(sample, playback_rate);
#endif
}

S32 __stdcall AIL_sample_playback_rate(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_playback_rate(ptrToVoiceId(sample)));
#else
	return AIL_sample_playback_ratePtr != nullptr ? AIL_sample_playback_ratePtr(sample) : 0;
#endif
}

void __stdcall AIL_sample_ms_position(HSAMPLE sample, S32* total_ms, S32* current_ms)
{
#if ZHR_RUST_AUDIO_BACKEND
	flushCompletedCallbacks();
	uint32_t total = 0;
	uint32_t cur = 0;
	zhr_audio_get_ms_position(ptrToVoiceId(sample), &total, &cur);
	if (total_ms != nullptr)
		*total_ms = static_cast<S32>(total);
	if (current_ms != nullptr)
		*current_ms = static_cast<S32>(cur);
#else
	if (AIL_sample_ms_positionPtr != nullptr)
	{
		AIL_sample_ms_positionPtr(sample, total_ms, current_ms);
		return;
	}

	if (total_ms != nullptr)
		*total_ms = 0;
	if (current_ms != nullptr)
		*current_ms = 0;
#endif
}

void __stdcall AIL_set_sample_ms_position(HSAMPLE sample, S32 pos)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_ms_position(ptrToVoiceId(sample), pos > 0 ? static_cast<uint32_t>(pos) : 0u);
#else
	if (AIL_set_sample_ms_positionPtr != nullptr)
		AIL_set_sample_ms_positionPtr(sample, pos);
#endif
}

S32 __stdcall AIL_sample_loop_count(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_loop_count(ptrToVoiceId(sample)));
#else
	return AIL_sample_loop_countPtr != nullptr ? AIL_sample_loop_countPtr(sample) : 0;
#endif
}

void __stdcall AIL_set_sample_loop_count(HSAMPLE sample, S32 count)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_loop_count(ptrToVoiceId(sample), count >= 0 ? static_cast<uint32_t>(count) : 0u);
#else
	if (AIL_set_sample_loop_countPtr != nullptr)
		AIL_set_sample_loop_countPtr(sample, count);
#endif
}

void __stdcall AIL_end_sample(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_stop(ptrToVoiceId(sample));
#else
	if (AIL_end_samplePtr != nullptr)
		AIL_end_samplePtr(sample);
#endif
}

void __stdcall AIL_resume_sample(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_pause(ptrToVoiceId(sample), false);
#else
	if (AIL_resume_samplePtr != nullptr)
		AIL_resume_samplePtr(sample);
#endif
}

void __stdcall AIL_stop_sample(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_stop(ptrToVoiceId(sample));
#else
	if (AIL_stop_samplePtr != nullptr)
		AIL_stop_samplePtr(sample);
#endif
}

void __stdcall AIL_start_sample(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_start(ptrToVoiceId(sample));
#else
	if (AIL_start_samplePtr != nullptr)
		AIL_start_samplePtr(sample);
#endif
}

void __stdcall AIL_init_sample(HSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_reset_voice(ptrToVoiceId(sample));
#else
	if (AIL_init_samplePtr != nullptr)
		AIL_init_samplePtr(sample);
#endif
}

S32 __stdcall AIL_set_named_sample_file(HSAMPLE sample, const char* file_name, const void* file_image, S32 file_size, S32 block)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)block;
	uint32_t id = ptrToVoiceId(sample);
	if (id == 0 || file_image == nullptr || file_size <= 0)
		return 0;
	return zhr_audio_load_bytes(
		id,
		static_cast<const uint8_t*>(file_image),
		static_cast<size_t>(file_size),
		extensionHintFromName(file_name));
#else
	return AIL_set_named_sample_filePtr != nullptr
		? AIL_set_named_sample_filePtr(sample, file_name, file_image, file_size, block)
		: 0;
#endif
}

void __stdcall AIL_set_3D_sample_effects_level(H3DSAMPLE sample, F32 effect_level)
{
	if (AIL_set_3D_sample_effects_levelPtr != nullptr)
		AIL_set_3D_sample_effects_levelPtr(sample, effect_level);
}

void __stdcall AIL_set_3D_sample_distances(H3DSAMPLE sample, F32 max_dist, F32 min_dist)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_3d_distances(ptrToVoiceId(sample), max_dist, min_dist);
#else
	if (AIL_set_3D_sample_distancesPtr != nullptr)
		AIL_set_3D_sample_distancesPtr(sample, max_dist, min_dist);
#endif
}

void __stdcall AIL_set_3D_velocity_vector(H3DPOBJECT obj, F32 x, F32 y, F32 z)
{
	if (AIL_set_3D_velocity_vectorPtr != nullptr)
		AIL_set_3D_velocity_vectorPtr(obj, x, y, z);
}

void __stdcall AIL_set_3D_position(H3DPOBJECT obj, F32 X, F32 Y, F32 Z)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (reinterpret_cast<uintptr_t>(obj) == kListenerSentinel)
	{
		zhr_audio_set_listener_position(X, Y, Z);
		flushCompletedCallbacks();
		return;
	}
	zhr_audio_set_3d_position(ptrToVoiceId(obj), X, Y, Z);
#else
	if (AIL_set_3D_positionPtr != nullptr)
		AIL_set_3D_positionPtr(obj, X, Y, Z);
#endif
}

void __stdcall AIL_set_3D_orientation(H3DPOBJECT obj, F32 X_face, F32 Y_face, F32 Z_face, F32 X_up, F32 Y_up, F32 Z_up)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (reinterpret_cast<uintptr_t>(obj) == kListenerSentinel)
	{
		zhr_audio_set_listener_orientation(X_face, Y_face, Z_face, X_up, Y_up, Z_up);
		flushCompletedCallbacks();
	}
#else
	if (AIL_set_3D_orientationPtr != nullptr)
		AIL_set_3D_orientationPtr(obj, X_face, Y_face, Z_face, X_up, Y_up, Z_up);
#endif
}

S32 __stdcall AIL_WAV_info(const void* data, AILSOUNDINFO* info)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (info != nullptr)
		std::memset(info, 0, sizeof(*info));
	if (data == nullptr || info == nullptr)
		return 0;

	const size_t totalSize = detectRiffSize(data);
	if (totalSize < 36)
		return 0;

	const uint8_t* p = static_cast<const uint8_t*>(data);
	if (std::memcmp(p + 8, "WAVE", 4) != 0)
		return 0;

	info->initial_ptr = data;
	bool foundFmt = false;
	bool foundData = false;
	size_t off = 12;
	while (off + 8 <= totalSize)
	{
		uint32_t chunkSize = 0;
		std::memcpy(&chunkSize, p + off + 4, 4);
		const uint8_t* body = p + off + 8;
		if (off + 8 + chunkSize > totalSize)
			chunkSize = static_cast<U32>(totalSize - (off + 8));

		if (std::memcmp(p + off, "fmt ", 4) == 0 && chunkSize >= 16)
		{
			uint16_t fmtTag = 0, channels = 0, blockAlign = 0, bits = 0;
			uint32_t sampleRate = 0;
			std::memcpy(&fmtTag, body + 0, 2);
			std::memcpy(&channels, body + 2, 2);
			std::memcpy(&sampleRate, body + 4, 4);
			std::memcpy(&blockAlign, body + 12, 2);
			std::memcpy(&bits, body + 14, 2);
			info->format = static_cast<S32>(fmtTag);
			info->channels = static_cast<S32>(channels);
			info->rate = static_cast<U32>(sampleRate);
			info->block_size = static_cast<U32>(blockAlign);
			info->bits = static_cast<S32>(bits);
			foundFmt = true;
		}
		else if (std::memcmp(p + off, "fact", 4) == 0 && chunkSize >= 4)
		{
			uint32_t samples = 0;
			std::memcpy(&samples, body, 4);
			info->samples = static_cast<U32>(samples);
		}
		else if (std::memcmp(p + off, "data", 4) == 0)
		{
			info->data_ptr = body;
			info->data_len = static_cast<U32>(chunkSize);
			foundData = true;
		}

		off += 8 + chunkSize + (chunkSize & 1u);
	}

	if (foundFmt && foundData && info->samples == 0 && info->channels > 0 && info->bits > 0)
	{
		const U32 bytesPerFrame = static_cast<U32>((info->channels * info->bits) / 8);
		if (bytesPerFrame > 0)
			info->samples = info->data_len / bytesPerFrame;
	}
	return (foundFmt && foundData) ? 1 : 0;
#else
	if (AIL_WAV_infoPtr != nullptr)
		return AIL_WAV_infoPtr(data, info);

	if (info != nullptr)
	{
		info->format = 0;
		info->data_ptr = nullptr;
		info->data_len = 0;
		info->rate = 0;
		info->bits = 0;
		info->channels = 0;
		info->samples = 0;
		info->block_size = 0;
		info->initial_ptr = nullptr;
	}
	return 0;
#endif
}

void __stdcall AIL_stop_timer(HTIMER timer)
{
	if (AIL_stop_timerPtr != nullptr)
		AIL_stop_timerPtr(timer);
}

void __stdcall AIL_release_timer_handle(HTIMER timer)
{
	if (AIL_release_timer_handlePtr != nullptr)
		AIL_release_timer_handlePtr(timer);
}

void __stdcall AIL_shutdown(void)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_shutdown();
#else
	if (AIL_shutdownPtr != nullptr)
		AIL_shutdownPtr();
#endif
}

S32 __stdcall AIL_enumerate_filters(HPROENUM* next, HPROVIDER* dest, char** name)
{
	if (AIL_enumerate_filtersPtr != nullptr)
		return AIL_enumerate_filtersPtr(next, dest, name);

	if (dest != nullptr)
		*dest = nullptr;
	if (name != nullptr)
		*name = nullptr;
	return 0;
}

void __stdcall AIL_set_file_callbacks(AIL_file_open_callback opencb, AIL_file_close_callback closecb, AIL_file_seek_callback seekcb, AIL_file_read_callback readcb)
{
#if ZHR_RUST_AUDIO_BACKEND
	std::lock_guard<std::mutex> lock(g_audioMutex);
	g_fileOpenCb = opencb;
	g_fileCloseCb = closecb;
	g_fileSeekCb = seekcb;
	g_fileReadCb = readcb;
#else
	if (AIL_set_file_callbacksPtr != nullptr)
		AIL_set_file_callbacksPtr(opencb, closecb, seekcb, readcb);
#endif
}

void __stdcall AIL_release_3D_sample_handle(H3DSAMPLE sample)
{
#if ZHR_RUST_AUDIO_BACKEND
	uint32_t id = ptrToVoiceId(sample);
	if (id != 0) {
		{
			std::lock_guard<std::mutex> lock(g_audioMutex);
			g_sample3dEosCbs.erase(id);
		}
		zhr_audio_release_voice(id);
	}
#else
	if (AIL_release_3D_sample_handlePtr != nullptr)
		AIL_release_3D_sample_handlePtr(sample);
#endif
}

H3DSAMPLE __stdcall AIL_allocate_3D_sample_handle(HPROVIDER lib)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)lib;
	uint32_t id = zhr_audio_alloc_voice(1);
	return reinterpret_cast<H3DSAMPLE>(static_cast<uintptr_t>(id));
#else
	return AIL_allocate_3D_sample_handlePtr != nullptr ? AIL_allocate_3D_sample_handlePtr(lib) : nullptr;
#endif
}

void __stdcall AIL_set_3D_user_data(H3DPOBJECT obj, U32 index, S32 value)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_user_data(ptrToVoiceId(obj), static_cast<uint32_t>(index), static_cast<int32_t>(value));
#else
	if (AIL_set_3D_user_dataPtr != nullptr)
		AIL_set_3D_user_dataPtr(obj, index, value);
#endif
}

void __stdcall AIL_unlock(void)
{
	if (AIL_unlockPtr != nullptr)
		AIL_unlockPtr();
}

void __stdcall AIL_unlock_mutex(void)
{
	if (AIL_unlock_mutexPtr != nullptr)
		AIL_unlock_mutexPtr();
}

void __stdcall AIL_lock(void)
{
	if (AIL_lockPtr != nullptr)
		AIL_lockPtr();
}

void __stdcall AIL_lock_mutex(void)
{
	if (AIL_lock_mutexPtr != nullptr)
		AIL_lock_mutexPtr();
}

void __stdcall AIL_set_3D_speaker_type(HPROVIDER lib, S32 speaker_type)
{
	if (AIL_set_3D_speaker_typePtr != nullptr)
		AIL_set_3D_speaker_typePtr(lib, speaker_type);
}

void __stdcall AIL_close_3D_listener(H3DPOBJECT listener)
{
	if (AIL_close_3D_listenerPtr != nullptr)
		AIL_close_3D_listenerPtr(listener);
}

S32 __stdcall AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (next == nullptr || *next != HPROENUM_FIRST)
		return 0;
	*next = 1;
	if (dest != nullptr)
		*dest = reinterpret_cast<HPROVIDER>(kProviderSentinel);
	if (name != nullptr)
		*name = g_providerName;
	return 1;
#else
	if (AIL_enumerate_3D_providersPtr != nullptr)
		return AIL_enumerate_3D_providersPtr(next, dest, name);

	if (dest != nullptr)
		*dest = nullptr;
	if (name != nullptr)
		*name = nullptr;
	return 0;
#endif
}

M3DRESULT __stdcall AIL_open_3D_provider(HPROVIDER lib)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)lib;
	return M3D_NOERR;
#else
	return AIL_open_3D_providerPtr != nullptr ? AIL_open_3D_providerPtr(lib) : 1;
#endif
}

char* __stdcall AIL_last_error(void)
{
#if ZHR_RUST_AUDIO_BACKEND
	return g_emptyError;
#else
	return AIL_last_errorPtr != nullptr ? AIL_last_errorPtr() : nullptr;
#endif
}

H3DPOBJECT __stdcall AIL_open_3D_listener(HPROVIDER lib)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)lib;
	return reinterpret_cast<H3DPOBJECT>(kListenerSentinel);
#else
	return AIL_open_3D_listenerPtr != nullptr ? AIL_open_3D_listenerPtr(lib) : nullptr;
#endif
}

S32 __stdcall AIL_3D_user_data(H3DPOBJECT obj, U32 index)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_user_data(ptrToVoiceId(obj), static_cast<uint32_t>(index)));
#else
	return AIL_3D_user_dataPtr != nullptr ? AIL_3D_user_dataPtr(obj, index) : 0;
#endif
}

S32 __stdcall AIL_sample_user_data(HSAMPLE sample, U32 index)
{
#if ZHR_RUST_AUDIO_BACKEND
	return static_cast<S32>(zhr_audio_get_user_data(ptrToVoiceId(sample), static_cast<uint32_t>(index)));
#else
	return AIL_sample_user_dataPtr != nullptr ? AIL_sample_user_dataPtr(sample, index) : 0;
#endif
}

HSAMPLE __stdcall AIL_allocate_sample_handle(HDIGDRIVER dig)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)dig;
	uint32_t id = zhr_audio_alloc_voice(0);
	return reinterpret_cast<HSAMPLE>(static_cast<uintptr_t>(id));
#else
	return AIL_allocate_sample_handlePtr != nullptr ? AIL_allocate_sample_handlePtr(dig) : nullptr;
#endif
}

void __stdcall AIL_set_sample_user_data(HSAMPLE sample, U32 index, S32 value)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_user_data(ptrToVoiceId(sample), static_cast<uint32_t>(index), static_cast<int32_t>(value));
#else
	if (AIL_set_sample_user_dataPtr != nullptr)
		AIL_set_sample_user_dataPtr(sample, index, value);
#endif
}

S32 __stdcall AIL_decompress_ADPCM(const AILSOUNDINFO *info, void **outdata, U32 *outsize)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (outdata != nullptr)
		*outdata = nullptr;
	if (outsize != nullptr)
		*outsize = 0;
	if (info == nullptr || info->initial_ptr == nullptr || outdata == nullptr || outsize == nullptr)
		return 0;

	const size_t srcLen = detectRiffSize(info->initial_ptr);
	if (srcLen == 0)
		return 0;

	uint8_t* rustBuf = nullptr;
	size_t rustLen = 0;
	if (zhr_audio_decode_wav_pcm16(
			static_cast<const uint8_t*>(info->initial_ptr),
			srcLen,
			&rustBuf,
			&rustLen) == 0 || rustBuf == nullptr || rustLen == 0)
	{
		return 0;
	}

	void* copy = std::malloc(rustLen);
	if (copy != nullptr)
	{
		std::memcpy(copy, rustBuf, rustLen);
		*outdata = copy;
		*outsize = static_cast<U32>(rustLen);
	}
	zhr_audio_free_buffer(rustBuf, rustLen);
	return copy != nullptr ? 1 : 0;
#else
	if (AIL_decompress_ADPCMPtr != nullptr)
		return AIL_decompress_ADPCMPtr(info, outdata, outsize);

	if (outdata != nullptr)
		*outdata = nullptr;
	if (outsize != nullptr)
		*outsize = 0;
	return 0;
#endif
}

void __stdcall AIL_get_DirectSound_info(HSAMPLE sample, AILLPDIRECTSOUND *lplpDS, AILLPDIRECTSOUNDBUFFER *lplpDSB)
{
	if (AIL_get_DirectSound_infoPtr != nullptr)
	{
		AIL_get_DirectSound_infoPtr(sample, lplpDS, lplpDSB);
		return;
	}

	if (lplpDS != nullptr)
		*lplpDS = nullptr;
	if (lplpDSB != nullptr)
		*lplpDSB = nullptr;
}

void __stdcall AIL_mem_free_lock(void *ptr)
{
#if ZHR_RUST_AUDIO_BACKEND
	std::free(ptr);
#else
	if (AIL_mem_free_lockPtr != nullptr)
		AIL_mem_free_lockPtr(ptr);
#endif
}

HSTREAM __stdcall AIL_open_stream(HDIGDRIVER dig, const char *filename, S32 stream_mem)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)dig;
	(void)stream_mem;
	std::vector<uint8_t> bytes;
	if (!readStreamFileBytes(filename, bytes))
		return nullptr;
	uint32_t id = zhr_audio_alloc_voice(2);
	if (id == 0)
		return nullptr;
	if (zhr_audio_load_bytes(id, bytes.data(), bytes.size(), extensionHintFromName(filename)) == 0)
	{
		zhr_audio_release_voice(id);
		return nullptr;
	}
	return reinterpret_cast<HSTREAM>(static_cast<uintptr_t>(id));
#else
	return AIL_open_streamPtr != nullptr ? AIL_open_streamPtr(dig, filename, stream_mem) : nullptr;
#endif
}

S32 __stdcall AIL_startup(void)
{
#if ZHR_RUST_AUDIO_BACKEND
	return zhr_audio_init(false);
#else
	return AIL_startupPtr != nullptr ? AIL_startupPtr() : 0;
#endif
}

void __stdcall AIL_quick_unload(HAUDIO audio)
{
#if ZHR_RUST_AUDIO_BACKEND
	AIL_close_stream(reinterpret_cast<HSTREAM>(audio));
#else
	if (AIL_quick_unloadPtr != nullptr)
		AIL_quick_unloadPtr(audio);
#endif
}

HAUDIO __stdcall AIL_quick_load_and_play(const char *filename, U32 loop_count, S32 wait_request)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)wait_request;
	HSTREAM stream = AIL_open_stream(&g_digDriver, filename, 0);
	if (stream == nullptr)
		return nullptr;
	AIL_set_stream_loop_count(stream, static_cast<S32>(loop_count));
	AIL_start_stream(stream);
	return reinterpret_cast<HAUDIO>(stream);
#else
	return AIL_quick_load_and_playPtr != nullptr
		? AIL_quick_load_and_playPtr(filename, loop_count, wait_request)
		: nullptr;
#endif
}

void __stdcall AIL_quick_set_volume(HAUDIO audio, F32 volume, F32 extravol)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)extravol;
	AIL_set_stream_volume_pan(reinterpret_cast<HSTREAM>(audio), volume, 0.5f);
#else
	if (AIL_quick_set_volumePtr != nullptr)
		AIL_quick_set_volumePtr(audio, volume, extravol);
#endif
}

S32 __stdcall AIL_quick_startup(S32 use_digital, S32 use_MIDI, U32 output_rate, S32 output_bits, S32 output_channels)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)use_digital;
	(void)use_MIDI;
	(void)output_rate;
	(void)output_bits;
	(void)output_channels;
	return zhr_audio_init(false);
#else
	return AIL_quick_startupPtr != nullptr
		? AIL_quick_startupPtr(use_digital, use_MIDI, output_rate, output_bits, output_channels)
		: 0;
#endif
}

void __stdcall AIL_quick_handles(HDIGDRIVER *pdig, HMDIDRIVER *pmdi, HDLSDEVICE *pdls)
{
#if ZHR_RUST_AUDIO_BACKEND
	if (pdig != nullptr)
		*pdig = &g_digDriver;
	if (pmdi != nullptr)
		*pmdi = nullptr;
	if (pdls != nullptr)
		*pdls = nullptr;
#else
	if (AIL_quick_handlesPtr != nullptr)
	{
		AIL_quick_handlesPtr(pdig, pmdi, pdls);
		return;
	}

	if (pdig != nullptr)
		*pdig = nullptr;
	if (pmdi != nullptr)
		*pmdi = nullptr;
	if (pdls != nullptr)
		*pdls = nullptr;
#endif
}

void __stdcall AIL_sample_volume_pan(HSAMPLE sample, F32 *volume, F32 *pan)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_get_volume_pan(ptrToVoiceId(sample), volume, pan);
#else
	if (AIL_sample_volume_panPtr != nullptr)
	{
		AIL_sample_volume_panPtr(sample, volume, pan);
		return;
	}

	if (volume != nullptr)
		*volume = 0.0f;
	if (pan != nullptr)
		*pan = 0.5f;
#endif
}

void __stdcall AIL_set_3D_sample_occlusion(H3DSAMPLE sample, F32 occlusion)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_3d_occlusion(ptrToVoiceId(sample), occlusion);
#else
	if (AIL_set_3D_sample_occlusionPtr != nullptr)
		AIL_set_3D_sample_occlusionPtr(sample, occlusion);
#endif
}

char * __stdcall AIL_set_redist_directory(const char *dir)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)dir;
	return g_emptyError;
#else
	return AIL_set_redist_directoryPtr != nullptr ? AIL_set_redist_directoryPtr(dir) : nullptr;
#endif
}

S32 __stdcall AIL_set_sample_file(HSAMPLE sample, const void *file_image, S32 block)
{
#if ZHR_RUST_AUDIO_BACKEND
	(void)block;
	uint32_t id = ptrToVoiceId(sample);
	size_t sz = detectRiffSize(file_image);
	if (id == 0 || sz == 0)
		return 0;
	return zhr_audio_load_bytes(id, static_cast<const uint8_t*>(file_image), sz, "wav");
#else
	return AIL_set_sample_filePtr != nullptr ? AIL_set_sample_filePtr(sample, file_image, block) : 0;
#endif
}

void __stdcall AIL_set_sample_volume_pan(HSAMPLE sample, F32 volume, F32 pan)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_volume_pan(ptrToVoiceId(sample), volume, pan);
#else
	if (AIL_set_sample_volume_panPtr != nullptr)
		AIL_set_sample_volume_panPtr(sample, volume, pan);
#endif
}

void __stdcall AIL_set_stream_volume_pan(HSTREAM stream, F32 volume, F32 pan)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_set_volume_pan(ptrToVoiceId(stream), volume, pan);
#else
	if (AIL_set_stream_volume_panPtr != nullptr)
		AIL_set_stream_volume_panPtr(stream, volume, pan);
#endif
}

void __stdcall AIL_stream_volume_pan(HSTREAM stream, F32 *volume, F32 *pan)
{
#if ZHR_RUST_AUDIO_BACKEND
	zhr_audio_get_volume_pan(ptrToVoiceId(stream), volume, pan);
#else
	if (AIL_stream_volume_panPtr != nullptr)
	{
		AIL_stream_volume_panPtr(stream, volume, pan);
		return;
	}

	if (volume != nullptr)
		*volume = 0.0f;
	if (pan != nullptr)
		*pan = 0.5f;
#endif
}

U32 __stdcall AIL_get_timer_highest_delay(void)
{
	return AIL_get_timer_highest_delayPtr != nullptr ? AIL_get_timer_highest_delayPtr() : 0;
}
