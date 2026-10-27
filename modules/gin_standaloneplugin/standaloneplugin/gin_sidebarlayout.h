#pragma once

namespace gin
{
// Keep every section reachable in a viewport, even with a short plugin editor.
struct SidebarLayout
{
    juce::Rectangle<int> midi, sample, spectrum, scope, xy, record, output;
    int height = 0;

    static SidebarLayout create (int width, bool showMidi, bool showSample, bool showXY)
    {
        SidebarLayout layout;
        const int contentWidth = juce::jmax (0, width - 4);
        int y = 2;
        auto section = [&] (int h)
        {
            juce::Rectangle<int> bounds (2, y, contentWidth, h);
            y += h + 8;
            return bounds;
        };

        if (showMidi)   layout.midi = section (71);
        if (showSample) layout.sample = section (71);
        layout.spectrum = section (118);
        layout.scope = section (118);
        if (showXY)    layout.xy = section (18 + contentWidth);
        layout.record = section (71);
        layout.output = section (38);
        layout.height = y - 8 + 2;
        return layout;
    }
};
}
