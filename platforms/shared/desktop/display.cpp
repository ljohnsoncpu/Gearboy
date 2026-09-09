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

#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "gearboy.h"
#include "config.h"
#include "gui.h"
#include "ogl_renderer.h"
#include "emu.h"
#include "application.h"

#define DISPLAY_IMPORT
#include "display.h"

static Uint64 frame_time_start = 0;
static Uint64 frame_time_end = 0;
static Uint64 frame_time_start_ns = 0;
static Uint64 frame_pacing_deadline_ns = 0;
// WSLg ignores OpenGL swap interval on the measured path. Its compositor still
// has a precise near-60 Hz clock, so the launcher can request software-paced
// submissions at that clock while this integer phase preserves Game Boy time.
static Uint64 compositor_frame_ns = 0;
static Uint64 compositor_emu_phase = 0;
static Uint64 compositor_emu_step = 0;
static Uint64 compositor_emu_threshold = 0;
static unsigned int compositor_repeats_since_report = 0;
static bool compositor_pacing_enabled = false;
static int monitor_refresh_rate = 60;
static int vsync_frames_per_emu_frame = 1;
static int vsync_frame_counter = 0;
static int last_vsync_state = -1;
static bool multi_monitor_mixed_refresh = false;
static bool pending_gl_context_recreate = false;
static Uint64 pointer_event_timestamp_ns = 0;
static Uint64 pointer_event_serial = 0;
static Uint64 pointer_submitted_serial = 0;
static Uint64 pointer_queue_total_ns = 0;
static Uint64 pointer_queue_max_ns = 0;
static Uint64 pointer_submit_total_ns = 0;
static Uint64 pointer_submit_max_ns = 0;
static unsigned int pointer_queue_samples = 0;
static unsigned int pointer_submit_samples = 0;

static bool display_is_vrr_enabled(void);
static void display_set_swap_interval(bool enabled);

void display_begin_frame(void)
{
    frame_time_start = SDL_GetPerformanceCounter();
    frame_time_start_ns = SDL_GetTicksNS();
}

void display_note_pointer_event(Uint64 timestamp_ns)
{
    if (!SDL_getenv("GEARBOY_UI_LATENCY_DIAGNOSTICS"))
        return;

    const Uint64 now_ns = SDL_GetTicksNS();
    const Uint64 queue_ns = now_ns > timestamp_ns ? now_ns - timestamp_ns : 0;
    pointer_event_timestamp_ns = timestamp_ns;
    pointer_event_serial++;
    pointer_queue_total_ns += queue_ns;
    if (queue_ns > pointer_queue_max_ns)
        pointer_queue_max_ns = queue_ns;
    pointer_queue_samples++;
}

void display_render(void)
{
    const bool diagnostics = SDL_getenv("GEARBOY_TIMING_DIAGNOSTICS") != NULL;
    const Uint64 render_start = diagnostics ? SDL_GetTicksNS() : 0;
    ogl_renderer_begin_render();
    ImGui_ImplSDL3_NewFrame();
    gui_render();
    ogl_renderer_render();
    ogl_renderer_end_render();

    const Uint64 swap_start = diagnostics ? SDL_GetTicksNS() : 0;
    SDL_GL_SwapWindow(application_sdl_window);
    const Uint64 swap_end = diagnostics ? SDL_GetTicksNS() : 0;

    // Swap can return while WSLg still has multiple completed frames queued.
    // Waiting here bounds that queue without changing emulation cadence.
    const bool low_latency_present = SDL_getenv("GEARBOY_LOW_LATENCY_PRESENT") != NULL;
    const Uint64 finish_start = diagnostics ? SDL_GetTicksNS() : 0;
    if (low_latency_present)
        ogl_renderer_finish();
    const Uint64 finish_end = diagnostics ? SDL_GetTicksNS() : 0;

    if (pointer_event_serial != pointer_submitted_serial)
    {
        const Uint64 now_ns = SDL_GetTicksNS();
        const Uint64 submit_ns = now_ns > pointer_event_timestamp_ns ? now_ns - pointer_event_timestamp_ns : 0;
        pointer_submit_total_ns += submit_ns;
        if (submit_ns > pointer_submit_max_ns)
            pointer_submit_max_ns = submit_ns;
        pointer_submit_samples++;
        pointer_submitted_serial = pointer_event_serial;
    }

    if (SDL_getenv("GEARBOY_TIMING_DIAGNOSTICS"))
    {
        static Uint64 timing_start_ns = SDL_GetTicksNS();
        static Uint64 timing_last_present_ns = 0;
        static Uint64 timing_min_interval_ns = 0;
        static Uint64 timing_max_interval_ns = 0;
        static unsigned int timing_frames = 0;
        static Uint64 render_ns = 0, swap_ns = 0, finish_ns = 0;
        static unsigned int timing_early_intervals = 0;
        static unsigned int timing_late_intervals = 0;
        const Uint64 game_boy_frame_ns = ((Uint64)GAMEBOY_CLOCKS_PER_FRAME * SDL_NS_PER_SECOND
            + GEARBOY_MASTER_CLOCK_RATE / 2) / GEARBOY_MASTER_CLOCK_RATE;
        const Uint64 expected_ns = compositor_pacing_enabled ? compositor_frame_ns : game_boy_frame_ns;
        Uint64 timing_now_ns = SDL_GetTicksNS();
        render_ns += swap_start - render_start;
        swap_ns += swap_end - swap_start;
        finish_ns += finish_end - finish_start;

        if (timing_last_present_ns != 0)
        {
            Uint64 interval_ns = timing_now_ns - timing_last_present_ns;
            if (timing_min_interval_ns == 0 || interval_ns < timing_min_interval_ns)
                timing_min_interval_ns = interval_ns;
            if (interval_ns > timing_max_interval_ns)
                timing_max_interval_ns = interval_ns;
            if (interval_ns < expected_ns / 2)
                timing_early_intervals++;
            if (interval_ns > expected_ns * 3 / 2)
                timing_late_intervals++;
        }

        timing_last_present_ns = timing_now_ns;
        timing_frames++;
        Uint64 timing_elapsed_ns = timing_now_ns - timing_start_ns;
        if (timing_elapsed_ns >= SDL_NS_PER_SECOND)
        {
            Log("Timing: presented %.2f frames/s, submit intervals %.3f-%.3f ms, early %u, late %u, repeats %u",
                (double)timing_frames * (double)SDL_NS_PER_SECOND / (double)timing_elapsed_ns,
                (double)timing_min_interval_ns / 1000000.0,
                (double)timing_max_interval_ns / 1000000.0,
                timing_early_intervals, timing_late_intervals,
                compositor_repeats_since_report);
            Log("Timing stages: render %.3f ms/frame, swap %.3f ms/frame, present drain %.3f ms/frame",
                (double)render_ns / timing_frames / 1e6, (double)swap_ns / timing_frames / 1e6,
                (double)finish_ns / timing_frames / 1e6);
            if (pointer_queue_samples || pointer_submit_samples)
                Log("UI latency: pointer queue %.3f/%.3f ms avg/max, event-to-submit %.3f/%.3f ms avg/max, queue limiter %s",
                    pointer_queue_samples ? (double)pointer_queue_total_ns / pointer_queue_samples / 1e6 : 0.0,
                    (double)pointer_queue_max_ns / 1e6,
                    pointer_submit_samples ? (double)pointer_submit_total_ns / pointer_submit_samples / 1e6 : 0.0,
                    (double)pointer_submit_max_ns / 1e6, low_latency_present ? "on" : "off");
            render_ns = swap_ns = finish_ns = 0;
            pointer_queue_total_ns = pointer_queue_max_ns = 0;
            pointer_submit_total_ns = pointer_submit_max_ns = 0;
            pointer_queue_samples = pointer_submit_samples = 0;
            timing_start_ns = timing_now_ns;
            timing_frames = 0;
            timing_min_interval_ns = 0;
            timing_max_interval_ns = 0;
            timing_early_intervals = 0;
            timing_late_intervals = 0;
            compositor_repeats_since_report = 0;
        }
    }
}

void display_frame_throttle(void)
{
    frame_time_end = SDL_GetPerformanceCounter();

    bool active = !emu_is_empty() && !emu_is_paused() && !emu_is_debug_idle() && !config_emulator.ffwd;
    bool timer_paced = active && (config_video.sync_mode == config_VideoSync_Disabled || vsync_frames_per_emu_frame == 1);

    if (timer_paced)
    {
        const Uint64 game_boy_frame_ns = ((Uint64)GAMEBOY_CLOCKS_PER_FRAME * SDL_NS_PER_SECOND + GEARBOY_MASTER_CLOCK_RATE / 2) / GEARBOY_MASTER_CLOCK_RATE;
        const Uint64 frame_ns = compositor_pacing_enabled ? compositor_frame_ns : game_boy_frame_ns;

        if (frame_pacing_deadline_ns == 0 || frame_time_start_ns > frame_pacing_deadline_ns + frame_ns * 4)
            frame_pacing_deadline_ns = frame_time_start_ns + frame_ns;
        else
            frame_pacing_deadline_ns += frame_ns;

        Uint64 now_ns = SDL_GetTicksNS();
        if (now_ns < frame_pacing_deadline_ns)
            SDL_DelayPrecise(frame_pacing_deadline_ns - now_ns);
        return;
    }

    frame_pacing_deadline_ns = 0;

    if (emu_is_empty() || emu_is_paused() || emu_is_debug_idle() || !emu_is_audio_open() || config_emulator.ffwd)
    {
        Uint64 count_per_sec = SDL_GetPerformanceFrequency();
        float elapsed = (float)(frame_time_end - frame_time_start) / (float)count_per_sec;
        elapsed *= 1000.0f;

        float min = 16.666f;

        if (config_emulator.ffwd)
        {
            switch (config_emulator.ffwd_speed)
            {
                case 0:
                    min = 16.666f / 1.5f;
                    break;
                case 1:
                    min = 16.666f / 2.0f;
                    break;
                case 2:
                    min = 16.666f / 2.5f;
                    break;
                case 3:
                    min = 16.666f / 3.0f;
                    break;
                default:
                    min = 0.0f;
            }
        }

        if (elapsed < min)
            SDL_Delay((Uint32)(min - elapsed));
    }
}

bool display_should_run_emu_frame(void)
{
    bool active = !emu_is_empty() && !emu_is_paused() && !emu_is_debug_idle()
        && !config_emulator.ffwd;

    if (compositor_pacing_enabled && config_video.sync_mode == config_VideoSync_Disabled && active)
    {
        // Usually advance one Game Boy frame. Repeat the last completed frame
        // only when the faster compositor clock crosses this exact ratio.
        compositor_emu_phase += compositor_emu_step;
        if (compositor_emu_phase >= compositor_emu_threshold)
        {
            compositor_emu_phase -= compositor_emu_threshold;
            return true;
        }

        compositor_repeats_since_report++;
        return false;
    }

    if (config_video.sync_mode != config_VideoSync_Disabled && !emu_is_empty() && !emu_is_paused()
        && !emu_is_debug_idle() && emu_is_audio_open() && !config_emulator.ffwd)
    {
        if (display_is_vrr_enabled())
            return true;

        bool should_run = (vsync_frame_counter == 0);
        vsync_frame_counter++;
        if (vsync_frame_counter >= vsync_frames_per_emu_frame)
            vsync_frame_counter = 0;
        return should_run;
    }

    return true;
}

void display_use_vsync_if_enabled(void)
{
    bool effective = config_video.sync_mode != config_VideoSync_Disabled && !display_is_vsync_forced_off();
    display_set_swap_interval(effective);
    display_update_frame_pacing();
}

void display_disable_vsync(void)
{
    display_set_swap_interval(false);
    display_update_frame_pacing();
}

void display_update_frame_pacing(void)
{
    SDL_DisplayID display = SDL_GetDisplayForWindow(application_sdl_window);

    if (display == 0)
        display = SDL_GetPrimaryDisplay();

    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    float precise_refresh_rate = 60.0f;
    int refresh_rate_numerator = 0;
    int refresh_rate_denominator = 0;
    if (mode && mode->refresh_rate > 0)
    {
        precise_refresh_rate = mode->refresh_rate;
        refresh_rate_numerator = mode->refresh_rate_numerator;
        refresh_rate_denominator = mode->refresh_rate_denominator;
        monitor_refresh_rate = (int)mode->refresh_rate;
    }
    else
        monitor_refresh_rate = 60;

    const char* compositor_pacing = SDL_getenv("GEARBOY_COMPOSITOR_PACING");
    bool compositor_pacing_requested = compositor_pacing && compositor_pacing[0] != 0
        && SDL_strcmp(compositor_pacing, "0") != 0;
    bool compositor_mode_valid = refresh_rate_numerator > 0 && refresh_rate_denominator > 0
        && precise_refresh_rate >= 55.0f && precise_refresh_rate <= 65.0f;

    compositor_pacing_enabled = compositor_pacing_requested && compositor_mode_valid
        && config_video.sync_mode == config_VideoSync_Disabled;
    if (compositor_pacing_enabled)
    {
        compositor_frame_ns = ((Uint64)refresh_rate_denominator * SDL_NS_PER_SECOND
            + refresh_rate_numerator / 2) / refresh_rate_numerator;
        compositor_emu_step = (Uint64)GEARBOY_MASTER_CLOCK_RATE * refresh_rate_denominator;
        compositor_emu_threshold = (Uint64)GAMEBOY_CLOCKS_PER_FRAME * refresh_rate_numerator;
        compositor_emu_phase = compositor_emu_threshold - 1;
    }
    else
    {
        compositor_frame_ns = 0;
        compositor_emu_step = 0;
        compositor_emu_threshold = 0;
        compositor_emu_phase = 0;
    }
    frame_pacing_deadline_ns = 0;

    const int emu_fps = 60;

    if (monitor_refresh_rate <= emu_fps + 5)
        vsync_frames_per_emu_frame = 1;
    else
        vsync_frames_per_emu_frame = (monitor_refresh_rate + emu_fps / 2) / emu_fps;

    if (display_is_vrr_enabled())
        vsync_frames_per_emu_frame = 1;

    vsync_frames_per_emu_frame = CLAMP(vsync_frames_per_emu_frame, 1, 8);

    vsync_frame_counter = 0;

#if defined(_WIN32)
    Debug("Monitor refresh rate: %.3f Hz (%d/%d), vsync frames per emu frame: %d%s", precise_refresh_rate, refresh_rate_numerator, refresh_rate_denominator, vsync_frames_per_emu_frame, display_is_vrr_enabled() ? " (VRR)" : "");
#else
    Log("Monitor refresh rate: %.3f Hz (%d/%d), vsync frames per emu frame: %d", precise_refresh_rate, refresh_rate_numerator, refresh_rate_denominator, vsync_frames_per_emu_frame);
    if (compositor_pacing_enabled)
        Log("Compositor pacing enabled: display %.6f Hz, emulation %.4f Hz",
            (double)refresh_rate_numerator / refresh_rate_denominator,
            (double)GEARBOY_MASTER_CLOCK_RATE / GAMEBOY_CLOCKS_PER_FRAME);
    else if (compositor_pacing_requested && !compositor_mode_valid)
        Log("Compositor pacing unavailable for reported display mode %.3f Hz (%d/%d)",
            precise_refresh_rate, refresh_rate_numerator, refresh_rate_denominator);
#endif
}

void display_check_mixed_refresh_rates(void)
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);

    if (!displays || count <= 1)
    {
        if (displays)
            SDL_free(displays);
        multi_monitor_mixed_refresh = false;
        return;
    }

    int first_rate = 0;
    bool mixed = false;

    for (int i = 0; i < count; i++)
    {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displays[i]);
        if (mode && mode->refresh_rate > 0)
        {
            int rate = (int)mode->refresh_rate;
            if (first_rate == 0)
                first_rate = rate;
            else if (rate != first_rate)
            {
                mixed = true;
                break;
            }
        }
    }

    SDL_free(displays);

    if (mixed != multi_monitor_mixed_refresh)
    {
        multi_monitor_mixed_refresh = mixed;
        if (mixed)
            Log("Multiple monitors with different refresh rates detected");

        if (display_is_vsync_forced_off())
        {
            display_set_swap_interval(false);
            Debug("Vsync forced off: multi-viewport with mixed refresh rate monitors");
        }
        else if (config_video.sync_mode != config_VideoSync_Disabled)
        {
            display_use_vsync_if_enabled();
        }
    }
}

bool display_is_vsync_forced_off(void)
{
    return config_debug.debug && config_debug.multi_viewport && multi_monitor_mixed_refresh;
}

void display_request_gl_context_recreate(void)
{
    pending_gl_context_recreate = true;
}

void display_recreate_gl_context(void)
{
    ogl_renderer_destroy();
    ImGui_ImplSDL3_Shutdown();

    SDL_GLContext old_context = display_gl_context;
    display_gl_context = SDL_GL_CreateContext(application_sdl_window);

    if (display_gl_context)
    {
        SDL_GL_MakeCurrent(application_sdl_window, display_gl_context);
        SDL_GL_DestroyContext(old_context);

        bool enable_vsync = config_video.sync_mode != config_VideoSync_Disabled && !display_is_vsync_forced_off();
        display_set_swap_interval(enable_vsync);

        ImGui_ImplSDL3_InitForOpenGL(application_sdl_window, display_gl_context);
        ogl_renderer_init();
        display_update_frame_pacing();
    }
}

static bool display_is_vrr_enabled(void)
{
#if defined(_WIN32)
    return config_video.sync_mode == config_VideoSync_VRR;
#else
    return false;
#endif
}

static void display_set_swap_interval(bool enabled)
{
    const bool applied = SDL_GL_SetSwapInterval(enabled ? 1 : 0);
    if (SDL_getenv("GEARBOY_TIMING_DIAGNOSTICS"))
    {
        int actual = -99;
        const bool queried = SDL_GL_GetSwapInterval(&actual);
        Log("Timing swap interval: requested %d, applied %d, queried %d, actual %d",
            enabled ? 1 : 0, applied, queried, actual);
    }

    last_vsync_state = enabled ? 1 : 0;
}
