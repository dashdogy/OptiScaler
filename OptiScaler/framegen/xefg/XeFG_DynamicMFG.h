#pragma once

#include "XeFG_DynamicCount.h"
#include "XeFG_DynamicCap.h"

#include <Windows.h>

// Dynamic MFG for an XeFG or DLSSG output: it passes the wait it measured and applies the result its own way
class XeFG_DynamicMFG
{
  public:
    // The output's own settings
    struct Settings
    {
        bool enabled = false;
        float targetFps = 0.0f;   // 0 = the FPS limit, else just under the refresh rate
        int rounding = 1;         // 0 at most, 1 around, 2 at least the target
        bool supports1X = false;  // the output can switch frame generation off and keep its count
        bool allow1X = false;     // with supports1X: may use 1X (at 2X it is the only other step)
        bool targetAsCap = false; // the target is also the FPS limit (FrameLimit::targetFps)
    };

    // [XeFG]: 1X and the cap
    static Settings XeFGSettings();

    // [DLSSG]: changes go to Streamline's numFramesToGenerate. No 1X (DLSSG's mode off isn't known to be
    // seamless) and no cap (FrameLimit::targetFps only holds XeFG's).
    static Settings DLSSGSettings();

    struct Result
    {
        bool dynamic = false;       // Dynamic MFG runs this frame
        bool ownChange = false;     // it ran last frame too, a count change is its own and not the user's
        int count = 1;              // interpolated frames to use, kept while generation is off
        bool generationOff = false; // 1X: no frames generated
    };

    // presentWaitMs: what the game waited in the output's present path this frame
    Result Update(const Settings& settings, int targetCount, int framesToInterpolate, bool generationOff,
                  double presentWaitMs, HWND window);

    // FG back on: the averages start over, what was learned stays
    void Restart();

  private:
    XeFG_DynamicCount _count;
    XeFG_DynamicCap _cap; // with "Use target as cap": holds the target, no rounding band, no 1X
    bool _active = false;
    bool _capActive = false;
    double _logMs = 0.0;
    int _refreshRate = 0;
    double _refreshCheckedMs = 0.0;
    bool _unfocused = false; // the game's window was in the background last frame

    // 1: the game is in front, 0: in the background, -1: no foreground window
    static int Focused(HWND window);
    float DisplayRefreshFps(double nowMs, HWND window);
    float TargetFps(double nowMs, HWND window, float setTargetFps);
};
