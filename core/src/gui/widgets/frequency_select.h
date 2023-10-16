#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <stdint.h>

class FrequencySelect {
public:
    FrequencySelect();
    void init();
    void draw();
    void setFrequency(int64_t freq);

    uint64_t frequency;
    uint64_t old_freq =0;
    bool frequencyChanged = false;
    bool digitHovered = false;

    bool limitFreq;
    uint64_t minFreq;
    uint64_t maxFreq;


private:
    void onPosChange();
    void onResize();
    void incrementDigit(int i);
    void decrementDigit(int i);
    void moveCursorToDigit(int i);

    ImVec2 widgetPos;
    ImVec2 lastWidgetPos;

    int digits[12];
    ImVec2 digitBottomMins[12];
    ImVec2 digitTopMins[12];
    ImVec2 digitBottomMaxs[12];
    ImVec2 digitTopMaxs[12];

    char buf[100];
};