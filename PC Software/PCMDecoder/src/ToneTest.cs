// The sample rate loopback test: a 1 kHz sine out a soundcard of
// choice, through the board's ADC and the picture, and the frequency
// of what the decoder delivers measured against the stream's nominal
// rate. With both clocks exact the tone reads 1000.000 Hz; the ppm
// deviation shown is the tone card's clock error minus the board's
// ADC clock error, since the decoder counts samples rather than time.
using System;
using System.Collections.Generic;
using NAudio.Wave;

namespace PcmDecoder;

// Plays the tone until disposed. Created on the UI thread (the button
// handler), which is also what an ASIO driver requires.
public sealed class TonePlayer : IDisposable
{
    public const double ToneHz = 1000.0;
    private readonly IWavePlayer _out;

    public TonePlayer(OutputDevice device)
    {
        var provider = new SineProvider();
        if (device.AsioDriver != null)
        {
            var o = new AsioOut(device.AsioDriver) { ChannelOffset = device.AsioChannelOffset };
            o.Init(provider);
            _out = o;
        }
        else
        {
            var o = new WaveOutEvent { DeviceNumber = device.WaveOutIndex, DesiredLatency = 200 };
            o.Init(provider);
            _out = o;
        }
        _out.Play();
    }

    public void Dispose()
    {
        _out.Stop();
        _out.Dispose();
    }

    // -6 dBFS on both channels: full scale would clip the board's ADC
    // input stage before the measurement ever sees a clean sine.
    private sealed class SineProvider : IWaveProvider
    {
        public WaveFormat WaveFormat { get; } = new WaveFormat(48000, 16, 2);
        private double _phase;

        public int Read(byte[] buffer, int offset, int count)
        {
            int frames = count / 4;
            double step = 2 * Math.PI * ToneHz / WaveFormat.SampleRate;
            for (int i = 0; i < frames; i++)
            {
                short v = (short)(Math.Sin(_phase) * 0.5 * 32767);
                _phase += step;
                if (_phase >= 2 * Math.PI) _phase -= 2 * Math.PI;
                BitConverter.TryWriteBytes(buffer.AsSpan(offset + i * 4, 2), v);
                BitConverter.TryWriteBytes(buffer.AsSpan(offset + i * 4 + 2, 2), v);
            }
            return count;
        }
    }
}

// Measures the frequency of the decoded left channel against the
// stream's nominal rate: upward zero crossings with linear
// interpolation, cycles counted between the first and last crossing of
// a two second window. Two thousand interpolated crossings average the
// quantisation jitter out to well under a ppm on a clean sine; without
// a strong periodic signal (level or cycle count too low, or a result
// outside 20 Hz..20 kHz) the reading is 0.
public sealed class FrequencyMeter
{
    private int _rate;
    private double _firstCross = -1, _lastCross;
    private long _cycles;
    private long _index, _windowStart;
    private short _prev;
    private bool _have;
    private int _peak;
    // Every individual crossing-to-crossing period of the window, in
    // samples: the median is immune to a handful of dropouts or phase
    // jumps that drag the mean, and the spread tells the two apart.
    private readonly List<double> _periods = new();
    private double _lastPos = -1;

    // The last completed window's frequency in Hz (median based, so a
    // few bad periods do not move it); 0 = no measurement.
    public double Hz { get; private set; }
    // One line of statistics per window, for the log.
    public string Diag { get; private set; } = "";

    public void Feed(IReadOnlyList<SamplePair> pairs, int rate)
    {
        if (rate != _rate) { _rate = rate; Reset(); }
        for (int i = 0; i < pairs.Count; i++)
        {
            short cur = pairs[i].Left;
            int a = Math.Abs((int)cur);
            if (a > _peak) _peak = a;
            if (_have && _prev < 0 && cur >= 0)
            {
                // The zero lies a fraction past the previous sample.
                double frac = _prev / (double)(_prev - cur);
                double pos = _index - 1 + frac;
                if (_firstCross < 0) _firstCross = pos;
                else { _cycles++; _lastCross = pos; _periods.Add(pos - _lastPos); }
                _lastPos = pos;
            }
            _prev = cur;
            _have = true;
            _index++;
        }

        if (_index - _windowStart >= _rate * 2L)
        {
            double hz = 0;
            Diag = "";
            if (_cycles >= 40 && _peak > 1500 && _lastCross > _firstCross && _periods.Count >= 40)
            {
                double mean = _cycles * (double)_rate / (_lastCross - _firstCross);
                var sorted = new List<double>(_periods);
                sorted.Sort();
                double median = sorted[sorted.Count / 2];
                double medianHz = _rate / median;
                // Off by more than 5% of the median: a dropout, a phase
                // jump or a concealed stretch, not clock drift.
                int outliers = 0;
                foreach (double p in _periods)
                    if (Math.Abs(p - median) > median * 0.05) outliers++;
                Diag = $"mean={mean:0.000} median={medianHz:0.000} Hz  cycles={_cycles}  " +
                       $"period[min={sorted[0]:0.00} med={median:0.00} max={sorted[^1]:0.00} smp]  " +
                       $"outliers={outliers}  peak={_peak}";
                hz = medianHz;
                if (hz < 20 || hz > 20000) hz = 0;
                // A quarter of the periods off by more than 5% is not a
                // tone with a few dropouts, it is noise (the ADC input's
                // idle interference reads 92% outliers): no measurement.
                if (outliers * 4 > _periods.Count) hz = 0;
            }
            Hz = hz;
            _windowStart = _index;
            // The last crossing seeds the next window, so no cycles are
            // lost between windows.
            if (_cycles > 0) { _firstCross = _lastCross; }
            else { _firstCross = -1; _lastPos = -1; }
            _cycles = 0;
            _peak = 0;
            _periods.Clear();
        }
    }

    private void Reset()
    {
        _firstCross = -1;
        _cycles = 0;
        _index = 0;
        _windowStart = 0;
        _have = false;
        _peak = 0;
        _periods.Clear();
        _lastPos = -1;
        Hz = 0;
        Diag = "";
    }
}
