#pragma once

#include <cmath>
#include <functional>
#include <vector>

namespace gin
{
//==============================================================================
// Estimates the true peak (the peak of the reconstructed analog signal, which
// can be above every sample) by 4x oversampling, as ITU-R BS.1770 does. Each
// input sample is checked along with three windowed sinc interpolations
// between it and the sample before.
class TruePeakMeter
{
public:
    explicit TruePeakMeter (int numChannels)
        : history (size_t (numChannels), std::vector<float> (size_t (2 * length), 0.0f)),
          position (size_t (numChannels), 0)
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            const double fraction = (phase + 1) / 4.0;
            double sum = 0.0;

            for (int j = 0; j < length; ++j)
            {
                // Distance from the point being interpolated, which is
                // between taps half - 1 and half
                const double t = j - (half - 1) - fraction;
                const double x = juce::MathConstants<double>::pi * t;
                const double sinc = std::sin (x) / x;
                const double window = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * t / half);
                taps[phase][j] = sinc * window;
                sum += taps[phase][j];
            }

            // Unity gain at DC, so a constant isn't read as louder than it is
            for (auto& tap : taps[phase])
                tap /= sum;
        }
    }

    void process (const juce::AudioBuffer<float>& buffer, int start, int num)
    {
        const int channels = std::min (buffer.getNumChannels(), int (history.size()));

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < num; ++i)
                push (ch, buffer.getSample (ch, start + i));
    }

    // Runs the history out, so the interpolations after the last sample count
    void finish()
    {
        for (int ch = 0; ch < int (history.size()); ++ch)
            for (int i = 0; i < half; ++i)
                push (ch, 0.0f);
    }

    float getPeak() const { return float (peak); }

private:
    void push (int ch, float sample)
    {
        // Written twice, so the last 'length' samples are always contiguous
        auto& h = history[size_t (ch)];
        auto& pos = position[size_t (ch)];
        h[size_t (pos)] = sample;
        h[size_t (pos + length)] = sample;
        pos = (pos + 1) % length;

        const float* window = h.data() + pos;

        peak = std::max (peak, double (std::abs (sample)));

        for (const auto& phase : taps)
        {
            double y = 0.0;
            for (int j = 0; j < length; ++j)
                y += phase[j] * window[j];

            peak = std::max (peak, std::abs (y));
        }
    }

    static constexpr int half = 16, length = 2 * half;

    double taps[3][length] {};
    std::vector<std::vector<float>> history;
    std::vector<int> position;
    double peak = 0.0;
};

//==============================================================================
// Renders a MIDI file through a processor, faster than real time, to a WAV.
//
// The written file can't clip: the render goes to a temporary float file
// first while its true peak is measured, then it's written at a gain that
// keeps the true peak at or under the ceiling. With normalize, the peak is
// brought to the ceiling. Without it the level is kept unless it would go over
// the ceiling, and then it's turned down just enough.
class MidiFileExporter
{
public:
    struct Options
    {
        bool normalize = false;
        double ceilingDb = -1.0;         // dBTP, 0 at most
        int bitsPerSample = 24;
        int blockSize = 512;

        // After the file ends, rendering goes on until the output has stayed
        // under silenceDb for silenceSeconds, up to maxTailSeconds (then it
        // fades out over fadeSeconds)
        double silenceDb = -90.0;
        double silenceSeconds = 0.5;
        double maxTailSeconds = 30.0;
        double fadeSeconds = 1.0;
    };

    struct Result
    {
        juce::Result status = juce::Result::ok();
        bool cancelled = false;
        double seconds = 0.0;
        float renderedPeak = 0.0f;   // true peak of the render, as a gain
        float gain = 1.0f;           // what the render was multiplied by
        bool tailCut = false;        // still sounding at maxTailSeconds

        float getWrittenPeak() const { return renderedPeak * gain; }
    };

    // Returns false from progress to cancel. The processor should be one of
    // its own (not one a device is playing): it's prepared, played and
    // released here, from the calling thread.
    static Result render (juce::AudioProcessor& processor, const juce::File& midiFile, const juce::File& wavFile,
                          double sampleRate, const Options& options,
                          std::function<bool (double progress)> progress = {})
    {
        Result result;
        auto fail = [&] (const juce::String& message)
        {
            result.status = juce::Result::fail (message);
            return result;
        };
        auto keepGoing = [&] (double p)
        {
            if (progress && ! progress (juce::jlimit (0.0, 1.0, p)))
                result.cancelled = true;

            return ! result.cancelled;
        };

        const int outputs = processor.getTotalNumOutputChannels();
        const int blockSize = std::max (1, options.blockSize);

        if (outputs <= 0)
            return fail ("The plugin has no outputs");

        if (sampleRate <= 0.0)
            return fail ("No sample rate");

        {
            // The player only plays files timed in ticks
            juce::FileInputStream stream (midiFile);
            juce::MidiFile file;

            if (! stream.openedOk() || ! file.readFrom (stream))
                return fail ("Couldn't read " + midiFile.getFileName());

            if (file.getTimeFormat() <= 0)
                return fail ("SMPTE timed MIDI files aren't supported");
        }

        MidiFilePlayer player;
        if (! player.load (midiFile))
            return fail ("Couldn't read " + midiFile.getFileName());

        player.setSampleRate (sampleRate);
        player.setLooping (false);
        player.play();

        const double length = player.getLengthInSeconds();

        // Pass 1: render to a float file, measuring the true peak
        juce::TemporaryFile rendered (".wav");
        juce::WavAudioFormat wav;
        TruePeakMeter meter (outputs);
        juce::int64 numRendered = 0;

        {
            std::unique_ptr<juce::OutputStream> stream (rendered.getFile().createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer;

            if (stream != nullptr)
                writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                          .withSampleRate (sampleRate)
                                                          .withNumChannels (outputs)
                                                          .withBitsPerSample (32)
                                                          .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            if (writer == nullptr)
                return fail ("Couldn't create a temporary file");

            struct Playhead : public juce::AudioPlayHead
            {
                juce::Optional<PositionInfo> getPosition() const override { return info; }
                PositionInfo info;
            };

            Playhead playhead;

            processor.setPlayHead (&playhead);
            processor.setNonRealtime (true);
            processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
            processor.prepareToPlay (sampleRate, blockSize);

            const juce::ScopeGuard release { [&]
            {
                processor.releaseResources();
                processor.setPlayHead (nullptr);
            } };

            // The processor's output starts this late
            juce::int64 toSkip = processor.getLatencySamples();

            const auto threshold = juce::Decibels::decibelsToGain (float (options.silenceDb));
            const auto silenceNeeded = juce::int64 (options.silenceSeconds * sampleRate);
            const auto maxTail = juce::int64 (options.maxTailSeconds * sampleRate);
            juce::int64 tail = 0, silent = 0;

            juce::AudioBuffer<float> buffer (std::max (outputs, processor.getTotalNumInputChannels()), blockSize);
            juce::MidiBuffer midi;

            while (true)
            {
                const bool fileEnded = ! player.isPlaying();

                midi.clear();
                playhead.info = player.populatePositionInfo();
                player.processBlock (blockSize, midi);

                buffer.clear();
                processor.processBlock (buffer, midi);

                const int start = int (std::min<juce::int64> (toSkip, blockSize));
                const int num = blockSize - start;
                toSkip -= start;

                float blockPeak = 0.0f;
                for (int ch = 0; ch < outputs; ++ch)
                {
                    const float* data = buffer.getReadPointer (ch, start);

                    for (int i = 0; i < num; ++i)
                    {
                        if (! std::isfinite (data[i]))
                            return fail ("The plugin made invalid samples (NaN or infinity)");

                        blockPeak = std::max (blockPeak, std::abs (data[i]));
                    }
                }

                if (num > 0)
                {
                    meter.process (buffer, start, num);

                    if (! writer->writeFromAudioSampleBuffer (buffer, start, num))
                        return fail ("Couldn't write the temporary file");

                    numRendered += num;
                }

                if (fileEnded && toSkip == 0)
                {
                    tail += num;
                    silent = blockPeak < threshold ? silent + num : 0;

                    if (silent >= silenceNeeded)
                        break;

                    if (tail >= maxTail)
                    {
                        result.tailCut = true;
                        break;
                    }
                }

                const double done = fileEnded ? 0.8 + 0.05 * double (tail) / double (std::max<juce::int64> (1, maxTail))
                                              : 0.8 * player.getPlayheadPosition() / std::max (length, 1.0e-9);
                if (! keepGoing (done))
                    return result;
            }
        }

        meter.finish();
        result.renderedPeak = meter.getPeak();
        result.seconds = double (numRendered) / sampleRate;

        const auto ceiling = juce::Decibels::decibelsToGain (float (std::min (0.0, options.ceilingDb)));

        if (result.renderedPeak > 0.0f)
        {
            const auto fit = ceiling / result.renderedPeak;
            result.gain = options.normalize ? fit : std::min (1.0f, fit);
        }

        // Pass 2: write the file at that gain. It replaces the target only
        // once it's complete.
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (rendered.getFile().createInputStream().release(), true));
        if (reader == nullptr)
            return fail ("Couldn't read the temporary file");

        juce::TemporaryFile output (wavFile);

        {
            std::unique_ptr<juce::OutputStream> stream (output.getFile().createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer;

            if (stream != nullptr)
                writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                          .withSampleRate (sampleRate)
                                                          .withChannelLayout (juce::AudioChannelSet::canonicalChannelSet (outputs))
                                                          .withBitsPerSample (options.bitsPerSample));
            if (writer == nullptr)
                return fail ("Couldn't create " + wavFile.getFullPathName());

            const auto fadeLength = result.tailCut ? std::min (numRendered, juce::int64 (options.fadeSeconds * sampleRate)) : 0;
            const auto fadeStart = numRendered - fadeLength;

            juce::AudioBuffer<float> buffer (outputs, 8192);

            for (juce::int64 pos = 0; pos < numRendered; pos += buffer.getNumSamples())
            {
                const int num = int (std::min<juce::int64> (buffer.getNumSamples(), numRendered - pos));

                if (! reader->read (&buffer, 0, num, pos, true, true))
                    return fail ("Couldn't read the temporary file");

                buffer.applyGain (0, num, result.gain);

                for (int i = 0; i < num; ++i)
                {
                    if (pos + i < fadeStart)
                        continue;

                    const auto g = float (numRendered - (pos + i)) / float (fadeLength);
                    for (int ch = 0; ch < outputs; ++ch)
                        buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
                }

                if (! writer->writeFromAudioSampleBuffer (buffer, 0, num))
                    return fail ("Couldn't write " + wavFile.getFullPathName());

                if (! keepGoing (0.85 + 0.15 * double (pos + num) / double (numRendered)))
                    return result;
            }
        }

        if (! output.overwriteTargetFileWithTemporary())
            return fail ("Couldn't replace " + wavFile.getFullPathName());

        if (progress)
            progress (1.0);

        return result;
    }
};
}
