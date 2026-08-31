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

#include <string.h>
#include <stdio.h>
#include "GameAdaptation.h"
#include "log.h"

//------------------------------------------------------------------------------
// SHA-256. A ROM is identified by its exact digest and by nothing else - not by
// a filename, not by a header title, not by a CRC that a second image could
// share. Small, self-contained, and only ever run once per ROM load.
//------------------------------------------------------------------------------

namespace
{

struct Sha256Context
{
    u32 state[8];
    u64 bit_count;
    u8 buffer[64];
    size_t buffer_used;
};

const u32 k_sha256_k[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u,
    0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u,
    0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
    0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au,
    0x5B9CCA4Fu, 0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

inline u32 RotateRight(u32 value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void Sha256Init(Sha256Context& ctx)
{
    ctx.state[0] = 0x6A09E667u; ctx.state[1] = 0xBB67AE85u;
    ctx.state[2] = 0x3C6EF372u; ctx.state[3] = 0xA54FF53Au;
    ctx.state[4] = 0x510E527Fu; ctx.state[5] = 0x9B05688Cu;
    ctx.state[6] = 0x1F83D9ABu; ctx.state[7] = 0x5BE0CD19u;
    ctx.bit_count = 0;
    ctx.buffer_used = 0;
}

void Sha256Block(Sha256Context& ctx, const u8* block)
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
    {
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++)
    {
        u32 s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    u32 e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

    for (int i = 0; i < 64; i++)
    {
        u32 s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 temp1 = h + s1 + ch + k_sha256_k[i] + w[i];
        u32 s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = s0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

void Sha256Update(Sha256Context& ctx, const u8* data, size_t size)
{
    ctx.bit_count += (u64)size * 8;
    while (size > 0)
    {
        size_t space = 64 - ctx.buffer_used;
        size_t take = (size < space) ? size : space;
        memcpy(ctx.buffer + ctx.buffer_used, data, take);
        ctx.buffer_used += take;
        data += take;
        size -= take;
        if (ctx.buffer_used == 64)
        {
            Sha256Block(ctx, ctx.buffer);
            ctx.buffer_used = 0;
        }
    }
}

void Sha256Final(Sha256Context& ctx, u8 digest[32])
{
    u64 bit_count = ctx.bit_count;
    u8 pad = 0x80;
    Sha256Update(ctx, &pad, 1);
    u8 zero = 0x00;
    while (ctx.buffer_used != 56)
        Sha256Update(ctx, &zero, 1);

    u8 length[8];
    for (int i = 0; i < 8; i++)
        length[i] = (u8)((bit_count >> (56 - i * 8)) & 0xFF);
    // Feed the length directly: Sha256Update would keep adding to bit_count.
    memcpy(ctx.buffer + ctx.buffer_used, length, 8);
    Sha256Block(ctx, ctx.buffer);
    ctx.buffer_used = 0;

    for (int i = 0; i < 8; i++)
    {
        digest[i * 4] = (u8)((ctx.state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (u8)((ctx.state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (u8)((ctx.state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (u8)(ctx.state[i] & 0xFF);
    }
}

//------------------------------------------------------------------------------
// The profile table.
//
// This is the emulator-side mirror of `tools/build/rom_patches.py` in the
// project repository, which carries the reverse-engineering rationale for every
// byte. `tests/smoke/test_game_adaptation.py` parses the site rows below out of
// this file and fails if the two lists disagree, so the mirror cannot drift.
//
// Each row is: name, bank:address of the instruction, the operand's file offset,
// the vanilla byte that must already be there, and the byte wide mode writes.
// Bank 00 lives at file offset == address; bank 03 at 0xC000 + (address - 0x4000).
//------------------------------------------------------------------------------

// The horizontal level bounds of Super Mario Bros. Deluxe (U) (V1.1), for the
// level-edge fill (ADR 0009). Read-only, and per-game rather than per-width.
//
// `H_CameraXLow`/`H_CameraXHigh` ($FFB8/$FFB9) are the confirmed authoritative
// camera (project M2). `W_SubLvScreenCount` ($C161) is level header byte 00,
// written at `00:31A3`; a sublevel is `count + 1` screens of 256 pixels, which
// is the same bound `background_reconstruction.expected_entry` uses to decide a
// world column is outside the level. `H_GameState` ($FFB5) gates all three: the
// two values below are the only states this project has ever captured a loaded
// level in - `$0B` one-player and `$36` You vs. Boo - measured across every
// gameplay checkpoint in `artifacts/scenarios`.
const u8 k_smbdx_gameplay_states[] = { 0x0B, 0x36 };

// The HUD band is drawn with SCX and SCY forced to 0 - `VBlankInterrupt` writes
// them and arms `LYC` = $07 at `00:0C83`-`00:0C8B`, and the STAT handler
// restores the camera pair at `00:0DED`-`00:0DF3`. SCY 0 identifies those rows
// on its own, because `H_CameraY` is clamped to `[$38, $70]` by two independent
// copies of the same bound and can never be zero; SCX 0 is required as well
// only so the pair reads as the raster signature it is.
const GameLevelBounds k_smbdx_level_bounds = {
    0xFFB8,     // H_CameraXLow
    0xFFB9,     // H_CameraXHigh
    0xC161,     // W_SubLvScreenCount
    0xFFB5,     // H_GameState
    0x00,       // HUD raster SCX
    0x00,       // HUD raster SCY
    k_smbdx_gameplay_states,
    (int)(sizeof(k_smbdx_gameplay_states) / sizeof(k_smbdx_gameplay_states[0]))
};

const GameAdaptationSite k_smbdx_wide176_sites[] = {
    { "oam-clip-bias-two-entry",     "00:2829", 0x0282A, 0x10, 0x0F },
    { "oam-clip-two-entry",          "00:2833", 0x02834, 0xB8, 0xBF },
    { "oam-clip-bias-one-entry",     "00:28D6", 0x028D7, 0x10, 0x0F },
    { "oam-clip-one-entry",          "00:28E0", 0x028E1, 0xB8, 0xBF },
    { "oam-clip-bias-player",        "03:73D2", 0x0F3D3, 0x10, 0x0F },
    { "oam-clip-player",             "03:73DD", 0x0F3DE, 0xB8, 0xBF },
    { "spawn-lookahead-level-load",  "00:2AD0", 0x02AD1, 0xB0, 0xB8 },
    { "spawn-lookahead-per-frame",   "00:2C8D", 0x02C8E, 0xB0, 0xB8 },
    { "player-back-scroll-wall",     "03:6472", 0x0E473, 0x60, 0x68 }
};

const GameAdaptationSite k_smbdx_wide192_sites[] = {
    { "oam-clip-bias-two-entry",     "00:2829", 0x0282A, 0x10, 0x17 },
    { "oam-clip-two-entry",          "00:2833", 0x02834, 0xB8, 0xCF },
    { "oam-clip-bias-one-entry",     "00:28D6", 0x028D7, 0x10, 0x17 },
    { "oam-clip-one-entry",          "00:28E0", 0x028E1, 0xB8, 0xCF },
    { "oam-clip-bias-player",        "03:73D2", 0x0F3D3, 0x10, 0x17 },
    { "oam-clip-player",             "03:73DD", 0x0F3DE, 0xB8, 0xCF },
    { "spawn-lookahead-level-load",  "00:2AD0", 0x02AD1, 0xB0, 0xC0 },
    { "spawn-lookahead-per-frame",   "00:2C8D", 0x02C8E, 0xB0, 0xC0 },
    { "player-back-scroll-wall",     "03:6472", 0x0E473, 0x60, 0x70 }
};

const GameAdaptationSite k_smbdx_wide208_sites[] = {
    { "oam-clip-bias-two-entry",     "00:2829", 0x0282A, 0x10, 0x1F },
    { "oam-clip-two-entry",          "00:2833", 0x02834, 0xB8, 0xDF },
    { "oam-clip-bias-one-entry",     "00:28D6", 0x028D7, 0x10, 0x1F },
    { "oam-clip-one-entry",          "00:28E0", 0x028E1, 0xB8, 0xDF },
    { "oam-clip-bias-player",        "03:73D2", 0x0F3D3, 0x10, 0x1F },
    { "oam-clip-player",             "03:73DD", 0x0F3DE, 0xB8, 0xDF },
    { "spawn-lookahead-level-load",  "00:2AD0", 0x02AD1, 0xB0, 0xC8 },
    { "spawn-lookahead-per-frame",   "00:2C8D", 0x02C8E, 0xB0, 0xC8 },
    { "bg-stream-lookbehind",        "00:3695", 0x03696, 0x0F, 0x0E },
    { "player-back-scroll-wall",     "03:6472", 0x0E473, 0x60, 0x78 }
};

const GameAdaptationSite k_smbdx_wide224_sites[] = {
    { "oam-clip-bias-two-entry",     "00:2829", 0x0282A, 0x10, 0x27 },
    { "oam-clip-two-entry",          "00:2833", 0x02834, 0xB8, 0xEF },
    { "oam-clip-bias-one-entry",     "00:28D6", 0x028D7, 0x10, 0x27 },
    { "oam-clip-one-entry",          "00:28E0", 0x028E1, 0xB8, 0xEF },
    { "oam-clip-bias-player",        "03:73D2", 0x0F3D3, 0x10, 0x27 },
    { "oam-clip-player",             "03:73DD", 0x0F3DE, 0xB8, 0xEF },
    { "spawn-lookahead-level-load",  "00:2AD0", 0x02AD1, 0xB0, 0xD0 },
    { "spawn-lookahead-per-frame",   "00:2C8D", 0x02C8E, 0xB0, 0xD0 },
    { "bg-stream-lookbehind",        "00:3695", 0x03696, 0x0F, 0x0E },
    { "player-back-scroll-wall",     "03:6472", 0x0E473, 0x60, 0x80 }
};

const GameAdaptationProfile k_profiles[] = {
    {
        "smbdx-u-v11-wide176",
        "Super Mario Bros. Deluxe (U) (V1.1): move the three OAM clip windows "
        "to screen_x [-23, 168), both object spawn lookaheads to 184, leaving "
        "the background lookbehind vanilla because one column already covers "
        "this margin, and the player back-scroll wall to 104 pixels so he stops "
        "at the picture's edge rather than inside it, for the 8-pixel margins of "
        "the 176x144 viewport",
        "db81dd4acbd0c7a3b9004f169ee278450c764c842ae777abd28073fbedf4078b",
        "e133a228883b984aaff64af9c9b0039ca6960c424fbe7e78510737bb7f38f39f",
        8,
        k_smbdx_wide176_sites,
        (int)(sizeof(k_smbdx_wide176_sites) / sizeof(k_smbdx_wide176_sites[0])),
        &k_smbdx_level_bounds
    },
    {
        "smbdx-u-v11-wide192",
        "Super Mario Bros. Deluxe (U) (V1.1): move the three OAM clip windows "
        "to screen_x [-31, 176), both object spawn lookaheads to 192, leaving "
        "the background lookbehind vanilla because one column already covers "
        "this margin, and the player back-scroll wall to 112 pixels so he stops "
        "at the picture's edge rather than inside it, for the 16-pixel margins "
        "of the 192x144 viewport",
        "db81dd4acbd0c7a3b9004f169ee278450c764c842ae777abd28073fbedf4078b",
        "3ae1cb0fc780cb65fc8c4964eb9838455e38659f5203a9faeae1b5fde502dc7d",
        16,
        k_smbdx_wide192_sites,
        (int)(sizeof(k_smbdx_wide192_sites) / sizeof(k_smbdx_wide192_sites[0])),
        &k_smbdx_level_bounds
    },
    {
        "smbdx-u-v11-wide208",
        "Super Mario Bros. Deluxe (U) (V1.1): move the three OAM clip windows "
        "to screen_x [-39, 184), both object spawn lookaheads to 200, and the "
        "background stream a second column behind the camera, and the player "
        "back-scroll wall to 120 pixels so he stops at the picture's edge "
        "rather than inside it, for the 24-pixel margins of the 208x144 "
        "viewport",
        "db81dd4acbd0c7a3b9004f169ee278450c764c842ae777abd28073fbedf4078b",
        "d685804821bc6be2d2545e854af73431c44df0d9f5878a3088844b040e1b1696",
        24,
        k_smbdx_wide208_sites,
        (int)(sizeof(k_smbdx_wide208_sites) / sizeof(k_smbdx_wide208_sites[0])),
        &k_smbdx_level_bounds
    },
    {
        "smbdx-u-v11-wide224",
        "Super Mario Bros. Deluxe (U) (V1.1): move the three OAM clip windows "
        "to screen_x [-47, 192), both object spawn lookaheads to 208, and the "
        "background stream a second column behind the camera, and the player "
        "back-scroll wall to 128 pixels so he stops at the picture's edge "
        "rather than inside it, for the 32-pixel margins of the 224x144 "
        "viewport",
        "db81dd4acbd0c7a3b9004f169ee278450c764c842ae777abd28073fbedf4078b",
        "494068a07197a04b57fb1ab6316da521da7c1c0c6abc4beae3984c462b247c77",
        32,
        k_smbdx_wide224_sites,
        (int)(sizeof(k_smbdx_wide224_sites) / sizeof(k_smbdx_wide224_sites[0])),
        &k_smbdx_level_bounds
    }
};

const int k_profile_count = (int)(sizeof(k_profiles) / sizeof(k_profiles[0]));

} // anonymous namespace

//------------------------------------------------------------------------------

std::string GameAdaptation::Sha256Hex(const u8* data, size_t size)
{
    if (!IsValidPointer(data))
        return std::string();

    Sha256Context ctx;
    Sha256Init(ctx);
    Sha256Update(ctx, data, size);
    u8 digest[32];
    Sha256Final(ctx, digest);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[64] = 0;
    return std::string(hex);
}

int GameAdaptation::ProfileCount()
{
    return k_profile_count;
}

const GameAdaptationProfile* GameAdaptation::ProfileAt(int index)
{
    if (index < 0 || index >= k_profile_count)
        return NULL;
    return &k_profiles[index];
}

const GameAdaptationProfile* GameAdaptation::FindProfile(const std::string& rom_sha256,
                                                         int margin_pixels)
{
    if (rom_sha256.empty())
        return NULL;
    // One image now has a profile per viewport width, so a digest alone no
    // longer identifies an adaptation: a 176-pixel viewport running the 224
    // profile would clip and spawn for a screen it is not drawing. The margin
    // is part of the key. See project ADR 0007.
    for (int i = 0; i < k_profile_count; i++)
    {
        if ((rom_sha256 == k_profiles[i].rom_sha256)
            && (margin_pixels == k_profiles[i].margin_pixels))
            return &k_profiles[i];
    }
    return NULL;
}

void GameAdaptation::Apply(u8* rom, int size, bool wide_mode, int margin_pixels,
                           bool enabled, GameAdaptationState& out)
{
    out = GameAdaptationState();

    if (!IsValidPointer(rom) || (size <= 0))
    {
        out.reason = "no-rom";
        return;
    }

    out.rom_loaded = true;
    out.wide_mode = wide_mode;
    out.enabled = enabled;
    out.rom_sha256 = Sha256Hex(rom, (size_t)size);
    out.adapted_sha256 = out.rom_sha256;

    if (!wide_mode)
    {
        // Native mode is the compatibility baseline and never adapts a game.
        out.reason = "wide-mode-disabled";
        return;
    }

    if (!enabled)
    {
        // --no-game-adaptation: render wide, leave the game exactly as supplied.
        out.reason = "adaptation-disabled";
        Log("Wide mode: game adaptation disabled by request; rendering wide with "
            "the game unmodified.");
        return;
    }

    const GameAdaptationProfile* profile = FindProfile(out.rom_sha256, margin_pixels);

    if (!IsValidPointer(profile))
    {
        // Widen the picture, but never write to a ROM we have not identified -
        // at this width. A recognised image at a width with no profile is the
        // same situation as an unrecognised image: render wide, change nothing.
        out.reason = "unrecognised-rom";
        Log("Wide mode: no game adaptation for ROM %s at a %d-pixel margin; "
            "rendering wide with the game unmodified.", out.rom_sha256.c_str(),
            margin_pixels);
        return;
    }

    out.profile_id = profile->id;
    out.profile_description = profile->description;
    out.margin_pixels = profile->margin_pixels;

    // Refuse to write anything unless every site still holds the byte the
    // profile expects. The digest already proves that, so this can only fire on
    // a table mistake - which is exactly when writing would be worst.
    for (int i = 0; i < profile->site_count; i++)
    {
        const GameAdaptationSite& site = profile->sites[i];
        if (((int)site.file_offset >= size) || (rom[site.file_offset] != site.vanilla))
        {
            out.reason = "site-mismatch";
            Log("Wide mode: game adaptation '%s' site '%s' at 0x%05X expected "
                "0x%02X; applying nothing.", profile->id, site.name,
                (unsigned int)site.file_offset, site.vanilla);
            return;
        }
    }

    for (int i = 0; i < profile->site_count; i++)
    {
        const GameAdaptationSite& site = profile->sites[i];
        GameAdaptationAppliedSite applied;
        applied.name = site.name;
        applied.site = site.site;
        applied.file_offset = site.file_offset;
        applied.from = rom[site.file_offset];
        applied.to = site.adapted;
        rom[site.file_offset] = site.adapted;
        out.applied.push_back(applied);
    }

    out.matched = true;
    out.reason = "applied";
    out.live_verified = true;
    out.adapted_sha256 = Sha256Hex(rom, (size_t)size);
    out.adapted_sha256_matches_profile = (out.adapted_sha256 == profile->adapted_sha256);

    Log("Wide mode: applied game adaptation '%s' to ROM %s (%d sites); adapted "
        "image is %s.", profile->id, out.rom_sha256.c_str(),
        (int)out.applied.size(), out.adapted_sha256.c_str());

    if (!out.adapted_sha256_matches_profile)
    {
        Log("Wide mode: adapted image %s does not match the digest recorded for "
            "'%s' (%s).", out.adapted_sha256.c_str(), profile->id,
            profile->adapted_sha256);
    }
}

void GameAdaptation::Verify(const u8* rom, int size, GameAdaptationState& state)
{
    if (!state.matched)
        return;

    if (!IsValidPointer(rom) || (size <= 0))
    {
        state.live_verified = false;
        return;
    }

    bool verified = true;
    for (size_t i = 0; i < state.applied.size(); i++)
    {
        const GameAdaptationAppliedSite& applied = state.applied[i];
        if (((int)applied.file_offset >= size) || (rom[applied.file_offset] != applied.to))
        {
            verified = false;
            break;
        }
    }
    state.live_verified = verified;
}

const GameLevelBounds* GameAdaptation::ResolveLevelEdgeFill(
    bool fill_enabled, int margin_pixels, GameAdaptationState& state)
{
    state.level_bounds_known = false;
    state.level_edge_fill_enabled = fill_enabled;

    if (!state.rom_loaded)
    {
        state.level_edge_fill_reason = "no-rom";
        return NULL;
    }

    if (!state.wide_mode)
    {
        // Native mode never fills: its viewport cannot see past a level's end.
        state.level_edge_fill_reason = "wide-mode-disabled";
        return NULL;
    }

    const GameAdaptationProfile* profile = FindProfile(state.rom_sha256, margin_pixels);

    if (!IsValidPointer(profile) || !IsValidPointer(profile->level_bounds))
    {
        // Same rule as the site table: a ROM this table has not identified gets
        // the wide renderer and nothing else. Guessing where an unknown game
        // keeps its camera would paint over a picture we cannot read.
        state.level_edge_fill_reason = "unrecognised-rom";
        return NULL;
    }

    state.level_bounds_known = true;

    if (!fill_enabled)
    {
        // --no-level-edge-fill: the mutation lever. Renders wide with the ring
        // shown raw at a level's edges, which is what every capture before
        // ADR 0009 recorded.
        state.level_edge_fill_reason = "fill-disabled";
        Log("Wide mode: level-edge fill disabled by request; the background ring "
            "is shown unmasked outside the level.");
        return NULL;
    }

    state.level_edge_fill_reason = "active";
    return profile->level_bounds;
}
