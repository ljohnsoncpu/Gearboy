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

#include "Video.h"
#include "Memory.h"
#include "Processor.h"
#include "TraceLogger.h"

Video::Video(Memory* pMemory, Processor* pProcessor)
{
    m_pMemory = pMemory;
    m_pMemory->SetVideo(this);
    m_pProcessor = pProcessor;
    InitPointer(m_pFrameBuffer);
    InitPointer(m_pColorFrameBuffer);
    InitPointer(m_pSpriteXCacheBuffer);
    InitPointer(m_pColorCacheBuffer);
    InitPointer(m_pTraceLogger);
    m_iStatusMode = 0;
    m_iStatusModeCounter = 0;
    m_iStatusModeCounterAux = 0;
    m_iPendingVBlankInterruptCycles = 0;
    m_iStatusModeLYCounter = 0;
    m_iScreenEnableDelayCycles = 0;
    m_iStatusVBlankLine = 0;
    m_iWindowLine = 0;
    m_bWindowYTrigger = false;
    m_iPixelCounter = 0;
    m_iTileCycleCounter = 0;
    m_bScreenEnabled = true;
    m_bCGB = false;
    m_bSGBTransferMode = false;
    m_bScanLineTransfered = false;
    m_iHideFrames = 0;
    m_IRQ48Signal = 0;
    m_pixelFormat = GB_PIXEL_RGB565;
    m_bWideScreen = false;
    m_iScreenWidth = GAMEBOY_WIDTH;
    m_iViewportOriginX = 0;
    m_iWideOamXSignedMin = GAMEBOY_WIDE_OAM_X_SIGNED_MIN;
    InitPointer(m_pLevelBounds);
    m_bLevelEdgeGameplay = false;
    m_iLevelEdgeCameraX = 0;
    m_iLevelEdgeScreenCount = 0;
    m_iLevelEdgeOriginX = 0;
}

Video::~Video()
{
    SafeDeleteArray(m_pSpriteXCacheBuffer);
    SafeDeleteArray(m_pColorCacheBuffer);
    SafeDeleteArray(m_pFrameBuffer);
}

void Video::Init()
{
    // Allocated at the maximum width so selecting wide mode never reallocates.
    // Native mode uses a GAMEBOY_WIDTH stride into the same storage, leaving the
    // tail of each row untouched and every native index unchanged.
    m_pFrameBuffer = new u8[GAMEBOY_MAX_WIDTH * GAMEBOY_HEIGHT];
    m_pSpriteXCacheBuffer = new int[GAMEBOY_MAX_WIDTH * GAMEBOY_HEIGHT];
    m_pColorCacheBuffer = new u8[GAMEBOY_MAX_WIDTH * GAMEBOY_HEIGHT];
    Reset(false);
}

void Video::SetWideScreen(bool enabled, int width)
{
    // The width is a run-time parameter (project ADR 0007). Anything outside
    // the measured range is clamped rather than honoured: no renderer change
    // can make a viewport wider than 224 correct for this game, because the
    // game's own background streaming does not reach that far and an unsigned
    // OAM X stops being unambiguous.
    if (width < GAMEBOY_WIDE_MIN_WIDTH)
        width = GAMEBOY_WIDE_MIN_WIDTH;
    if (width > GAMEBOY_WIDE_MAX_WIDTH)
        width = GAMEBOY_WIDE_MAX_WIDTH;

    m_bWideScreen = enabled;
    m_iScreenWidth = enabled ? width : GAMEBOY_WIDTH;
    m_iViewportOriginX = enabled ? GAMEBOY_WIDE_MARGIN_FOR(width) : 0;
    // Derived from the margin rather than fixed: the largest OAM X a clipped
    // path can write moves with the width, and so does the first value that can
    // only mean "left margin".
    m_iWideOamXSignedMin =
        GAMEBOY_WIDE_OAM_X_SIGNED_MIN_FOR(GAMEBOY_WIDE_MARGIN_FOR(width));
    m_bLevelEdgeGameplay = false;
    m_iLevelEdgeCameraX = 0;
    m_iLevelEdgeScreenCount = 0;
    m_iLevelEdgeOriginX = m_iViewportOriginX;
}

bool Video::IsWideScreen() const
{
    return m_bWideScreen;
}

int Video::GetScreenWidth() const
{
    return m_iScreenWidth;
}

void Video::SetLevelBounds(const GameLevelBounds* bounds)
{
    m_pLevelBounds = bounds;
}

bool Video::HasLevelBounds() const
{
    return IsValidPointer(m_pLevelBounds);
}

bool Video::LevelEdgeView(u8 scroll_x, u8 scroll_y, int& origin_x,
                          int& left_end, int& right_start) const
{
    // The widened viewport can point at world columns the level does not have.
    // Nothing in VRAM says so - the background map is a 256-pixel ring with no
    // notion of where a level starts or stops, so a slot outside the level holds
    // whatever was last written to it: level content from elsewhere in the ring
    // at a level start, or, below camera 32, bytes the game's own backward
    // streaming read from outside the level map. Both are stale rather than
    // wrong for the ring; they are simply not a picture of anywhere the player
    // can be. See project ADR 0009.
    //
    // The answer is to stop looking there. This computes a DISPLAY CAMERA - the
    // game's own camera pushed just far enough inside the level that the whole
    // 224-pixel viewport fits - and returns the viewport origin that presents
    // it. Nothing about the game changes: the camera it plays by is untouched,
    // and only where the picture is taken from moves. At a level start the
    // effect is the one the original has anyway - the level's first column sits
    // against the screen's left edge and the view holds still while the player
    // walks in, then scrolls normally once the camera passes the margin.
    //
    // The content that needs is always resident. While the game camera is below
    // the margin the display camera is pinned AT the margin, so the view is a
    // fixed world 0..223 - inside the world 0..255 every level load fills the
    // ring with, and never the ring slots the backward streaming underflow
    // writes to.
    //
    // `left_end`/`right_start` are the residual: anything still outside the
    // level after the shift, which is filled with the background colour. It is
    // zero whenever the camera is at rest, and a pixel or two while it moves.
    origin_x = m_iViewportOriginX;
    left_end = 0;
    right_start = m_iScreenWidth;

    if (!m_bWideScreen || !IsValidPointer(m_pLevelBounds))
        return false;

    // The bounds only describe a loaded level. This flag is latched with the
    // other mutable inputs on scanline 0, so a state transition cannot turn the
    // clamp on or off for one row in the middle of the picture.
    if (!m_bLevelEdgeGameplay)
        return false;

    // A scanline the game draws with its HUD raster has no camera of its own:
    // SCX and SCY are both forced to zero for those rows and restored at LY 7.
    // Clamping them against a camera they are not drawn with would move the
    // status bar - and, because the forced scroll always resolves to the page
    // boundary, would have moved it on EVERY frame rather than only at a
    // level's edge. They keep the vanilla wide picture, margins and all.
    if ((scroll_x == m_pLevelBounds->hud_raster_scx)
        && (scroll_y == m_pLevelBounds->hud_raster_scy))
        return false;

    int logical_camera = m_iLevelEdgeCameraX;

    // `H_CameraX` is one frame ahead of what is being drawn: VBlank copies its
    // low byte to SCX before the camera update, so SCX is the authority for the
    // frame on screen. Take the low byte from this scanline's own SCX and the
    // page from the logical camera, choosing the congruent page nearest it -
    // which is also what makes the HUD band come out right, because those seven
    // scanlines are drawn with SCX forced to 0 and land on the page boundary.
    int page = logical_camera & ~0xFF;
    int camera = page + (int)scroll_x;
    if ((camera - logical_camera) > 128)
        camera -= 0x100;
    else if ((logical_camera - camera) > 128)
        camera += 0x100;

    int level_width = (m_iLevelEdgeScreenCount + 1) << 8;

    // The display camera: the nearest camera whose whole viewport is inside the
    // level. The lower bound is the margin; the upper is where the last visible
    // column is the level's last. A level narrower than the viewport would put
    // the two the wrong way round, so the low bound wins - it cannot happen
    // here, since the narrowest sublevel is one 256-pixel screen and the widest
    // viewport is 224, but a clamp that can inverse is a clamp that will.
    int display_low = m_iViewportOriginX;
    int display_high = level_width - GAMEBOY_WIDTH - m_iViewportOriginX;
    if (display_high < display_low)
        display_high = display_low;

    int display_camera = camera;
    if (display_camera < display_low)
        display_camera = display_low;
    else if (display_camera > display_high)
        display_camera = display_high;

    // Presenting a different camera IS moving the viewport origin, and the two
    // have to move together or the background and the sprites would part
    // company. The window layer is deliberately left alone: the HUD is a
    // screen-space overlay, and a status bar that slid about at a level edge
    // would be the opposite of what this is for.
    origin_x = m_iViewportOriginX - (display_camera - camera);

    // Output column x now shows world column `camera + x - origin_x`.
    int first_inside = origin_x - camera;
    int first_outside = origin_x + level_width - camera;

    // Clamped to the margins on purpose: the native window is what every native
    // and native-window assertion in this project compares, and the game's own
    // camera clamp already keeps it inside the level. A bounds read that went
    // wrong therefore cannot reach the 160 pixels the game itself draws.
    if (first_inside < 0)
        first_inside = 0;
    if (first_inside > m_iViewportOriginX)
        first_inside = m_iViewportOriginX;
    int native_end = m_iViewportOriginX + GAMEBOY_WIDTH;
    if (first_outside < native_end)
        first_outside = native_end;
    if (first_outside > m_iScreenWidth)
        first_outside = m_iScreenWidth;

    left_end = first_inside;
    right_start = first_outside;
    return true;
}

void Video::SetTraceLogger(TraceLogger* pTraceLogger)
{
    m_pTraceLogger = pTraceLogger;
}

void Video::LogTraceEvent(u8 event, u8 value)
{
#if !defined(GEARBOY_DISABLE_DISASSEMBLER)
    GB_Trace_Entry e = {};
    e.type = TRACE_LCD;
    e.lcd.event = event;
    e.lcd.value = value;
    e.lcd.line = (u16)m_iStatusModeLYCounter;
    e.lcd.mode = (u8)m_iStatusMode;
    m_pTraceLogger->TraceLog(e);
#else
    UNUSED(event);
    UNUSED(value);
#endif
}

void Video::SetSGBTransferMode(bool enabled)
{
    m_bSGBTransferMode = enabled;
}

void Video::Reset(bool bCGB)
{
    for (int i = 0; i < (m_iScreenWidth * GAMEBOY_HEIGHT); i++)
        m_pSpriteXCacheBuffer[i] = m_pFrameBuffer[i] = m_pColorCacheBuffer[i] = 0;

    for (int p = 0; p < 8; p++)
        for (int c = 0; c < 4; c++)
        {
            // CGB boot ROM fades all BG palettes to white
            m_CGBBackgroundPalettes[p][c][0] = bCGB ? 0x7FFF : 0x0000;
            m_CGBBackgroundPalettes[p][c][1] = bCGB ? 0xFFFF : 0x0000;
            m_CGBSpritePalettes[p][c][0] = 0x0000;
            m_CGBSpritePalettes[p][c][1] = 0x0000;
        }

    m_iStatusMode = 1;
    m_iStatusModeCounter = 0;
    m_iStatusModeCounterAux = 0;
    m_iPendingVBlankInterruptCycles = 0;
    m_iStatusModeLYCounter = 144;
    m_iScreenEnableDelayCycles = 0;
    m_iStatusVBlankLine = 0;
    m_iWindowLine = 0;
    m_bWindowYTrigger = false;
    m_iPixelCounter = 0;
    m_iTileCycleCounter = 0;
    m_bScreenEnabled = true;
    m_bScanLineTransfered = false;
    m_bCGB = bCGB;
    m_iHideFrames = 0;
    m_IRQ48Signal = 0;
    m_bLevelEdgeGameplay = false;
    m_iLevelEdgeCameraX = 0;
    m_iLevelEdgeScreenCount = 0;
    m_iLevelEdgeOriginX = m_iViewportOriginX;
}

void Video::ResetToBootromState()
{
    m_bScreenEnabled = false;
    m_iStatusMode = 0;
    m_iStatusModeCounter = 0;
    m_iStatusModeCounterAux = 0;
    m_iStatusModeLYCounter = 0;
    m_iScreenEnableDelayCycles = 0;
}

bool Video::Tick(unsigned int &clockCycles, u16* pColorFrameBuffer, GB_Color_Format pixelFormat)
{
    m_pColorFrameBuffer = pColorFrameBuffer;
    m_pixelFormat = pixelFormat;

    bool vblank = false;
    m_iStatusModeCounter += clockCycles;

    if (m_iPendingVBlankInterruptCycles > 0)
    {
        m_iPendingVBlankInterruptCycles -= clockCycles;

        if (m_iPendingVBlankInterruptCycles <= 0)
        {
            m_iPendingVBlankInterruptCycles = 0;
            m_pMemory->Load(0xFF0F, m_pMemory->Retrieve(0xFF0F) | Processor::VBlank_Interrupt);
            TraceEvent(TRACE_LCD_VBLANK_IRQ, 0);
        }
    }

    if (m_bScreenEnabled)
    {
        switch (m_iStatusMode)
        {
            // During H-BLANK
            case 0:
            {
                if (m_iStatusModeCounter >= 204)
                {
                    m_iStatusModeCounter -= 204;
                    m_iStatusMode = 2;
                    if (m_bCGB)
                        m_IRQ48Signal = UnsetBit(m_IRQ48Signal, 0);

                    m_iStatusModeLYCounter++;
                    m_pMemory->Load(0xFF44, m_iStatusModeLYCounter);
                    CompareLYToLYC();
                    CheckWindowY();

                    if (m_iStatusModeLYCounter == 144)
                    {
                        m_iStatusMode = 1;
                        m_iStatusVBlankLine = 0;
                        m_iStatusModeCounterAux = m_iStatusModeCounter;

                        if (m_pProcessor->CGBSpeed())
                        {
                            m_iPendingVBlankInterruptCycles = 12;
                        }
                        else
                        {
                            m_pProcessor->RequestInterrupt(Processor::VBlank_Interrupt);
                            TraceEvent(TRACE_LCD_VBLANK_IRQ, 0);
                        }

                        if (m_iHideFrames > 0)
                        {
                            m_iHideFrames--;

                            if (IsValidPointer(m_pColorFrameBuffer))
                            {
                                if (m_bCGB)
                                {
                                    for (int i = 0; i < m_iScreenWidth * GAMEBOY_HEIGHT; i++)
                                        m_pColorFrameBuffer[i] = 0xFFFF;
                                }
                                else
                                {
                                    if (!m_bSGBTransferMode)
                                        memset(m_pFrameBuffer, 0, m_iScreenWidth * GAMEBOY_HEIGHT);
                                    memset(m_pColorFrameBuffer, 0, m_iScreenWidth * GAMEBOY_HEIGHT * sizeof(u16));
                                }
                            }

                            vblank = true;
                        }
                        else
                            vblank = true;

                        m_iWindowLine = 0;
                        m_bWindowYTrigger = false;
                    }

                    UpdateStatRegister();
                    RefreshStatInterruptSignal(true);
                }
                break;
            }
            // During V-BLANK
            case 1:
            {
                m_iStatusModeCounterAux += clockCycles;

                if (m_iStatusModeCounterAux >= 456)
                {
                    m_iStatusModeCounterAux -= 456;
                    m_iStatusVBlankLine++;

                    if (m_iStatusVBlankLine <= 9)
                    {
                        m_iStatusModeLYCounter++;
                        m_pMemory->Load(0xFF44, m_iStatusModeLYCounter);
                        CompareLYToLYC();
                    }
                }

                if ((m_iStatusModeCounter >= 4104) && (m_iStatusModeCounterAux >= 4) && (m_iStatusModeLYCounter == 153))
                {
                    m_iStatusModeLYCounter = 0;
                    m_pMemory->Load(0xFF44, m_iStatusModeLYCounter);
                    CompareLYToLYC();
                }

                if (m_iStatusModeCounter >= 4560)
                {
                    m_iStatusModeCounter -= 4560;
                    m_iStatusMode = 2;
                    CheckWindowY();
                    UpdateStatRegister();
                    RefreshStatInterruptSignal(true);
                }
                break;
            }
            // During searching OAM RAM
            case 2:
            {
                if (m_iStatusModeCounter >= 80)
                {
                    m_iStatusModeCounter -= 80;
                    m_iStatusMode = 3;
                    m_bScanLineTransfered = false;
                    UpdateStatRegister();
                    RefreshStatInterruptSignal(true);
                }
                break;
            }
            // During transfering data to LCD driver
            case 3:
            {
#ifndef PERFORMANCE
                // Wide mode renders the whole row once in ScanLine instead. The
                // 96 extra columns have no hardware timing analogue, so there is
                // no mid-line SCX for them; see the limitation in docs/adr/0003.
                if (!m_bWideScreen && m_iPixelCounter < 160
                    && (m_iHideFrames == 0 || m_bSGBTransferMode))
                {
                    m_iTileCycleCounter += clockCycles;
                    u8 lcdc = m_pMemory->Retrieve(0xFF40);

                    if (m_bScreenEnabled && IsSetBit(lcdc, 7))
                    {
                        while (m_iTileCycleCounter >= 3)
                        {
                            if (IsValidPointer(m_pColorFrameBuffer))
                            {
                                RenderBG(m_iStatusModeLYCounter, m_iPixelCounter, 4);
                            }
                            m_iPixelCounter += 4;
                            m_iTileCycleCounter -= 3;

                            if (m_iPixelCounter >= 160)
                            {
                                break;
                            }
                        }
                    }
                }
#endif

                if (m_iStatusModeCounter >= 160 && !m_bScanLineTransfered)
                {
                    ScanLine(m_iStatusModeLYCounter);
                    m_bScanLineTransfered = true;
                }

                if (m_iStatusModeCounter >= 172)
                {
                    m_iPixelCounter = 0;
                    m_iStatusModeCounter -= 172;
                    m_iStatusMode = 0;
                    m_iTileCycleCounter = 0;

                    if (m_bCGB && m_pMemory->IsHDMAEnabled())
                    {
                        unsigned int cycles = m_pMemory->PerformHDMA();
                        m_iStatusModeCounter += cycles;
                        clockCycles += cycles;
                    }

                    UpdateStatRegister();
                    RefreshStatInterruptSignal(true);
                }
                break;
            }
        }
    }
    // Screen disabled
    else
    {
        if (m_iScreenEnableDelayCycles > 0)
        {
            m_iScreenEnableDelayCycles -= clockCycles;

            if (m_iScreenEnableDelayCycles <= 0)
            {
                m_iScreenEnableDelayCycles = 0;
                m_bScreenEnabled = true;
                if (m_iHideFrames < 0)
                    m_iHideFrames = 0;
                else
                    m_iHideFrames = 3;
                m_iStatusMode = 0;
                m_iStatusModeCounter = 0;
                m_iStatusModeCounterAux = 0;
                m_iPendingVBlankInterruptCycles = 0;
                m_iStatusModeLYCounter = 0;
                m_iWindowLine = 0;
                m_bWindowYTrigger = false;
                m_iStatusVBlankLine = 0;
                m_iPixelCounter = 0;
                m_iTileCycleCounter = 0;
                m_pMemory->Load(0xFF44, m_iStatusModeLYCounter);
                m_IRQ48Signal = 0;

                u8 stat = m_pMemory->Retrieve(0xFF41);
                if (IsSetBit(stat, 5))
                {
                    m_pProcessor->RequestInterrupt(Processor::LCDSTAT_Interrupt);
                    m_IRQ48Signal = SetBit(m_IRQ48Signal, 2);
                    TraceEvent(TRACE_LCD_STAT_IRQ, m_IRQ48Signal);
                }

                CompareLYToLYC();
                RefreshStatInterruptSignal(true);
            }
        }
        else if (m_iStatusModeCounter >= 70224)
        {
            m_iStatusModeCounter -= 70224;
            m_iHideFrames = 0;

            if (IsValidPointer(m_pColorFrameBuffer))
            {
                if (m_bCGB)
                {
                    for (int i = 0; i < m_iScreenWidth * GAMEBOY_HEIGHT; i++)
                        m_pColorFrameBuffer[i] = 0xFFFF;
                }
                else
                {
                    if (!m_bSGBTransferMode)
                        memset(m_pFrameBuffer, 0, m_iScreenWidth * GAMEBOY_HEIGHT);
                    memset(m_pColorFrameBuffer, 0, m_iScreenWidth * GAMEBOY_HEIGHT * sizeof(u16));
                }
            }

            vblank = true;
        }
    }
    return vblank;
}

void Video::EnableScreen()
{
    if (!m_bScreenEnabled)
    {
        m_iScreenEnableDelayCycles = 244;
    }
}

void Video::DisableScreen()
{
    bool disabled_in_vblank = m_bCGB && m_bScreenEnabled && (m_iStatusMode == 1);

    m_bScreenEnabled = false;
    m_pMemory->Load(0xFF44, 0x00);
    u8 stat = m_pMemory->Retrieve(0xFF41);
    stat &= 0x7C;
    m_pMemory->Load(0xFF41, stat);
    m_iStatusMode = 0;
    m_iStatusModeCounter = 0;
    m_iStatusModeCounterAux = 0;
    m_iPendingVBlankInterruptCycles = 0;
    m_iStatusModeLYCounter = 0;
    m_IRQ48Signal = 0;
    m_iHideFrames = disabled_in_vblank ? -1 : 0;

    if (!disabled_in_vblank && IsValidPointer(m_pColorFrameBuffer))
    {
        if (m_bCGB)
        {
            for (int i = 0; i < m_iScreenWidth * GAMEBOY_HEIGHT; i++)
                m_pColorFrameBuffer[i] = 0xFFFF;
        }
        else
        {
            if (!m_bSGBTransferMode)
                memset(m_pFrameBuffer, 0, m_iScreenWidth * GAMEBOY_HEIGHT);
            memset(m_pColorFrameBuffer, 0, m_iScreenWidth * GAMEBOY_HEIGHT * sizeof(u16));
        }
    }
}

bool Video::IsScreenEnabled() const
{
    return m_bScreenEnabled;
}

const u8* Video::GetFrameBuffer() const
{
    return m_pFrameBuffer;
}

const u16* Video::GetColorFrameBuffer() const
{
    return m_pColorFrameBuffer;
}

void Video::UpdatePaletteToSpecification(bool background, u8 value)
{
    bool hl = IsSetBit(value, 0);
    int index = (value >> 1) & 0x03;
    int pal = (value >> 3) & 0x07;

    u16 color = (background ? m_CGBBackgroundPalettes[pal][index][0] : m_CGBSpritePalettes[pal][index][0]);

    m_pMemory->Load(background ? 0xFF69 : 0xFF6B, hl ? (color >> 8) & 0xFF : color & 0xFF);
}

void Video::SetColorPalette(bool background, u8 value)
{
    u8 ps = background ? m_pMemory->Retrieve(0xFF68) : m_pMemory->Retrieve(0xFF6A);
    bool hl = IsSetBit(ps, 0);
    int index = (ps >> 1) & 0x03;
    int pal = (ps >> 3) & 0x07;
    bool increment = IsSetBit(ps, 7);

    if (increment)
    {
        u8 address = ps & 0x3F;
        address++;
        address &= 0x3F;
        ps = (ps & 0x80) | address;
        m_pMemory->Load(background ? 0xFF68 : 0xFF6A, ps);
        UpdatePaletteToSpecification(background, ps);
    }

    u16* palette_color_gbc = background ? &m_CGBBackgroundPalettes[pal][index][0] : &m_CGBSpritePalettes[pal][index][0];
    u16* palette_color_final = background ? &m_CGBBackgroundPalettes[pal][index][1] : &m_CGBSpritePalettes[pal][index][1];

    *palette_color_gbc = hl ? (*palette_color_gbc & 0x00FF) | (value << 8) : (*palette_color_gbc & 0xFF00) | value;
    
    u8 red_5bit = *palette_color_gbc & 0x1F;
    u8 blue_5bit = (*palette_color_gbc >> 10) & 0x1F;

    switch (m_pixelFormat)
    {
        case GB_PIXEL_RGB565:
        {
            u8 green_5bit = (*palette_color_gbc >> 5) & 0x1F;
            u8 green_6bit = (green_5bit << 1) | (green_5bit >> 4);
            *palette_color_final = (red_5bit << 11) | (green_6bit << 5) | blue_5bit;
            break;
        }
        case GB_PIXEL_BGR565:
        {
            u8 green_5bit = (*palette_color_gbc >> 5) & 0x1F;
            u8 green_6bit = (green_5bit << 1) | (green_5bit >> 4);
            *palette_color_final = (blue_5bit << 11) | (green_6bit << 5) | red_5bit;
            break;
        }
        case GB_PIXEL_RGB555:
        {
            u8 green_5bit = (*palette_color_gbc >> 5) & 0x1F;
            *palette_color_final = 0x8000 | (red_5bit << 10) | (green_5bit << 5) | blue_5bit;
            break;
        }
        case GB_PIXEL_BGR555:
        {
            u8 green_5bit = (*palette_color_gbc >> 5) & 0x1F;
            *palette_color_final = 0x8000 | (blue_5bit << 10) | (green_5bit << 5) | red_5bit;
            break;
        }
    }

}

bool Video::CGBPaletteAccessBlocked() const
{
    return m_bCGB && m_bScreenEnabled && (m_iStatusMode == 3);
}

bool Video::VRAMAccessBlocked() const
{
    //if (m_bCGB)
        return false;
    //return m_bScreenEnabled && (m_iStatusMode == 3) && !m_bScanLineTransfered;
}

int Video::GetCurrentStatusMode() const
{
    return m_iStatusMode;
}

void Video::RefreshStatInterruptSignal(bool requestInterrupt)
{
    u8 signal = 0;

    if (m_bScreenEnabled)
    {
        u8 stat = m_pMemory->Retrieve(0xFF41);
        if (IsSetBit(stat, 3) && (m_iStatusMode == 0))
            signal = SetBit(signal, 0);
        if (IsSetBit(stat, 4) && (m_iStatusMode == 1))
            signal = SetBit(signal, 1);
        if (IsSetBit(stat, 5) && (m_iStatusMode == 2))
            signal = SetBit(signal, 2);
        if (IsSetBit(stat, 6) && (m_pMemory->Retrieve(0xFF45) == m_iStatusModeLYCounter))
            signal = SetBit(signal, 3);
    }

    if (requestInterrupt && (m_IRQ48Signal == 0) && (signal != 0))
    {
        m_pProcessor->RequestInterrupt(Processor::LCDSTAT_Interrupt);
        TraceEvent(TRACE_LCD_STAT_IRQ, signal);
    }

    m_IRQ48Signal = signal;
}

void Video::CheckWindowY()
{
    if (m_bWindowYTrigger)
        return;

    u8 lcdc = m_pMemory->Retrieve(0xFF40);
    if (!IsSetBit(lcdc, 5))
        return;

    u8 wy = m_pMemory->Retrieve(0xFF4A);
    if (wy == (u8)m_iStatusModeLYCounter)
        m_bWindowYTrigger = true;
}

void Video::ResetWindowLine()
{
    if ((m_iWindowLine == 0) && (m_iStatusModeLYCounter < 144))
    {
        u8 wy = m_pMemory->Retrieve(0xFF4A);

        if ((m_iStatusModeLYCounter == wy) && (m_iStatusMode == 3) && !m_bScanLineTransfered)
            m_iWindowLine = -1;
    }

    CheckWindowY();
}

void Video::ScanLine(int line)
{
    if (m_iHideFrames > 0 && !m_bSGBTransferMode)
        return;

    if (IsValidPointer(m_pColorFrameBuffer))
    {
        u8 lcdc = m_pMemory->Retrieve(0xFF40);

        if (m_bScreenEnabled && IsSetBit(lcdc, 7))
        {
#ifdef PERFORMANCE
            RenderBG(line, 0, m_iScreenWidth);
#else
            // Native mode has already drawn this row four pixels at a time from
            // the mode 3 pixel counter. Wide mode skips that path and draws the
            // full row here, from a single SCX sample.
            if (m_bWideScreen)
                RenderBG(line, 0, m_iScreenWidth);
#endif
            RenderWindow(line);
            RenderSprites(line);
        }
        else
        {
            int line_width = (line * m_iScreenWidth);
            if (m_bCGB)
            {
                for (int x = 0; x < m_iScreenWidth; x++)
                    m_pColorFrameBuffer[line_width + x] = 0xFFFF;
            }
            else
            {
                for (int x = 0; x < m_iScreenWidth; x++)
                    m_pFrameBuffer[line_width + x] = 0;
            }
        }
    }
}

void Video::RenderBG(int line, int pixel, int pixels_to_render)
{
    u8 lcdc = m_pMemory->Retrieve(0xFF40);
    int line_width = (line * m_iScreenWidth);

    if (m_bCGB || IsSetBit(lcdc, 0))
    {
        int offset_x_init = pixel & 0x7;
        int offset_x_end = offset_x_init + pixels_to_render;
        int screen_tile = pixel >> 3;
        int tile_start_addr = IsSetBit(lcdc, 4) ? 0x8000 : 0x8800;
        int map_start_addr = IsSetBit(lcdc, 3) ? 0x9C00 : 0x9800;
        u8 scroll_x = m_pMemory->Retrieve(0xFF43);
        u8 scroll_y = m_pMemory->Retrieve(0xFF42);
        u8 line_scrolled = line + scroll_y;
        int line_scrolled_32 = (line_scrolled >> 3) << 5;
        int tile_pixel_y = line_scrolled & 0x7;
        int tile_pixel_y_2 = tile_pixel_y << 1;
        int tile_pixel_y_flip_2 = (7 - tile_pixel_y) << 1;
        u8 palette = m_pMemory->Retrieve(0xFF47);

        // These are game variables, not atomic video registers. The game
        // updates them during visible lines; sampling them again on every row
        // let the clamp observe a transient level-state snapshot (measured as
        // camera $FFFF/$FFFE and screen count $FF on row 13 at a level start).
        // That moved exactly one scanline to another background-ring page.
        // Latch every mutable input once so the whole frame uses one view.
        // SCX remains the per-scanline authority for the camera's low byte.
        if (line == 0 && m_bWideScreen && IsValidPointer(m_pLevelBounds))
        {
            u8 state = m_pMemory->Retrieve(m_pLevelBounds->game_state);
            m_bLevelEdgeGameplay = false;
            for (int i = 0; i < m_pLevelBounds->gameplay_state_count; i++)
            {
                if (state == m_pLevelBounds->gameplay_states[i])
                {
                    m_bLevelEdgeGameplay = true;
                    break;
                }
            }
            m_iLevelEdgeCameraX =
                m_pMemory->Retrieve(m_pLevelBounds->camera_x_low)
                | (m_pMemory->Retrieve(m_pLevelBounds->camera_x_high) << 8);
            m_iLevelEdgeScreenCount =
                m_pMemory->Retrieve(m_pLevelBounds->screen_count);
        }

        // SMBDX widescreen: where this line's viewport sits, and which of its
        // output columns are still outside the level. Computed once per
        // scanline; the pixel loop only compares.
        //
        // LevelEdgeView returns false for a HUD-raster scanline, which is what
        // keeps the status bar out of it; see the comment there.
        int origin_x = m_iViewportOriginX;
        int level_left_end = 0;
        int level_right_start = m_iScreenWidth;
        bool fill_edges = LevelEdgeView(
            scroll_x, scroll_y, origin_x, level_left_end, level_right_start);
        if (fill_edges)
        {
            // Latched from the gameplay rows, because sprites take this origin
            // on every row - including the HUD band, which has no camera to
            // derive one from and where a sprite must not be torn in two.
            m_iLevelEdgeOriginX = origin_x;
        }

        for (int offset_x = offset_x_init; offset_x < offset_x_end; offset_x++)
        {
            int screen_pixel_x = (screen_tile << 3) + offset_x;

            if (fill_edges
                && ((screen_pixel_x < level_left_end)
                    || (screen_pixel_x >= level_right_start)))
            {
                // Outside the level: paint the background colour rather than a
                // ring slot that is a picture of somewhere else. Colour index 0
                // with no priority bit, so sprites still composite normally over
                // it - the player can stand in the margin and be drawn there.
                int index = line_width + screen_pixel_x;
                m_pColorCacheBuffer[index] = 0;
                if (m_bCGB)
                    m_pColorFrameBuffer[index] = m_CGBBackgroundPalettes[0][0][1];
                else
                    m_pColorFrameBuffer[index] = m_pFrameBuffer[index] = palette & 0x03;
                continue;
            }

            // The native 160-pixel window sits at `origin_x` inside the output
            // row, so the map coordinate is shifted left by that origin. That is
            // m_iViewportOriginX everywhere except at a level's edges, where
            // LevelEdgeView moves it to present a camera the level can fill.
            // map_pixel_x is a u8: it wraps modulo the 256-pixel map, which is
            // exactly why a 256-wide viewport walks the ring once and no more.
            u8 map_pixel_x = screen_pixel_x - origin_x + scroll_x;
            int map_tile_x = map_pixel_x >> 3;
            int map_tile_offset_x = map_pixel_x & 0x7;
            u16 map_tile_addr = map_start_addr + line_scrolled_32 + map_tile_x;
            int map_tile = 0;

            if (tile_start_addr == 0x8800)
            {
                map_tile = static_cast<s8> (m_pMemory->Retrieve(map_tile_addr));
                map_tile += 128;
            }
            else
            {
                map_tile = m_pMemory->Retrieve(map_tile_addr);
            }

            u8 cgb_tile_attr = m_bCGB ? m_pMemory->ReadCGBLCDRAM(map_tile_addr, true) : 0;
            u8 cgb_tile_pal = m_bCGB ? (cgb_tile_attr & 0x07) : 0;
            bool cgb_tile_bank = m_bCGB ? IsSetBit(cgb_tile_attr, 3) : false;
            bool cgb_tile_xflip = m_bCGB ? IsSetBit(cgb_tile_attr, 5) : false;
            bool cgb_tile_yflip = m_bCGB ? IsSetBit(cgb_tile_attr, 6) : false;
            int map_tile_16 = map_tile << 4;
            u8 byte1 = 0;
            u8 byte2 = 0;
            int final_pixely_2 = cgb_tile_yflip ? tile_pixel_y_flip_2 : tile_pixel_y_2;
            int tile_address = tile_start_addr + map_tile_16 + final_pixely_2;

            if (cgb_tile_bank)
            {
                byte1 = m_pMemory->ReadCGBLCDRAM(tile_address, true);
                byte2 = m_pMemory->ReadCGBLCDRAM(tile_address + 1, true);
            }
            else
            {
                byte1 = m_pMemory->Retrieve(tile_address);
                byte2 = m_pMemory->Retrieve(tile_address + 1);
            }

            int pixel_x_in_tile = map_tile_offset_x;

            if (cgb_tile_xflip)
            {
                pixel_x_in_tile = 7 - pixel_x_in_tile;
            }
            int pixel_x_in_tile_bit = 0x1 << (7 - pixel_x_in_tile);
            int pixel_data = (byte1 & pixel_x_in_tile_bit) ? 1 : 0;
            pixel_data |= (byte2 & pixel_x_in_tile_bit) ? 2 : 0;

            int index = line_width + screen_pixel_x;
            m_pColorCacheBuffer[index] = pixel_data & 0x03;

            if (m_bCGB)
            {
                bool cgb_tile_priority = IsSetBit(cgb_tile_attr, 7) && IsSetBit(lcdc, 0);
                if (cgb_tile_priority && (pixel_data != 0))
                    m_pColorCacheBuffer[index] = SetBit(m_pColorCacheBuffer[index], 2);
                m_pColorFrameBuffer[index] = m_CGBBackgroundPalettes[cgb_tile_pal][pixel_data][1];
            }
            else
            {
                u8 color = (palette >> (pixel_data << 1)) & 0x03;
                m_pColorFrameBuffer[index] = m_pFrameBuffer[index] = color;
            }
        }
    }
    else
    {
        for (int x = 0; x < pixels_to_render; x++)
        {
            int position = line_width + pixel + x;
            m_pColorFrameBuffer[position] = 0;
            m_pFrameBuffer[position] = 0;
            m_pColorCacheBuffer[position] = 0;
        }
    }
}

void Video::RenderWindow(int line)
{
    if (m_iWindowLine > 143)
        return;

    if (m_iWindowLine < 0)
    {
        m_iWindowLine = 0;
        return;
    }

    u8 lcdc = m_pMemory->Retrieve(0xFF40);
    if (!IsSetBit(lcdc, 5))
        return;

    if (!m_bWindowYTrigger)
        return;

    int wx = m_pMemory->Retrieve(0xFF4B) - 7;
    if (wx > 159)
        return;

    u8 wy = m_pMemory->Retrieve(0xFF4A);
    if ((wy > 143) || (wy > line))
        return;

    int tiles = IsSetBit(lcdc, 4) ? 0x8000 : 0x8800;
    int map = IsSetBit(lcdc, 6) ? 0x9C00 : 0x9800;
    int lineAdjusted = m_iWindowLine;
    int y_32 = (lineAdjusted >> 3) << 5;
    int pixely = lineAdjusted & 0x7;
    int pixely_2 = pixely << 1;
    int pixely_2_flip = (7 - pixely) << 1;
    int line_width = (line * m_iScreenWidth);
    u8 palette = m_pMemory->Retrieve(0xFF47);

    for (int x = 0; x < 32; x++)
    {
        int tile = 0;

        if (tiles == 0x8800)
        {
            tile = static_cast<s8> (m_pMemory->Retrieve(map + y_32 + x));
            tile += 128;
        }
        else
        {
            tile = m_pMemory->Retrieve(map + y_32 + x);
        }

        u8 cgb_tile_attr = m_bCGB ? m_pMemory->ReadCGBLCDRAM(map + y_32 + x, true) : 0;
        u8 cgb_tile_pal = m_bCGB ? (cgb_tile_attr & 0x07) : 0;
        bool cgb_tile_bank = m_bCGB ? IsSetBit(cgb_tile_attr, 3) : false;
        bool cgb_tile_xflip = m_bCGB ? IsSetBit(cgb_tile_attr, 5) : false;
        bool cgb_tile_yflip = m_bCGB ? IsSetBit(cgb_tile_attr, 6) : false;
        int mapOffsetX = x << 3;
        int tile_16 = tile << 4;
        u8 byte1 = 0;
        u8 byte2 = 0;
        int final_pixely_2 = (m_bCGB && cgb_tile_yflip) ? pixely_2_flip : pixely_2;
        int tile_address = tiles + tile_16 + final_pixely_2;

        if (m_bCGB && cgb_tile_bank)
        {
            byte1 = m_pMemory->ReadCGBLCDRAM(tile_address, true);
            byte2 = m_pMemory->ReadCGBLCDRAM(tile_address + 1, true);
        }
        else
        {
            byte1 = m_pMemory->Retrieve(tile_address);
            byte2 = m_pMemory->Retrieve(tile_address + 1);
        }

        for (int pixelx = 0; pixelx < 8; pixelx++)
        {
            // The window is screen-space, so it moves with the viewport origin
            // but keeps its native 160-pixel extent. M5 deliberately does not
            // stretch the HUD into the new margins: the window map holds no
            // authored content out there, so widening it would invent picture.
            int bufferX = (mapOffsetX + pixelx + wx) + m_iViewportOriginX;

            if (bufferX < m_iViewportOriginX
                || bufferX >= m_iViewportOriginX + GAMEBOY_WIDTH)
                continue;

            int pixelx_pos = pixelx;

            if (m_bCGB && cgb_tile_xflip)
            {
                pixelx_pos = 7 - pixelx_pos;
            }

            int pixel = (byte1 & (0x1 << (7 - pixelx_pos))) ? 1 : 0;
            pixel |= (byte2 & (0x1 << (7 - pixelx_pos))) ? 2 : 0;

            int position = line_width + bufferX;
            m_pColorCacheBuffer[position] = pixel & 0x03;

            if (m_bCGB)
            {
                bool cgb_tile_priority = IsSetBit(cgb_tile_attr, 7) && IsSetBit(lcdc, 0);
                if (cgb_tile_priority && (pixel != 0))
                    m_pColorCacheBuffer[position] = SetBit(m_pColorCacheBuffer[position], 2);
                 m_pColorFrameBuffer[position] = m_CGBBackgroundPalettes[cgb_tile_pal][pixel][1];
            }
            else
            {
                u8 color = (palette >> (pixel << 1)) & 0x03;
                m_pColorFrameBuffer[position] = m_pFrameBuffer[position] = color;
            }
        }
    }
    m_iWindowLine++;
}

void Video::RenderSprites(int line)
{
    u8 lcdc = m_pMemory->Retrieve(0xFF40);

    if (!IsSetBit(lcdc, 1))
        return;

    int sprite_height = IsSetBit(lcdc, 2) ? 16 : 8;
    int line_width = (line * m_iScreenWidth);

    // Sprites are positioned against the same viewport the background is drawn
    // with, or a level-edge shift would leave the player hanging off the terrain
    // (ADR 0009). RenderBG latches that origin from the gameplay rows, and
    // sprites take it on EVERY row: a sprite that crosses the HUD raster
    // boundary has to be drawn in one piece, and the seven rows above it have no
    // camera of their own to be positioned against.
    int origin_x = m_bWideScreen ? m_iLevelEdgeOriginX : m_iViewportOriginX;

    bool visible_sprites[40];
    int sprite_limit = 0;

    for (int sprite = 0; sprite < 40; sprite++)
    {
        int sprite_4 = sprite << 2;
        int sprite_y = m_pMemory->Retrieve(0xFE00 + sprite_4) - 16;

        if ((sprite_y > line) || ((sprite_y + sprite_height) <= line))
        {
            visible_sprites[sprite] = false;
            continue;
        }

        sprite_limit++;

        // Wide mode lifts the hardware ten-per-scanline limit (ADR 0003). Native
        // mode keeps it, and with it the game's own rotating-cursor flicker.
        visible_sprites[sprite] = m_bWideScreen || (sprite_limit <= 10);
    }

    for (int sprite = 39; sprite >= 0; sprite--)
    {
        if (!visible_sprites[sprite])
            continue;

        int sprite_4 = sprite << 2;
        // OAM X is screen-space, so it shifts with the viewport origin. In wide
        // mode it is also read as SIGNED above GAMEBOY_WIDE_OAM_X_SIGNED_MIN:
        // the register is one unsigned byte holding screen_x + 8, so an object
        // in the left margin writes a wrapped high value rather than a negative
        // one, and without this it would be drawn at the far right or culled.
        // The widened clip caps the largest value the game can write at 207 and
        // the smallest a left-margin sprite needs is 217, so the two ranges are
        // separated by an unused guard band and cannot be confused. Native mode
        // keeps the plain unsigned read, byte for byte. See docs/adr/0006.
        int oam_x = m_pMemory->Retrieve(0xFE00 + sprite_4 + 1);
        if (m_bWideScreen && (oam_x >= m_iWideOamXSignedMin))
            oam_x -= 256;
        int sprite_x = oam_x - 8 + origin_x;

        if ((sprite_x < -7) || (sprite_x >= m_iScreenWidth))
            continue;

        int sprite_y = m_pMemory->Retrieve(0xFE00 + sprite_4) - 16;
        int sprite_tile_16 = (m_pMemory->Retrieve(0xFE00 + sprite_4 + 2)
                & ((sprite_height == 16) ? 0xFE : 0xFF)) << 4;
        u8 sprite_flags = m_pMemory->Retrieve(0xFE00 + sprite_4 + 3);
        int sprite_pallette = IsSetBit(sprite_flags, 4) ? 1 : 0;
        u8 palette = m_pMemory->Retrieve(sprite_pallette ? 0xFF49 : 0xFF48);
        bool xflip = IsSetBit(sprite_flags, 5);
        bool yflip = IsSetBit(sprite_flags, 6);
        bool aboveBG = (!IsSetBit(sprite_flags, 7));
        bool cgb_tile_bank = IsSetBit(sprite_flags, 3);
        int cgb_tile_pal = sprite_flags & 0x07;
        int tiles = 0x8000;
        int pixel_y = yflip ? ((sprite_height == 16) ? 15 : 7) - (line - sprite_y) : line - sprite_y;
        u8 byte1 = 0;
        u8 byte2 = 0;
        int pixel_y_2 = 0;
        int offset = 0;

        if (sprite_height == 16 && (pixel_y >= 8))
        {
            pixel_y_2 = (pixel_y - 8) << 1;
            offset = 16;
        }
        else
            pixel_y_2 = pixel_y << 1;

        int tile_address = tiles + sprite_tile_16 + pixel_y_2 + offset;

        if (m_bCGB && cgb_tile_bank)
        {
            byte1 = m_pMemory->ReadCGBLCDRAM(tile_address, true);
            byte2 = m_pMemory->ReadCGBLCDRAM(tile_address + 1, true);
        }
        else
        {
            byte1 = m_pMemory->Retrieve(tile_address);
            byte2 = m_pMemory->Retrieve(tile_address + 1);
        }

        for (int pixelx = 0; pixelx < 8; pixelx++)
        {
            int pixel = (byte1 & (0x01 << (xflip ? pixelx : 7 - pixelx))) ? 1 : 0;
            pixel |= (byte2 & (0x01 << (xflip ? pixelx : 7 - pixelx))) ? 2 : 0;

            if (pixel == 0)
                continue;

            int bufferX = (sprite_x + pixelx);

            if (bufferX < 0 || bufferX >= m_iScreenWidth)
                continue;

            int position = line_width + bufferX;
            u8 color_cache = m_pColorCacheBuffer[position];

            if (m_bCGB)
            {
                if (IsSetBit(color_cache, 2))
                    continue;
            }
            else
            {
                int sprite_x_cache = m_pSpriteXCacheBuffer[position];
                if (IsSetBit(color_cache, 3) && (sprite_x_cache < sprite_x))
                    continue;
            }

            if (!aboveBG && (color_cache & 0x03))
                continue;

            m_pColorCacheBuffer[position] = SetBit(color_cache, 3);
            m_pSpriteXCacheBuffer[position] = sprite_x;
            if (m_bCGB)
            {
                m_pColorFrameBuffer[position] = m_CGBSpritePalettes[cgb_tile_pal][pixel][1];
            }
            else
            {
                u8 color = (palette >> (pixel << 1)) & 0x03;
                m_pColorFrameBuffer[position] = m_pFrameBuffer[position] = color;
            }
        }
    }
}

void Video::UpdateStatRegister()
{
    // Updates the STAT register with current mode
    u8 stat = m_pMemory->Retrieve(0xFF41);
    m_pMemory->Load(0xFF41, (stat & 0xFC) | (m_iStatusMode & 0x3));
}

void Video::CompareLYToLYC()
{
    if (m_bScreenEnabled)
    {
        u8 lyc = m_pMemory->Retrieve(0xFF45);
        u8 stat = m_pMemory->Retrieve(0xFF41);

        if (lyc == m_iStatusModeLYCounter)
        {
            stat = SetBit(stat, 2);
        }
        else
        {
            stat = UnsetBit(stat, 2);
        }

        m_pMemory->Load(0xFF41, stat);
        RefreshStatInterruptSignal(true);
    }
}

u8 Video::GetIRQ48Signal() const
{
    return m_IRQ48Signal;
}

void Video::SetIRQ48Signal(u8 signal)
{
    m_IRQ48Signal = signal;
}

void Video::SaveState(std::ostream& stream)
{
    using namespace std;

    // Deliberately still GAMEBOY_WIDTH: the savestate format is unchanged, so
    // states stay compatible in both directions. These three buffers are rebuilt
    // every scanline, so saving only the native-sized prefix loses nothing.
    stream.write(reinterpret_cast<const char*> (m_pFrameBuffer), GAMEBOY_WIDTH * GAMEBOY_HEIGHT);
    stream.write(reinterpret_cast<const char*> (m_pSpriteXCacheBuffer), sizeof(int) * GAMEBOY_WIDTH * GAMEBOY_HEIGHT);
    stream.write(reinterpret_cast<const char*> (m_pColorCacheBuffer), GAMEBOY_WIDTH * GAMEBOY_HEIGHT);
    stream.write(reinterpret_cast<const char*> (&m_iStatusMode), sizeof(m_iStatusMode));
    stream.write(reinterpret_cast<const char*> (&m_iStatusModeCounter), sizeof(m_iStatusModeCounter));
    stream.write(reinterpret_cast<const char*> (&m_iStatusModeCounterAux), sizeof(m_iStatusModeCounterAux));
    stream.write(reinterpret_cast<const char*> (&m_iStatusModeLYCounter), sizeof(m_iStatusModeLYCounter));
    stream.write(reinterpret_cast<const char*> (&m_iScreenEnableDelayCycles), sizeof(m_iScreenEnableDelayCycles));
    stream.write(reinterpret_cast<const char*> (&m_iStatusVBlankLine), sizeof(m_iStatusVBlankLine));
    stream.write(reinterpret_cast<const char*> (&m_iPixelCounter), sizeof(m_iPixelCounter));
    stream.write(reinterpret_cast<const char*> (&m_iTileCycleCounter), sizeof(m_iTileCycleCounter));
    stream.write(reinterpret_cast<const char*> (&m_bScreenEnabled), sizeof(m_bScreenEnabled));
    stream.write(reinterpret_cast<const char*> (m_CGBSpritePalettes), sizeof(m_CGBSpritePalettes));
    stream.write(reinterpret_cast<const char*> (m_CGBBackgroundPalettes), sizeof(m_CGBBackgroundPalettes));
    stream.write(reinterpret_cast<const char*> (&m_bScanLineTransfered), sizeof(m_bScanLineTransfered));
    stream.write(reinterpret_cast<const char*> (&m_iWindowLine), sizeof(m_iWindowLine));
    stream.write(reinterpret_cast<const char*> (&m_iHideFrames), sizeof(m_iHideFrames));
    stream.write(reinterpret_cast<const char*> (&m_IRQ48Signal), sizeof(m_IRQ48Signal));
    stream.write(reinterpret_cast<const char*> (&m_iPendingVBlankInterruptCycles), sizeof(m_iPendingVBlankInterruptCycles));
    stream.write(reinterpret_cast<const char*> (&m_bWindowYTrigger), sizeof(m_bWindowYTrigger));
}

void Video::LoadState(std::istream& stream, u32 version)
{
    using namespace std;

    stream.read(reinterpret_cast<char*> (m_pFrameBuffer), GAMEBOY_WIDTH * GAMEBOY_HEIGHT);
    stream.read(reinterpret_cast<char*> (m_pSpriteXCacheBuffer), sizeof(int) * GAMEBOY_WIDTH * GAMEBOY_HEIGHT);
    stream.read(reinterpret_cast<char*> (m_pColorCacheBuffer), GAMEBOY_WIDTH * GAMEBOY_HEIGHT);
    stream.read(reinterpret_cast<char*> (&m_iStatusMode), sizeof(m_iStatusMode));
    stream.read(reinterpret_cast<char*> (&m_iStatusModeCounter), sizeof(m_iStatusModeCounter));
    stream.read(reinterpret_cast<char*> (&m_iStatusModeCounterAux), sizeof(m_iStatusModeCounterAux));
    stream.read(reinterpret_cast<char*> (&m_iStatusModeLYCounter), sizeof(m_iStatusModeLYCounter));
    stream.read(reinterpret_cast<char*> (&m_iScreenEnableDelayCycles), sizeof(m_iScreenEnableDelayCycles));
    stream.read(reinterpret_cast<char*> (&m_iStatusVBlankLine), sizeof(m_iStatusVBlankLine));
    stream.read(reinterpret_cast<char*> (&m_iPixelCounter), sizeof(m_iPixelCounter));
    stream.read(reinterpret_cast<char*> (&m_iTileCycleCounter), sizeof(m_iTileCycleCounter));
    stream.read(reinterpret_cast<char*> (&m_bScreenEnabled), sizeof(m_bScreenEnabled));
    stream.read(reinterpret_cast<char*> (m_CGBSpritePalettes), sizeof(m_CGBSpritePalettes));
    stream.read(reinterpret_cast<char*> (m_CGBBackgroundPalettes), sizeof(m_CGBBackgroundPalettes));
    stream.read(reinterpret_cast<char*> (&m_bScanLineTransfered), sizeof(m_bScanLineTransfered));
    stream.read(reinterpret_cast<char*> (&m_iWindowLine), sizeof(m_iWindowLine));
    stream.read(reinterpret_cast<char*> (&m_iHideFrames), sizeof(m_iHideFrames));
    stream.read(reinterpret_cast<char*> (&m_IRQ48Signal), sizeof(m_IRQ48Signal));

    if (version >= 101)
    {
        stream.read(reinterpret_cast<char*> (&m_iPendingVBlankInterruptCycles), sizeof(m_iPendingVBlankInterruptCycles));
    }
    else
    {
        m_iPendingVBlankInterruptCycles = 0;
    }

    if (version >= 102)
    {
        stream.read(reinterpret_cast<char*> (&m_bWindowYTrigger), sizeof(m_bWindowYTrigger));
    }
    else
    {
        m_bWindowYTrigger = false;
    }
}

PaletteMatrix Video::GetCGBBackgroundPalettes()
{
    return &m_CGBBackgroundPalettes;
}

PaletteMatrix Video::GetCGBSpritePalettes()
{
    return &m_CGBSpritePalettes;
}
