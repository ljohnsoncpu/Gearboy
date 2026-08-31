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

#ifndef GAME_ADAPTATION_H
#define	GAME_ADAPTATION_H

#include <string>
#include <vector>
#include "definitions.h"

// Game adaptation profiles (project ADR 0005).
//
// This fork is effectively new hardware, and a game is ported to it. The
// artifact a user receives is the emulator; the user supplies their own legally
// obtained, unmodified ROM. Where an extended viewport needs the game itself to
// behave differently, the emulator makes that adjustment **in memory at load
// time**, keyed by the SHA-256 of the image the user supplied. Nobody has to
// patch a ROM.
//
// Two rules keep that honest:
//
//   1. An adaptation is only ever applied to a ROM this table has identified by
//      its exact digest, and only when the extended viewport is enabled. An
//      unrecognised ROM still gets the wide renderer and gets no adaptation.
//   2. Everything applied is reported - the digest of the file that was loaded,
//      the digest after adaptation, the profile identity, and every individual
//      site. A capture can therefore record both the ROM the user brought and
//      what this emulator did to it, which is the property a single patched ROM
//      file used to provide by being hashable.
//
// The site table mirrors `tools/build/rom_patches.py` in the project repository,
// which holds the reverse-engineering rationale for each byte.
// `tests/smoke/test_game_adaptation.py` parses this table out of
// `GameAdaptation.cpp` and fails if the two lists ever disagree.

struct GameAdaptationSite
{
    const char* name;
    const char* site;          // bank:address of the instruction, for cross-reference
    u32 file_offset;           // the immediate operand's offset in the ROM image
    u8 vanilla;                // the byte that must already be there
    u8 adapted;                // the byte this profile writes
};

// Where a recognised game keeps its horizontal level bounds (project ADR 0009).
//
// A widened viewport can see past the ends of a level: at camera 0 the whole
// left margin sits at a negative world X, and the background ring holds
// whatever world column shares its ring slot - level content from 224 pixels
// ahead at a level start, and, once the game's own backward column streaming
// underflows below camera 32, bytes read from outside the level map entirely.
// Neither is a picture of the level, and no clip or streaming constant can make
// one, because there is nothing there to draw.
//
// So the renderer needs one thing the ROM digest alone cannot tell it: which
// output columns are outside the level right now. These are the addresses that
// answer that, and they are read - never written. Unlike a site table this is
// per-game rather than per-width, because a level's extent does not depend on
// how much of it is on screen.
struct GameLevelBounds
{
    u16 camera_x_low;             // H_CameraXLow
    u16 camera_x_high;            // H_CameraXHigh
    u16 screen_count;             // W_SubLvScreenCount; the level is (n + 1) screens
    u16 game_state;               // H_GameState
    // The scroll pair a HUD raster forces, or (0x100, 0x100) for a game with no
    // such band. A scanline drawn with it has no camera of its own - its scroll
    // is a raster effect - so the level-edge clamp leaves it alone rather than
    // clamping the status bar against a camera it does not have.
    u16 hud_raster_scx;
    u16 hud_raster_scy;
    // The states in which the three addresses above describe a loaded level.
    // Outside them the renderer masks nothing, so an unrecognised state can
    // only ever leave the vanilla picture - never blank a menu or a title
    // screen.
    const u8* gameplay_states;
    int gameplay_state_count;
};

struct GameAdaptationProfile
{
    const char* id;
    const char* description;
    const char* rom_sha256;        // the vanilla image this profile is keyed to
    const char* adapted_sha256;    // the image applying every site produces
    int margin_pixels;
    const GameAdaptationSite* sites;
    int site_count;
    const GameLevelBounds* level_bounds;
};

struct GameAdaptationAppliedSite
{
    std::string name;
    std::string site;
    u32 file_offset;
    u8 from;
    u8 to;
};

struct GameAdaptationState
{
    bool rom_loaded;
    bool wide_mode;
    bool enabled;
    bool matched;
    // One of: "no-rom", "wide-mode-disabled", "adaptation-disabled",
    // "unrecognised-rom", "site-mismatch", "applied".
    std::string reason;
    std::string rom_sha256;        // the image as the user supplied it
    std::string adapted_sha256;    // after adaptation; equals rom_sha256 if none
    std::string profile_id;
    std::string profile_description;
    int margin_pixels;
    bool adapted_sha256_matches_profile;
    // Every applied site still holds its adapted byte in the cartridge image.
    // Re-checked on every query, so a reset that quietly restored the ROM would
    // show up here instead of silently running the unadapted game.
    bool live_verified;
    std::vector<GameAdaptationAppliedSite> applied;
    // The level-edge fill is a RENDERER behaviour keyed to the same recognised
    // image, and it is deliberately independent of the site table above: it is
    // reported here because this is where a capture records what the emulator
    // knows about the game, but `--no-game-adaptation` does not turn it off and
    // `--no-level-edge-fill` does not turn the site table off. See ADR 0009.
    bool level_bounds_known;      // the recognised profile carries bounds
    bool level_edge_fill_enabled; // and `--no-level-edge-fill` was not passed
    // One of: "no-rom", "wide-mode-disabled", "unrecognised-rom",
    // "fill-disabled", "active".
    std::string level_edge_fill_reason;

    GameAdaptationState()
        : rom_loaded(false), wide_mode(false), enabled(true), matched(false),
          reason("no-rom"), margin_pixels(0),
          adapted_sha256_matches_profile(false), live_verified(false),
          level_bounds_known(false), level_edge_fill_enabled(true),
          level_edge_fill_reason("no-rom")
    {
    }
};

namespace GameAdaptation
{
    std::string Sha256Hex(const u8* data, size_t size);
    // A profile is keyed by the image's digest AND the viewport margin: one
    // game has one profile per width, because the clip windows and the spawn
    // frontier are both functions of the width. See project ADR 0007.
    const GameAdaptationProfile* FindProfile(const std::string& rom_sha256,
                                             int margin_pixels);
    int ProfileCount();
    const GameAdaptationProfile* ProfileAt(int index);

    // Identify `rom` and, in wide mode only, apply the profile matching that
    // image at `margin_pixels` in place. Always fills `out`, including for the
    // cases where nothing is applied. Never writes to a ROM it has not
    // identified, and never writes a profile meant for a different width.
    //
    // `enabled` is the `--no-game-adaptation` escape hatch inverted. It exists
    // so the wide renderer can still be measured against the game exactly as
    // the user supplied it - which is what M5 did when the adaptation lived in
    // a second ROM file - and so a scenario can be mutation-checked by running
    // it against the unadapted game. It never changes the picture's geometry,
    // only whether the game itself is adapted.
    void Apply(u8* rom, int size, bool wide_mode, int margin_pixels,
               bool enabled, GameAdaptationState& out);

    // Re-read every applied site and update `state.live_verified`.
    void Verify(const u8* rom, int size, GameAdaptationState& state);

    // Decide whether the level-edge fill runs for the state `Apply` produced,
    // fill in its three fields, and return the bounds the renderer should use
    // (NULL for "fill nothing"). Deliberately looked up by digest and margin
    // alone: it does NOT depend on whether the site table was applied, so
    // `--no-game-adaptation` still fills the edges and a wide scenario can
    // mutate one lever without moving the other. See project ADR 0009.
    const GameLevelBounds* ResolveLevelEdgeFill(bool fill_enabled,
                                                int margin_pixels,
                                                GameAdaptationState& state);
}

#endif	/* GAME_ADAPTATION_H */
