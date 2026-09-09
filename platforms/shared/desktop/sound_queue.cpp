/*
 * Gearboy - Nintendo Game Boy Emulator
 * Copyright (C) 2012  Ignacio Sanchez

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#include <string>
#include <string.h>

#define SOUND_QUEUE_IMPORT
#include "sound_queue.h"
#include "utils.h"

#define SOUND_QUEUE_DEBUG(...) { }
//#define SOUND_QUEUE_DEBUG(x, ...) Debug(x, ## __VA_ARGS__)

static SDL_AudioStream* sound_queue_stream;
static bool sound_queue_sound_open;
static bool sound_queue_playback_started;
static int sound_queue_max_queued_bytes;
static int sound_queue_buffer_size;
static int sound_queue_bytes_per_second;
static s16* sound_queue_last_written;
static bool sound_queue_wsl;

static bool is_running_in_wsl(void);

void sound_queue_init(void)
{
    InitPointer(sound_queue_stream);
    InitPointer(sound_queue_last_written);
    sound_queue_sound_open = false;
    sound_queue_playback_started = false;
    sound_queue_wsl = false;

    int audio_drivers_count = SDL_GetNumAudioDrivers();

    Debug("Sound Queue: %d audio drivers", audio_drivers_count);

    for (int i = 0; i < audio_drivers_count; i++)
    {
        Log("Sound Queue: Driver [%s]", SDL_GetAudioDriver(i));
    }

    std::string platform = SDL_GetPlatform();
    if (platform == "Linux")
    {
        if (is_running_in_wsl())
        {
            Debug("Sound Queue: Running in WSL");
            sound_queue_wsl = true;
            if (!SDL_getenv("SDL_AUDIODRIVER"))
                SDL_SetHint("SDL_AUDIODRIVER", "pulseaudio");
        }
        else
        {
            Debug("Sound Queue: Running in Linux");
        }
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        SDL_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO)");

    Log("Sound Queue: [%s] driver selected", SDL_GetCurrentAudioDriver());

    int audio_devices_count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&audio_devices_count);

    Debug("Sound Queue: %d audio devices", audio_devices_count);

    if (devices)
    {
        for (int i = 0; i < audio_devices_count; i++)
        {
            Log("Sound Queue: Device [%s]", SDL_GetAudioDeviceName(devices[i]));
        }
        SDL_free(devices);
    }
}

void sound_queue_destroy(void)
{
    sound_queue_stop();
}

bool sound_queue_start(int sample_rate, int channel_count, int buffer_size, int buffer_count)
{
    Debug("Sound Queue: Starting with %d Hz, %d channels, %d buffer size, %d buffers ...", sample_rate, channel_count, buffer_size, buffer_count);

    sound_queue_buffer_size = buffer_size;
    sound_queue_max_queued_bytes = buffer_size * buffer_count * (int)sizeof(s16);
    sound_queue_bytes_per_second = sample_rate * channel_count * (int)sizeof(s16);

    sound_queue_last_written = new s16[buffer_size];
    memset(sound_queue_last_written, 0, buffer_size * sizeof(s16));

    SDL_AudioSpec spec;
    spec.freq = sample_rate;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channel_count;

    Debug("Sound Queue: Spec - frequency: %d format: 0x%04X channels: %d", spec.freq, spec.format, spec.channels);

    sound_queue_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);

    if (!sound_queue_stream)
    {
        SDL_ERROR("SDL_OpenAudioDeviceStream");
        return false;
    }

    SDL_AudioDeviceID selected_device = SDL_GetAudioStreamDevice(sound_queue_stream);
    SDL_AudioSpec source_spec;
    SDL_AudioSpec device_spec;
    if (SDL_GetAudioStreamFormat(sound_queue_stream, &source_spec, &device_spec))
        Log("Sound Queue: Stream %d Hz -> device %d Hz", source_spec.freq, device_spec.freq);

    Log("Sound Queue: Started [%s] - frequency: %d format: 0x%04X channels: %d", SDL_GetAudioDeviceName(selected_device), spec.freq, spec.format, spec.channels);

    // WSL audio is intentionally decoupled from video pacing. Seed one silent
    // emulation buffer so device-sized pulls cannot race the first few frames.
    if (sound_queue_wsl)
        SDL_PutAudioStreamData(sound_queue_stream, sound_queue_last_written, buffer_size * (int)sizeof(s16));

    sound_queue_playback_started = false;
    sound_queue_sound_open = true;

    return true;
}

void sound_queue_stop(void)
{
    if (sound_queue_sound_open)
    {
        sound_queue_sound_open = false;
        sound_queue_playback_started = false;
        if (sound_queue_stream)
        {
            SDL_PauseAudioStreamDevice(sound_queue_stream);
            SDL_DestroyAudioStream(sound_queue_stream);
            InitPointer(sound_queue_stream);
        }

        Debug("Sound Queue: Stopped");
    }

    SafeDeleteArray(sound_queue_last_written);
}

int sound_queue_get_sample_count(void)
{
    if (!sound_queue_stream)
        return 0;
    return SDL_GetAudioStreamQueued(sound_queue_stream) / (int)sizeof(s16);
}

s16* sound_queue_get_currently_playing(void)
{
    return sound_queue_last_written;
}

bool sound_queue_is_open(void)
{
    return sound_queue_sound_open;
}

void sound_queue_write(s16* samples, int count, bool sync)
{
    if (!sound_queue_sound_open || !sound_queue_stream)
        return;

    int bytes = count * (int)sizeof(s16);
    int queued = SDL_GetAudioStreamQueued(sound_queue_stream);

    static Uint64 diagnostic_start_ns = 0;
    static Uint64 diagnostic_wait_ns = 0;
    static unsigned int diagnostic_underruns = 0;
    static unsigned int diagnostic_overruns = 0;
    bool diagnostics = SDL_getenv("GEARBOY_TIMING_DIAGNOSTICS") != NULL;
    if (diagnostics)
    {
        Uint64 now_ns = SDL_GetTicksNS();
        if (diagnostic_start_ns == 0)
            diagnostic_start_ns = now_ns;
        else if (now_ns - diagnostic_start_ns >= SDL_NS_PER_SECOND)
        {
            Log("Timing: audio underruns %u/s, overruns %u/s, queued %d bytes", diagnostic_underruns, diagnostic_overruns, queued);
            Log("Timing stages: audio sync wait %.3f ms/s", (double)diagnostic_wait_ns / 1e6);
            diagnostic_wait_ns = 0;
            diagnostic_start_ns = now_ns;
            diagnostic_underruns = 0;
            diagnostic_overruns = 0;
        }
    }

    if (count > sound_queue_buffer_size)
    {
        Log("Sound Queue: Write exceeds queue buffer size (%d > %d)", count, sound_queue_buffer_size);
    }

    if (queued == 0)
    {
        SOUND_QUEUE_DEBUG("Sound Queue: Underrun detected, queue was empty");
        if (diagnostics)
            diagnostic_underruns++;
    }

    if (sync && !sound_queue_wsl)
    {
        int room = sound_queue_max_queued_bytes - queued;
        if (room < bytes)
        {
            SOUND_QUEUE_DEBUG("Sound Queue: Sync wait, need %d bytes but only %d free (queued %d, max %d)", bytes, room, queued, sound_queue_max_queued_bytes);
            int needed = bytes - room;
            Uint64 wait_ns = ((Uint64)needed * SDL_NS_PER_SECOND) / (Uint64)sound_queue_bytes_per_second;
            if (wait_ns > 0)
            {
                const Uint64 start = diagnostics ? SDL_GetTicksNS() : 0;
                SDL_DelayPrecise(wait_ns);
                if (diagnostics) diagnostic_wait_ns += SDL_GetTicksNS() - start;
            }
        }
    }
    else
    {
        int queue_limit = sound_queue_wsl ? sound_queue_max_queued_bytes * 4 : sound_queue_max_queued_bytes;
        if (queued >= queue_limit)
        {
            SOUND_QUEUE_DEBUG("Sound Queue: Async overrun, dropping frame (queued %d >= max %d)", queued, sound_queue_max_queued_bytes);
            if (diagnostics)
                diagnostic_overruns++;
            return;
        }
    }

    SDL_PutAudioStreamData(sound_queue_stream, samples, bytes);

    if (!sound_queue_playback_started)
    {
        SDL_ResumeAudioStreamDevice(sound_queue_stream);
        sound_queue_playback_started = true;
    }

    int copy_count = count < sound_queue_buffer_size ? count : sound_queue_buffer_size;
    memcpy(sound_queue_last_written, samples + (count - copy_count), copy_count * sizeof(s16));
}

static bool is_running_in_wsl(void)
{
    FILE *file;

    if ((file = fopen("/proc/sys/fs/binfmt_misc/WSLInterop", "r")))
    {
        fclose(file);
        return true;
    }

    return false;
}
