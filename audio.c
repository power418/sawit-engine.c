#include "audio.h"

#include "diagnostics.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include "audio_macos.h"
#include <dirent.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#endif

enum
{
  AUDIO_COMMAND_LENGTH = 1024,
  AUDIO_VOLUME_MIN = 0,
  AUDIO_VOLUME_MAX = 1000,
  AUDIO_DEFAULT_TARGET_VOLUME = 680,
  AUDIO_FADE_IN_DURATION_MS = 1100
};

static int audio_directory_exists(const char* path);
static const char* audio_find_last_path_separator(const char* path);
static const char* audio_get_basename(const char* path);
static int audio_build_relative_path(const char* base_path, const char* relative_path, char* out_path, size_t out_path_size);
static int audio_resolve_audio_directory(char* out_path, size_t out_path_size);
static int audio_load_tracks(AudioState* state);
static void audio_clear_tracks(AudioState* state);
static int audio_reserve_tracks(AudioState* player, size_t required_track_capacity);
static int audio_add_track(AudioState* player, const char* full_path, int score);
static int audio_track_compare(const void* left, const void* right);
static int audio_is_supported_extension(const char* path);
static int audio_score_candidate(const char* filename);
static int audio_string_contains_ignore_case(const char* text, const char* needle);
static int audio_compare_ignore_case(const char* a, const char* b);
static char audio_to_lower_ascii(char value);
static unsigned long long audio_get_tick_ms(void);
static int audio_open_and_play(AudioState* player, const char* path);
static int audio_play_track_from(AudioState* player, size_t start_index);
static int audio_set_volume(AudioState* state, int volume, int log_failure);
static void audio_begin_fade_in(AudioState* state);
static void audio_update_fade(AudioState* state);
static int audio_clamp_int(int value, int min_value, int max_value);

#if defined(__linux__)
static int audio_linux_open_and_play(AudioState* state, const char* path);
static void audio_linux_stop(AudioState* state);
static int audio_linux_is_playing(const AudioState* state);
static int audio_linux_set_volume(AudioState* state, int volume);
#endif

#if defined(_WIN32)
typedef struct AudioWavePlayer
{
  HWAVEOUT device;
  WAVEHDR header;
  unsigned char* pcm_data;
  DWORD pcm_size;
  int header_prepared;
} AudioWavePlayer;

static const char* k_audio_alias = "sawit_game_music";
static int g_audio_mf_started = 0;
static int g_audio_com_initialized = 0;

static int audio_wave_open_and_play(AudioState* state, const char* path);
static void audio_wave_stop(AudioState* state);
static int audio_wave_is_playing(const AudioState* state);
static int audio_wave_set_volume(AudioState* state, int volume);
static int audio_wave_decode_file(const char* path, WAVEFORMATEX** out_format, UINT32* out_format_size, unsigned char** out_pcm_data, DWORD* out_pcm_size);
static int audio_wave_append_pcm(unsigned char** data, DWORD* size, DWORD* capacity, const BYTE* source, DWORD source_size);
static int audio_windows_media_foundation_startup(void);
static void audio_windows_media_foundation_shutdown(void);
static int audio_windows_path_to_wide(const char* path, wchar_t* out_path, size_t out_path_count);
static void audio_normalize_path_separators(char* path);
static int audio_send_command(const char* command, int log_failure);
static int audio_send_command_for_string(const char* command, char* out_text, size_t out_text_size, int log_failure);
static int audio_query_mode(const AudioState* player, char* out_mode, size_t out_mode_size);
static void audio_trim_trailing_whitespace(char* text);
#endif

void audio_init(AudioState* state)
{
  if (state == NULL)
  {
    return;
  }

  memset(state, 0, sizeof(*state));
  state->target_volume = AUDIO_DEFAULT_TARGET_VOLUME;
}

int audio_start_music(AudioState* state)
{
  if (state == NULL)
  {
    return 0;
  }

  audio_stop(state);
  audio_clear_tracks(state);
  if (!audio_load_tracks(state))
  {
    diagnostics_log("audio: no supported music track found in res/audio");
    return 0;
  }

#if defined(_WIN32)
  if (!audio_play_track_from(state, 0U))
  {
    diagnostics_log("audio: failed to start any playable music track from res/audio");
    audio_clear_tracks(state);
    return 0;
  }

  diagnostics_logf("audio: background music started with %zu track(s), now playing '%s'", state->track_count, state->active_path);
  return 1;
#elif defined(__APPLE__) || defined(__linux__)
  if (!audio_play_track_from(state, 0U))
  {
    diagnostics_log("audio: failed to start any playable music track from res/audio");
    audio_clear_tracks(state);
    return 0;
  }

  diagnostics_logf("audio: background music started with %zu track(s), now playing '%s'", state->track_count, state->active_path);
  return 1;
#else
  diagnostics_logf("audio: found %zu music track(s) in res/audio", state->track_count);
  diagnostics_log("audio: native background music playback is currently implemented only for Windows, macOS, and Linux");
  return 0;
#endif
}

void audio_update(AudioState* state)
{
#if defined(_WIN32)
  char mode[64] = { 0 };

  if (state == NULL || state->is_open == 0)
  {
    return;
  }

  audio_update_fade(state);

  if (state->native_player != NULL)
  {
    if (audio_wave_is_playing(state))
    {
      return;
    }

    if (state->track_count > 0U)
    {
      const size_t next_track_index =
        (state->track_count > 1U) ? ((state->current_track_index + 1U) % state->track_count) : state->current_track_index;
      (void)audio_play_track_from(state, next_track_index);
    }
    return;
  }

  if (!audio_query_mode(state, mode, sizeof(mode)))
  {
    return;
  }

  if (audio_compare_ignore_case(mode, "playing") == 0 ||
    audio_compare_ignore_case(mode, "paused") == 0 ||
    audio_compare_ignore_case(mode, "seeking") == 0)
  {
    return;
  }

  if (state->track_count > 0U)
  {
    const size_t next_track_index =
      (state->track_count > 1U) ? ((state->current_track_index + 1U) % state->track_count) : state->current_track_index;
    (void)audio_play_track_from(state, next_track_index);
  }
#elif defined(__APPLE__) || defined(__linux__)
  if (state == NULL || state->is_open == 0)
  {
    return;
  }

  audio_update_fade(state);

#if defined(__APPLE__)
  if (audio_macos_is_playing(state))
  {
    return;
  }
#else
  if (audio_linux_is_playing(state))
  {
    return;
  }
#endif

  if (state->track_count > 0U)
  {
    const size_t next_track_index =
      (state->track_count > 1U) ? ((state->current_track_index + 1U) % state->track_count) : state->current_track_index;
    (void)audio_play_track_from(state, next_track_index);
  }
#else
  (void)state;
#endif
}

void audio_stop(AudioState* state)
{
  if (state == NULL)
  {
    return;
  }

#if defined(_WIN32)
  if (state->native_player != NULL)
  {
    audio_wave_stop(state);
  }
  if (state->is_open != 0)
  {
    char command[AUDIO_COMMAND_LENGTH] = { 0 };

    (void)snprintf(command, sizeof(command), "stop %s", k_audio_alias);
    (void)audio_send_command(command, 0);
  }
  {
    char command[AUDIO_COMMAND_LENGTH] = { 0 };
    (void)snprintf(command, sizeof(command), "close %s", k_audio_alias);
    (void)audio_send_command(command, 0);
  }
  state->is_open = 0;
#elif defined(__APPLE__)
  if (state->native_player != NULL)
  {
    audio_macos_stop(state);
  }
#elif defined(__linux__)
  if (state->native_player_pid != 0)
  {
    audio_linux_stop(state);
  }
#endif

  state->is_open = 0;
  state->active_path[0] = '\0';
  state->fade_active = 0;
  state->current_volume = AUDIO_VOLUME_MIN;
  state->target_volume = AUDIO_DEFAULT_TARGET_VOLUME;
  state->fade_start_ms = 0ULL;
}

void audio_shutdown(AudioState* state)
{
  audio_stop(state);
  audio_clear_tracks(state);
#if defined(_WIN32)
  audio_windows_media_foundation_shutdown();
#endif
}

static int audio_directory_exists(const char* path)
{
  if (path == NULL || path[0] == '\0')
  {
    return 0;
  }

#if defined(_WIN32)
  {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  }
#else
  {
    struct stat status = { 0 };
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
  }
#endif
}

static const char* audio_find_last_path_separator(const char* path)
{
  const char* last_backslash = NULL;
  const char* last_slash = NULL;

  if (path == NULL)
  {
    return NULL;
  }

  last_backslash = strrchr(path, '\\');
  last_slash = strrchr(path, '/');
  if (last_backslash == NULL)
  {
    return last_slash;
  }
  if (last_slash == NULL)
  {
    return last_backslash;
  }

  return (last_backslash > last_slash) ? last_backslash : last_slash;
}

static const char* audio_get_basename(const char* path)
{
  const char* separator = audio_find_last_path_separator(path);
  return (separator != NULL) ? (separator + 1) : path;
}

static int audio_build_relative_path(const char* base_path, const char* relative_path, char* out_path, size_t out_path_size)
{
  const char* last_separator = NULL;
  size_t directory_length = 0U;

  if (base_path == NULL || relative_path == NULL || out_path == NULL || out_path_size == 0U)
  {
    return 0;
  }

  last_separator = audio_find_last_path_separator(base_path);
  if (last_separator == NULL)
  {
    return 0;
  }

  directory_length = (size_t)(last_separator - base_path + 1);
  if (directory_length + strlen(relative_path) + 1U > out_path_size)
  {
    return 0;
  }

  memcpy(out_path, base_path, directory_length);
  (void)snprintf(out_path + directory_length, out_path_size - directory_length, "%s", relative_path);
  return 1;
}

static int audio_resolve_audio_directory(char* out_path, size_t out_path_size)
{
  char module_path[PLATFORM_PATH_MAX] = { 0 };
  char candidate_directory[PLATFORM_PATH_MAX] = { 0 };
  char current_directory[PLATFORM_PATH_MAX] = { 0 };
  static const char* k_audio_directory = "res/audio/";
  static const char* k_audio_fallbacks[] = {
    "res/audio/",
    "../res/audio/",
    "../../res/audio/",
    "../../../res/audio/",
    "../Resources/res/audio/"
  };
  size_t i = 0U;

  if (out_path == NULL || out_path_size == 0U)
  {
    return 0;
  }

  if (!platform_support_get_executable_path(module_path, sizeof(module_path)))
  {
    return 0;
  }

  {
    const char* last_separator = audio_find_last_path_separator(module_path);
    if (last_separator == NULL)
    {
      return 0;
    }

    ((char*)last_separator)[1] = '\0';
    if (strlen(module_path) + strlen(k_audio_directory) + 1U <= sizeof(candidate_directory))
    {
      (void)snprintf(candidate_directory, sizeof(candidate_directory), "%s%s", module_path, k_audio_directory);
      if (audio_directory_exists(candidate_directory))
      {
        (void)snprintf(out_path, out_path_size, "%s", candidate_directory);
        return 1;
      }
    }
  }

  if (platform_support_get_current_directory(current_directory, sizeof(current_directory)) &&
    strlen(current_directory) + 1U + strlen(k_audio_directory) + 1U <= sizeof(candidate_directory))
  {
    (void)snprintf(candidate_directory, sizeof(candidate_directory), "%s/%s", current_directory, k_audio_directory);
    if (audio_directory_exists(candidate_directory))
    {
      (void)snprintf(out_path, out_path_size, "%s", candidate_directory);
      return 1;
    }
  }

  for (i = 0U; i < sizeof(k_audio_fallbacks) / sizeof(k_audio_fallbacks[0]); ++i)
  {
    if (audio_build_relative_path(module_path, k_audio_fallbacks[i], candidate_directory, sizeof(candidate_directory)) &&
      audio_directory_exists(candidate_directory))
    {
      (void)snprintf(out_path, out_path_size, "%s", candidate_directory);
      return 1;
    }
  }

  return 0;
}

static int audio_load_tracks(AudioState* state)
{
  char audio_directory[PLATFORM_PATH_MAX] = { 0 };

  if (state == NULL || !audio_resolve_audio_directory(audio_directory, sizeof(audio_directory)))
  {
    return 0;
  }

#if defined(_WIN32)
  {
    char search_pattern[PLATFORM_PATH_MAX] = { 0 };
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = INVALID_HANDLE_VALUE;

    (void)snprintf(search_pattern, sizeof(search_pattern), "%s*", audio_directory);
    find_handle = FindFirstFileA(search_pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
    {
      return 0;
    }

    do
    {
      char full_path[PLATFORM_PATH_MAX] = { 0 };
      int score = 0;

      if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
      {
        continue;
      }

      if (!audio_is_supported_extension(find_data.cFileName))
      {
        continue;
      }

      score = audio_score_candidate(find_data.cFileName);
      if (strlen(audio_directory) + strlen(find_data.cFileName) + 1U > sizeof(full_path))
      {
        continue;
      }

      (void)snprintf(full_path, sizeof(full_path), "%s%s", audio_directory, find_data.cFileName);
      if (!audio_add_track(state, full_path, score))
      {
        FindClose(find_handle);
        audio_clear_tracks(state);
        return 0;
      }
    }
    while (FindNextFileA(find_handle, &find_data) != FALSE);

    FindClose(find_handle);
  }
#else
  {
    DIR* directory = opendir(audio_directory);
    struct dirent* entry = NULL;

    if (directory == NULL)
    {
      return 0;
    }

    while ((entry = readdir(directory)) != NULL)
    {
      char full_path[PLATFORM_PATH_MAX] = { 0 };
      int score = 0;

      if (entry->d_name[0] == '.')
      {
        continue;
      }

      if (!audio_is_supported_extension(entry->d_name))
      {
        continue;
      }

      if (strlen(audio_directory) + strlen(entry->d_name) + 1U > sizeof(full_path))
      {
        continue;
      }

      score = audio_score_candidate(entry->d_name);
      (void)snprintf(full_path, sizeof(full_path), "%s%s", audio_directory, entry->d_name);
      if (!audio_add_track(state, full_path, score))
      {
        closedir(directory);
        audio_clear_tracks(state);
        return 0;
      }
    }

    closedir(directory);
  }
#endif

  if (state->track_count == 0U)
  {
    return 0;
  }

  qsort(state->tracks, state->track_count, sizeof(state->tracks[0]), audio_track_compare);
  state->current_track_index = 0U;
  diagnostics_logf("audio: loaded %zu music track(s) from '%s'", state->track_count, audio_directory);
  return 1;
}

static void audio_clear_tracks(AudioState* state)
{
  if (state == NULL)
  {
    return;
  }

  if (state->tracks != NULL)
  {
    free(state->tracks);
    state->tracks = NULL;
  }

  state->track_count = 0U;
  state->track_capacity = 0U;
  state->current_track_index = 0U;
}

static int audio_reserve_tracks(AudioState* player, size_t required_track_capacity)
{
  AudioTrack* resized_tracks = NULL;
  size_t new_capacity = 0U;

  if (player == NULL)
  {
    return 0;
  }

  if (required_track_capacity <= player->track_capacity)
  {
    return 1;
  }

  new_capacity = (player->track_capacity > 0U) ? player->track_capacity : 8U;
  while (new_capacity < required_track_capacity)
  {
    new_capacity *= 2U;
  }

  resized_tracks = (AudioTrack*)realloc(player->tracks, new_capacity * sizeof(player->tracks[0]));
  if (resized_tracks == NULL)
  {
    diagnostics_log("audio: failed to grow music track buffer");
    return 0;
  }

  player->tracks = resized_tracks;
  player->track_capacity = new_capacity;
  return 1;
}

static int audio_add_track(AudioState* player, const char* full_path, int score)
{
  AudioTrack* track = NULL;

  if (player == NULL || full_path == NULL || full_path[0] == '\0')
  {
    return 0;
  }

  if (!audio_reserve_tracks(player, player->track_count + 1U))
  {
    return 0;
  }

  track = &player->tracks[player->track_count];
  memset(track, 0, sizeof(*track));
  (void)snprintf(track->path, sizeof(track->path), "%s", full_path);
  track->score = score;
  player->track_count += 1U;
  return 1;
}

static int audio_track_compare(const void* left, const void* right)
{
  const AudioTrack* left_track = (const AudioTrack*)left;
  const AudioTrack* right_track = (const AudioTrack*)right;
  const char* left_name = NULL;
  const char* right_name = NULL;

  if (left_track == NULL && right_track == NULL)
  {
    return 0;
  }
  if (left_track == NULL)
  {
    return -1;
  }
  if (right_track == NULL)
  {
    return 1;
  }

  if (left_track->score != right_track->score)
  {
    return (left_track->score > right_track->score) ? -1 : 1;
  }

  left_name = audio_get_basename(left_track->path);
  right_name = audio_get_basename(right_track->path);
  return audio_compare_ignore_case(left_name, right_name);
}

static int audio_is_supported_extension(const char* path)
{
  const char* extension = NULL;

  if (path == NULL)
  {
    return 0;
  }

  extension = strrchr(path, '.');
  if (extension == NULL)
  {
    return 0;
  }

  return audio_compare_ignore_case(extension, ".mp3") == 0 ||
    audio_compare_ignore_case(extension, ".wav") == 0 ||
    audio_compare_ignore_case(extension, ".ogg") == 0;
}

static int audio_score_candidate(const char* filename)
{
  int score = 10;

  if (filename == NULL)
  {
    return -1;
  }

  if (audio_string_contains_ignore_case(filename, "theme"))
  {
    score += 100;
  }
  if (audio_string_contains_ignore_case(filename, "bgm"))
  {
    score += 80;
  }
  if (audio_string_contains_ignore_case(filename, "music"))
  {
    score += 60;
  }
  if (audio_string_contains_ignore_case(filename, "opening") ||
    audio_string_contains_ignore_case(filename, "intro") ||
    audio_string_contains_ignore_case(filename, "startup"))
  {
    score += 40;
  }

  return score;
}

static int audio_string_contains_ignore_case(const char* text, const char* needle)
{
  size_t text_length = 0U;
  size_t needle_length = 0U;
  size_t offset = 0U;
  size_t index = 0U;

  if (text == NULL || needle == NULL)
  {
    return 0;
  }

  text_length = strlen(text);
  needle_length = strlen(needle);
  if (needle_length == 0U || needle_length > text_length)
  {
    return 0;
  }

  for (offset = 0U; offset + needle_length <= text_length; ++offset)
  {
    int match = 1;
    for (index = 0U; index < needle_length; ++index)
    {
      if (audio_to_lower_ascii(text[offset + index]) != audio_to_lower_ascii(needle[index]))
      {
        match = 0;
        break;
      }
    }
    if (match != 0)
    {
      return 1;
    }
  }

  return 0;
}

static int audio_compare_ignore_case(const char* a, const char* b)
{
  size_t index = 0U;

  if (a == NULL && b == NULL)
  {
    return 0;
  }
  if (a == NULL)
  {
    return -1;
  }
  if (b == NULL)
  {
    return 1;
  }

  while (a[index] != '\0' && b[index] != '\0')
  {
    const char lower_a = audio_to_lower_ascii(a[index]);
    const char lower_b = audio_to_lower_ascii(b[index]);
    if (lower_a != lower_b)
    {
      return (lower_a < lower_b) ? -1 : 1;
    }
    ++index;
  }

  if (a[index] == b[index])
  {
    return 0;
  }

  return (a[index] == '\0') ? -1 : 1;
}

static char audio_to_lower_ascii(char value)
{
  return (char)tolower((unsigned char)value);
}

#if defined(_WIN32)

static int audio_wave_open_and_play(AudioState* state, const char* path)
{
  AudioWavePlayer* player = NULL;
  WAVEFORMATEX* wave_format = NULL;
  UINT32 wave_format_size = 0U;
  unsigned char* pcm_data = NULL;
  DWORD pcm_size = 0U;
  MMRESULT result = MMSYSERR_NOERROR;

  if (state == NULL || path == NULL || path[0] == '\0')
  {
    return 0;
  }

  if (!audio_wave_decode_file(path, &wave_format, &wave_format_size, &pcm_data, &pcm_size))
  {
    return 0;
  }

  player = (AudioWavePlayer*)calloc(1U, sizeof(*player));
  if (player == NULL)
  {
    CoTaskMemFree(wave_format);
    free(pcm_data);
    diagnostics_log("audio: failed to allocate waveOut player");
    return 0;
  }

  player->pcm_data = pcm_data;
  player->pcm_size = pcm_size;
  result = waveOutOpen(&player->device, WAVE_MAPPER, wave_format, 0U, 0U, CALLBACK_NULL);
  CoTaskMemFree(wave_format);
  if (result != MMSYSERR_NOERROR)
  {
    free(player->pcm_data);
    free(player);
    diagnostics_logf("audio: waveOutOpen failed with code %u", (unsigned)result);
    return 0;
  }

  player->header.lpData = (LPSTR)player->pcm_data;
  player->header.dwBufferLength = player->pcm_size;
  result = waveOutPrepareHeader(player->device, &player->header, sizeof(player->header));
  if (result != MMSYSERR_NOERROR)
  {
    waveOutClose(player->device);
    free(player->pcm_data);
    free(player);
    diagnostics_logf("audio: waveOutPrepareHeader failed with code %u", (unsigned)result);
    return 0;
  }

  player->header_prepared = 1;
  result = waveOutWrite(player->device, &player->header, sizeof(player->header));
  if (result != MMSYSERR_NOERROR)
  {
    waveOutUnprepareHeader(player->device, &player->header, sizeof(player->header));
    waveOutClose(player->device);
    free(player->pcm_data);
    free(player);
    diagnostics_logf("audio: waveOutWrite failed with code %u", (unsigned)result);
    return 0;
  }

  state->native_player = player;
  state->is_open = 1;
  state->target_volume = AUDIO_DEFAULT_TARGET_VOLUME;
  (void)snprintf(state->active_path, sizeof(state->active_path), "%s", path);
  audio_begin_fade_in(state);
  diagnostics_logf("audio: waveOut playback started for '%s' (%u bytes PCM)", audio_get_basename(path), (unsigned)pcm_size);
  (void)wave_format_size;
  return 1;
}

static void audio_wave_stop(AudioState* state)
{
  AudioWavePlayer* player = NULL;

  if (state == NULL || state->native_player == NULL)
  {
    return;
  }

  player = (AudioWavePlayer*)state->native_player;
  if (player->device != NULL)
  {
    (void)waveOutReset(player->device);
    if (player->header_prepared != 0)
    {
      (void)waveOutUnprepareHeader(player->device, &player->header, sizeof(player->header));
    }
    (void)waveOutClose(player->device);
  }

  free(player->pcm_data);
  free(player);
  state->native_player = NULL;
  state->is_open = 0;
}

static int audio_wave_is_playing(const AudioState* state)
{
  const AudioWavePlayer* player = NULL;

  if (state == NULL || state->native_player == NULL)
  {
    return 0;
  }

  player = (const AudioWavePlayer*)state->native_player;
  return (player->header.dwFlags & WHDR_DONE) == 0U;
}

static int audio_wave_set_volume(AudioState* state, int volume)
{
  AudioWavePlayer* player = NULL;
  const DWORD channel_volume = (DWORD)((audio_clamp_int(volume, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX) * 0xFFFF) / AUDIO_VOLUME_MAX);
  const DWORD packed_volume = channel_volume | (channel_volume << 16);

  if (state == NULL || state->native_player == NULL)
  {
    return 0;
  }

  player = (AudioWavePlayer*)state->native_player;
  return waveOutSetVolume(player->device, packed_volume) == MMSYSERR_NOERROR;
}

static int audio_wave_decode_file(const char* path, WAVEFORMATEX** out_format, UINT32* out_format_size, unsigned char** out_pcm_data, DWORD* out_pcm_size)
{
  wchar_t wide_path[PLATFORM_PATH_MAX] = { 0 };
  IMFSourceReader* reader = NULL;
  IMFMediaType* target_type = NULL;
  IMFMediaType* current_type = NULL;
  WAVEFORMATEX* wave_format = NULL;
  UINT32 wave_format_size = 0U;
  unsigned char* pcm_data = NULL;
  DWORD pcm_size = 0U;
  DWORD pcm_capacity = 0U;
  HRESULT hr = S_OK;
  int ok = 0;

  if (out_format == NULL || out_format_size == NULL || out_pcm_data == NULL || out_pcm_size == NULL)
  {
    return 0;
  }

  *out_format = NULL;
  *out_format_size = 0U;
  *out_pcm_data = NULL;
  *out_pcm_size = 0U;

  if (!audio_windows_path_to_wide(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])) ||
    !audio_windows_media_foundation_startup())
  {
    return 0;
  }

  hr = MFCreateSourceReaderFromURL(wide_path, NULL, &reader);
  if (FAILED(hr))
  {
    diagnostics_logf("audio: Media Foundation could not open '%s' (hr=0x%08x)", audio_get_basename(path), (unsigned)hr);
    goto cleanup;
  }

  hr = MFCreateMediaType(&target_type);
  if (FAILED(hr))
  {
    diagnostics_logf("audio: MFCreateMediaType failed (hr=0x%08x)", (unsigned)hr);
    goto cleanup;
  }

  hr = target_type->lpVtbl->SetGUID(target_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
  if (SUCCEEDED(hr))
  {
    hr = target_type->lpVtbl->SetGUID(target_type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
  }
  if (SUCCEEDED(hr))
  {
    hr = target_type->lpVtbl->SetUINT32(target_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16U);
  }
  if (FAILED(hr))
  {
    diagnostics_logf("audio: failed to configure PCM output type (hr=0x%08x)", (unsigned)hr);
    goto cleanup;
  }

  hr = reader->lpVtbl->SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, target_type);
  if (FAILED(hr))
  {
    diagnostics_logf("audio: source reader cannot decode '%s' to PCM (hr=0x%08x)", audio_get_basename(path), (unsigned)hr);
    goto cleanup;
  }

  hr = reader->lpVtbl->GetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current_type);
  if (FAILED(hr))
  {
    diagnostics_logf("audio: failed to query decoded audio type (hr=0x%08x)", (unsigned)hr);
    goto cleanup;
  }

  hr = MFCreateWaveFormatExFromMFMediaType(current_type, &wave_format, &wave_format_size, MFWaveFormatExConvertFlag_Normal);
  if (FAILED(hr) || wave_format == NULL || wave_format_size < sizeof(WAVEFORMATEX))
  {
    diagnostics_logf("audio: failed to build wave format (hr=0x%08x)", (unsigned)hr);
    goto cleanup;
  }

  for (;;)
  {
    DWORD stream_index = 0U;
    DWORD flags = 0U;
    LONGLONG timestamp = 0;
    IMFSample* sample = NULL;

    hr = reader->lpVtbl->ReadSample(
      reader,
      MF_SOURCE_READER_FIRST_AUDIO_STREAM,
      0U,
      &stream_index,
      &flags,
      &timestamp,
      &sample);
    (void)stream_index;
    (void)timestamp;

    if (FAILED(hr))
    {
      diagnostics_logf("audio: failed while decoding '%s' (hr=0x%08x)", audio_get_basename(path), (unsigned)hr);
      if (sample != NULL)
      {
        sample->lpVtbl->Release(sample);
      }
      goto cleanup;
    }

    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0U)
    {
      if (sample != NULL)
      {
        sample->lpVtbl->Release(sample);
      }
      break;
    }

    if (sample != NULL)
    {
      IMFMediaBuffer* buffer = NULL;
      BYTE* sample_data = NULL;
      DWORD max_length = 0U;
      DWORD current_length = 0U;

      hr = sample->lpVtbl->ConvertToContiguousBuffer(sample, &buffer);
      if (SUCCEEDED(hr))
      {
        hr = buffer->lpVtbl->Lock(buffer, &sample_data, &max_length, &current_length);
      }
      if (SUCCEEDED(hr))
      {
        if (!audio_wave_append_pcm(&pcm_data, &pcm_size, &pcm_capacity, sample_data, current_length))
        {
          hr = E_OUTOFMEMORY;
        }
        (void)buffer->lpVtbl->Unlock(buffer);
      }
      if (buffer != NULL)
      {
        buffer->lpVtbl->Release(buffer);
      }
      sample->lpVtbl->Release(sample);

      if (FAILED(hr))
      {
        diagnostics_logf("audio: failed to copy decoded PCM for '%s' (hr=0x%08x)", audio_get_basename(path), (unsigned)hr);
        goto cleanup;
      }
    }
  }

  if (pcm_size == 0U)
  {
    diagnostics_logf("audio: decoded '%s' produced no PCM samples", audio_get_basename(path));
    goto cleanup;
  }

  *out_format = wave_format;
  *out_format_size = wave_format_size;
  *out_pcm_data = pcm_data;
  *out_pcm_size = pcm_size;
  wave_format = NULL;
  pcm_data = NULL;
  ok = 1;

cleanup:
  if (current_type != NULL)
  {
    current_type->lpVtbl->Release(current_type);
  }
  if (target_type != NULL)
  {
    target_type->lpVtbl->Release(target_type);
  }
  if (reader != NULL)
  {
    reader->lpVtbl->Release(reader);
  }
  if (wave_format != NULL)
  {
    CoTaskMemFree(wave_format);
  }
  free(pcm_data);
  return ok;
}

static int audio_wave_append_pcm(unsigned char** data, DWORD* size, DWORD* capacity, const BYTE* source, DWORD source_size)
{
  DWORD required_size = 0U;

  if (data == NULL || size == NULL || capacity == NULL || source == NULL)
  {
    return 0;
  }
  if (source_size == 0U)
  {
    return 1;
  }
  if (*size > 0xFFFFFFFFUL - source_size)
  {
    return 0;
  }

  required_size = *size + source_size;
  if (required_size > *capacity)
  {
    DWORD new_capacity = (*capacity > 0U) ? *capacity : 65536U;
    unsigned char* resized = NULL;

    while (new_capacity < required_size)
    {
      if (new_capacity > 0x7FFFFFFFUL)
      {
        new_capacity = required_size;
        break;
      }
      new_capacity *= 2U;
    }

    resized = (unsigned char*)realloc(*data, (size_t)new_capacity);
    if (resized == NULL)
    {
      return 0;
    }

    *data = resized;
    *capacity = new_capacity;
  }

  memcpy(*data + *size, source, source_size);
  *size = required_size;
  return 1;
}

static int audio_windows_media_foundation_startup(void)
{
  HRESULT hr = S_OK;

  if (g_audio_mf_started != 0)
  {
    return 1;
  }

  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr))
  {
    g_audio_com_initialized = 1;
  }
  else if (hr != RPC_E_CHANGED_MODE)
  {
    diagnostics_logf("audio: CoInitializeEx failed (hr=0x%08x)", (unsigned)hr);
    return 0;
  }

  hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr))
  {
    diagnostics_logf("audio: MFStartup failed (hr=0x%08x)", (unsigned)hr);
    if (g_audio_com_initialized != 0)
    {
      CoUninitialize();
      g_audio_com_initialized = 0;
    }
    return 0;
  }

  g_audio_mf_started = 1;
  return 1;
}

static void audio_windows_media_foundation_shutdown(void)
{
  if (g_audio_mf_started != 0)
  {
    (void)MFShutdown();
    g_audio_mf_started = 0;
  }
  if (g_audio_com_initialized != 0)
  {
    CoUninitialize();
    g_audio_com_initialized = 0;
  }
}

static int audio_windows_path_to_wide(const char* path, wchar_t* out_path, size_t out_path_count)
{
  int converted = 0;

  if (path == NULL || out_path == NULL || out_path_count == 0U || out_path_count > (size_t)INT_MAX)
  {
    return 0;
  }

  out_path[0] = L'\0';
  converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out_path, (int)out_path_count);
  if (converted <= 0)
  {
    converted = MultiByteToWideChar(CP_ACP, 0U, path, -1, out_path, (int)out_path_count);
  }

  return converted > 0;
}

static void audio_normalize_path_separators(char* path)
{
  size_t index = 0U;

  if (path == NULL)
  {
    return;
  }

  for (index = 0U; path[index] != '\0'; ++index)
  {
    if (path[index] == '/')
    {
      path[index] = '\\';
    }
  }
}

static int audio_send_command(const char* command, int log_failure)
{
  return audio_send_command_for_string(command, NULL, 0U, log_failure);
}

static int audio_send_command_for_string(const char* command, char* out_text, size_t out_text_size, int log_failure)
{
  MCIERROR error = 0U;

  if (command == NULL || command[0] == '\0')
  {
    return 0;
  }

  if (out_text != NULL && out_text_size > 0U)
  {
    out_text[0] = '\0';
  }

  error = mciSendStringA(command, out_text, (UINT)out_text_size, NULL);
  if (error == 0U)
  {
    return 1;
  }

  if (log_failure != 0)
  {
    char error_text[256] = { 0 };
    if (mciGetErrorStringA(error, error_text, (UINT)sizeof(error_text)) == FALSE)
    {
      (void)snprintf(error_text, sizeof(error_text), "MCI error %u", (unsigned)error);
    }
    diagnostics_logf("audio: command failed `%s`: %s", command, error_text);
  }

  return 0;
}

static int audio_query_mode(const AudioState* player, char* out_mode, size_t out_mode_size)
{
  char command[AUDIO_COMMAND_LENGTH] = { 0 };

  if (player == NULL || player->is_open == 0 || out_mode == NULL || out_mode_size == 0U)
  {
    return 0;
  }

  (void)snprintf(command, sizeof(command), "status %s mode", k_audio_alias);
  if (!audio_send_command_for_string(command, out_mode, out_mode_size, 0))
  {
    return 0;
  }

  audio_trim_trailing_whitespace(out_mode);
  return 1;
}

static void audio_trim_trailing_whitespace(char* text)
{
  size_t length = 0U;

  if (text == NULL)
  {
    return;
  }

  length = strlen(text);
  while (length > 0U && isspace((unsigned char)text[length - 1U]))
  {
    text[length - 1U] = '\0';
    --length;
  }
}
#endif

static unsigned long long audio_get_tick_ms(void)
{
#if defined(_WIN32)
  return (unsigned long long)GetTickCount64();
#else
  struct timespec now = { 0 };

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
  {
    return 0ULL;
  }

  return ((unsigned long long)now.tv_sec * 1000ULL) + (unsigned long long)(now.tv_nsec / 1000000L);
#endif
}

static int audio_open_and_play(AudioState* player, const char* path)
{
  if (player == NULL || path == NULL || path[0] == '\0')
  {
    return 0;
  }

#if defined(_WIN32)
  {
    char normalized_path[PLATFORM_PATH_MAX] = { 0 };
    char command[AUDIO_COMMAND_LENGTH] = { 0 };
    const char* extension = NULL;

    audio_stop(player);
    if (audio_wave_open_and_play(player, path))
    {
      return 1;
    }

    diagnostics_logf("audio: Media Foundation playback failed for '%s', trying MCI fallback", audio_get_basename(path));
    (void)snprintf(normalized_path, sizeof(normalized_path), "%s", path);
    audio_normalize_path_separators(normalized_path);
    extension = strrchr(normalized_path, '.');

    (void)snprintf(command, sizeof(command), "open \"%s\" alias %s", normalized_path, k_audio_alias);
    if (!audio_send_command(command, 0))
    {
      const char* media_type = NULL;

      if (extension != NULL)
      {
        if (audio_compare_ignore_case(extension, ".wav") == 0)
        {
          media_type = "waveaudio";
        }
        else if (audio_compare_ignore_case(extension, ".mp3") == 0 ||
          audio_compare_ignore_case(extension, ".ogg") == 0)
        {
          media_type = "mpegvideo";
        }
      }

      if (media_type == NULL)
      {
        return 0;
      }

      (void)snprintf(command, sizeof(command), "open \"%s\" type %s alias %s", normalized_path, media_type, k_audio_alias);
      if (!audio_send_command(command, 1))
      {
        return 0;
      }
    }

    player->is_open = 1;
    (void)snprintf(player->active_path, sizeof(player->active_path), "%s", normalized_path);

    (void)snprintf(command, sizeof(command), "play %s from 0", k_audio_alias);
    if (!audio_send_command(command, 1))
    {
      audio_stop(player);
      return 0;
    }

    player->target_volume = AUDIO_DEFAULT_TARGET_VOLUME;
    audio_begin_fade_in(player);
    return 1;
  }
#elif defined(__APPLE__)
  {
    const int repeat = (player->track_count <= 1U) ? 1 : 0;

    audio_stop(player);
    player->target_volume = AUDIO_DEFAULT_TARGET_VOLUME;
    if (!audio_macos_open_and_play(player, path, repeat))
    {
      return 0;
    }

    player->is_open = 1;
    (void)snprintf(player->active_path, sizeof(player->active_path), "%s", path);
    audio_begin_fade_in(player);
    return 1;
  }
#elif defined(__linux__)
  {
    audio_stop(player);
    player->target_volume = AUDIO_DEFAULT_TARGET_VOLUME;
    if (!audio_linux_open_and_play(player, path))
    {
      return 0;
    }

    player->is_open = 1;
    (void)snprintf(player->active_path, sizeof(player->active_path), "%s", path);
    audio_begin_fade_in(player);
    return 1;
  }
#else
  (void)player;
  (void)path;
  return 0;
#endif
}

static int audio_play_track_from(AudioState* player, size_t start_index)
{
  size_t attempt = 0U;

  if (player == NULL || player->track_count == 0U)
  {
    return 0;
  }

  for (attempt = 0U; attempt < player->track_count; ++attempt)
  {
    const size_t track_index = (start_index + attempt) % player->track_count;
    const AudioTrack* track = &player->tracks[track_index];

    if (audio_open_and_play(player, track->path))
    {
      player->current_track_index = track_index;
      diagnostics_logf(
        "audio: playing music track %zu/%zu '%s'",
        track_index + 1U,
        player->track_count,
        player->active_path);
      return 1;
    }
  }

  return 0;
}

static int audio_set_volume(AudioState* state, int volume, int log_failure)
{
  const int clamped_volume = audio_clamp_int(volume, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX);

  if (state == NULL || state->is_open == 0)
  {
    return 0;
  }

#if defined(_WIN32)
  if (state->native_player != NULL)
  {
    if (!audio_wave_set_volume(state, clamped_volume))
    {
      if (log_failure != 0)
      {
        diagnostics_logf("audio: failed to set waveOut volume to %d", clamped_volume);
      }
      return 0;
    }

    state->current_volume = clamped_volume;
    return 1;
  }

  {
    char command[AUDIO_COMMAND_LENGTH] = { 0 };

    (void)snprintf(command, sizeof(command), "setaudio %s volume to %d", k_audio_alias, clamped_volume);
    if (!audio_send_command(command, log_failure))
    {
      return 0;
    }
  }
#elif defined(__APPLE__)
  if (!audio_macos_set_volume(state, clamped_volume))
  {
    if (log_failure != 0)
    {
      diagnostics_logf("audio: failed to set macOS track volume to %d", clamped_volume);
    }
    return 0;
  }
#elif defined(__linux__)
  if (!audio_linux_set_volume(state, clamped_volume))
  {
    if (log_failure != 0)
    {
      diagnostics_logf("audio: failed to set Linux track volume to %d", clamped_volume);
    }
    return 0;
  }
#else
  (void)log_failure;
  return 0;
#endif

  state->current_volume = clamped_volume;
  return 1;
}

static void audio_begin_fade_in(AudioState* state)
{
  if (state == NULL)
  {
    return;
  }

  state->target_volume = audio_clamp_int(state->target_volume, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX);
  state->fade_start_ms = audio_get_tick_ms();
  state->fade_active = 1;
  if (!audio_set_volume(state, AUDIO_VOLUME_MIN, 0))
  {
    state->fade_active = 0;
    state->current_volume = state->target_volume;
  }
}

static void audio_update_fade(AudioState* state)
{
  const unsigned long long now_ms = audio_get_tick_ms();
  unsigned long long elapsed_ms = 0ULL;
  int next_volume = 0;

  if (state == NULL || state->fade_active == 0 || state->is_open == 0)
  {
    return;
  }

  elapsed_ms = now_ms - state->fade_start_ms;
  if (elapsed_ms >= AUDIO_FADE_IN_DURATION_MS)
  {
    (void)audio_set_volume(state, state->target_volume, 0);
    state->fade_active = 0;
    return;
  }

  next_volume = (int)((long long)state->target_volume * (long long)elapsed_ms / (long long)AUDIO_FADE_IN_DURATION_MS);
  next_volume = audio_clamp_int(next_volume, AUDIO_VOLUME_MIN, state->target_volume);
  if (next_volume != state->current_volume)
  {
    (void)audio_set_volume(state, next_volume, 0);
  }
}

static int audio_clamp_int(int value, int min_value, int max_value)
{
  if (value < min_value)
  {
    return min_value;
  }
  if (value > max_value)
  {
    return max_value;
  }
  return value;
}

#if defined(__linux__)
static int audio_linux_open_and_play(AudioState* state, const char* path)
{
  pid_t pid = fork();
  if (pid < 0)
  {
    return 0;
  }

  if (pid == 0)
  {
    int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null != -1)
    {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }
    
    execlp("pw-play", "pw-play", path, NULL);
    execlp("paplay", "paplay", path, NULL);
    execlp("ffplay", "ffplay", "-nodisp", "-autoexit", path, NULL);
    exit(1);
  }

  state->native_player_pid = pid;
  return 1;
}

static void audio_linux_stop(AudioState* state)
{
  if (state->native_player_pid != 0)
  {
    kill(state->native_player_pid, SIGTERM);
    waitpid(state->native_player_pid, NULL, 0);
    state->native_player_pid = 0;
  }
}

static int audio_linux_is_playing(const AudioState* state)
{
  if (state->native_player_pid != 0)
  {
    int status;
    pid_t result = waitpid(state->native_player_pid, &status, WNOHANG);
    if (result == 0)
    {
      return 1;
    }
  }
  return 0;
}

static int audio_linux_set_volume(AudioState* state, int volume)
{
  (void)state;
  (void)volume;
  return 1;
}
#endif
