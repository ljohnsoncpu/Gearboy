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

#ifndef VIDEO_H
#define	VIDEO_H

#include "definitions.h"
#include "TraceLogger.h"
#include "GameAdaptation.h"

class Memory;
class Processor;

typedef u16 (*PaletteMatrix)[8][4][2];

class Video
{
public:
    Video(Memory* pMemory, Processor* pProcessor);
    ~Video();
    void Init();
    void Reset(bool bCGB);
    void ResetToBootromState();
    bool Tick(unsigned int &clockCycles, u16* pColorFrameBuffer, GB_Color_Format pixelFormat);
    void EnableScreen();
    void DisableScreen();
    void SetSGBTransferMode(bool enabled);
    // SMBDX widescreen: selects the runtime output width. Native mode leaves
    // every render path arithmetically identical to upstream. `width` is
    // clamped to [GAMEBOY_WIDE_MIN_WIDTH, GAMEBOY_WIDE_MAX_WIDTH]; see project
    // ADR 0007 for why that maximum is a measurement, not a preference.
    void SetWideScreen(bool enabled, int width = GAMEBOY_WIDE_WIDTH);
    bool IsWideScreen() const;
    int GetScreenWidth() const;
    // SMBDX widescreen: where the recognised game keeps its horizontal level
    // bounds, or NULL to fill nothing. With bounds, wide mode paints the
    // background colour over any MARGIN pixel whose world column is outside the
    // level instead of showing the background ring slot that happens to share
    // its position. The native 160-pixel window is never touched. ADR 0009.
    void SetLevelBounds(const GameLevelBounds* bounds);
    bool HasLevelBounds() const;
    bool IsScreenEnabled() const;
    const u8* GetFrameBuffer() const;
    const u16* GetColorFrameBuffer() const;
    void UpdatePaletteToSpecification(bool background, u8 value);
    void SetColorPalette(bool background, u8 value);
    bool VRAMAccessBlocked() const;
    bool CGBPaletteAccessBlocked() const;
    int GetCurrentStatusMode() const;
    void RefreshStatInterruptSignal(bool requestInterrupt);
    void ResetWindowLine();
    void CheckWindowY();
    void CompareLYToLYC();
    u8 GetIRQ48Signal() const;
    void SetIRQ48Signal(u8 signal);
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream, u32 version = GB_SAVESTATE_VERSION);
    PaletteMatrix GetCGBBackgroundPalettes();
    PaletteMatrix GetCGBSpritePalettes();
    void SetTraceLogger(TraceLogger* pTraceLogger);

private:
    void ScanLine(int line);
    void RenderBG(int line, int pixel, int pixels_to_render);
    // Where this scanline's viewport sits and what of it is still outside the
    // level: `origin_x` is the viewport origin to draw the background and the
    // sprites with, and `[left_end, right_start)` the output columns inside the
    // level. Returns false - leaving the plain viewport origin and nothing to
    // fill - for every native frame and every game with no bounds. ADR 0009.
    bool LevelEdgeView(u8 scroll_x, u8 scroll_y, int& origin_x,
                       int& left_end, int& right_start) const;
    void RenderWindow(int line);
    void RenderSprites(int line);
    void UpdateStatRegister();
    INLINE void TraceEvent(u8 event, u8 value);
    void LogTraceEvent(u8 event, u8 value);

private:
    Memory* m_pMemory;
    Processor* m_pProcessor;
    u8* m_pFrameBuffer;
    u16* m_pColorFrameBuffer;
    int* m_pSpriteXCacheBuffer;
    u8* m_pColorCacheBuffer;
    int m_iStatusMode;
    int m_iStatusModeCounter;
    int m_iStatusModeCounterAux;
    int m_iPendingVBlankInterruptCycles;
    int m_iStatusModeLYCounter;
    int m_iScreenEnableDelayCycles;
    int m_iStatusVBlankLine;
    int m_iPixelCounter;
    int m_iTileCycleCounter;
    bool m_bScreenEnabled;
    bool m_bCGB;
    bool m_bSGBTransferMode;
    // Runtime output width and the left-hand offset of the native 160-pixel
    // window inside it. Native mode is width GAMEBOY_WIDTH at origin 0.
    bool m_bWideScreen;
    int m_iScreenWidth;
    int m_iViewportOriginX;
    // The OAM X at or above which wide mode reads the byte as signed. Derived
    // from the margin, because the values it separates both move with it.
    int m_iWideOamXSignedMin;
    // Read-only addresses of the loaded game's horizontal level bounds, or NULL.
    const GameLevelBounds* m_pLevelBounds;
    // Mutable level state latched on scanline 0. The game updates these values
    // during the visible frame; reading them independently on every scanline
    // can observe a transient snapshot and move one row to another ring page.
    bool m_bLevelEdgeGameplay;
    int m_iLevelEdgeCameraX;
    int m_iLevelEdgeScreenCount;
    // The viewport origin the level-edge clamp last chose on a gameplay row.
    // Sprites use it on every row, including the HUD band, so that one crossing
    // the raster boundary is drawn in one piece rather than in two halves.
    int m_iLevelEdgeOriginX;
    u16 m_CGBSpritePalettes[8][4][2];
    u16 m_CGBBackgroundPalettes[8][4][2];
    bool m_bScanLineTransfered;
    int m_iWindowLine;
    bool m_bWindowYTrigger;
    int m_iHideFrames;
    u8 m_IRQ48Signal;
    GB_Color_Format m_pixelFormat;
    TraceLogger* m_pTraceLogger;
};

INLINE void Video::TraceEvent(u8 event, u8 value)
{
    if (m_pTraceLogger->IsEventEnabled(TRACE_LCD, event))
        LogTraceEvent(event, value);
}

#endif	/* VIDEO_H */
