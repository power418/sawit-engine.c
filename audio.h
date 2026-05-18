#ifndef AUDIO_H
#define AUDIO_H

#include "platform_support.h"

typedef struct AudioTrack
{
  char path[PLATFORM_PATH_MAX];
  int score;
} AudioTrack;

typedef struct AudioState
{
  char active_path[PLATFORM_PATH_MAX];
  AudioTrack* tracks;
  size_t track_count;
  size_t track_capacity;
  size_t current_track_index;
  int is_open;
  int current_volume;
  int target_volume;
  int fade_active;
  unsigned long long fade_start_ms;
#if defined(_WIN32) || defined(__APPLE__)
  void* native_player;
#else
  int native_player_pid;
#endif
} AudioState;

void audio_init(AudioState* state);
int audio_start_music(AudioState* state);
void audio_update(AudioState* state);
void audio_stop(AudioState* state);
void audio_shutdown(AudioState* state);

#endif
