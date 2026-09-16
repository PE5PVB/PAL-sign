// Plays decoded stereo pairs out a soundcard. The card is always
// opened at 48 kHz, 16 bit; the source rate (48 kHz Ham HQ, 44.1 kHz
// Sony, 32 and 16 kHz for the low Ham modes) is resampled to it here.
// Opening the card at the source rate instead played the Sony cards
// audibly sharp on a GIGAPORT eX (observed 30 August 2026): the
// stream came out at the wrong pitch, so that card does not resample
// a 44.1 kHz stream to whatever clock it is running on. One fixed
// output rate makes the pitch right by construction on any card.
//
// Two output routes: WinMM WaveOut (any Windows device, with ~100 ms
// of device-side buffering) and ASIO, which plays straight from the
// driver's own buffers of a few ms and also lets the held level be
// lower: the low-latency route.
//
// The board's clock and the soundcard's clock are two independent
// crystals, so the incoming and outgoing rates never agree exactly;
// left alone, the buffer between them drains to empty (silence padded
// in at every read) or fills up (bursts discarded). Measured: the board
// delivered ~47872 pairs/s against a 48000 Hz output, and the buffer
// ran dry after ~20 s with underruns every second from then on. So the
// output is resampled with a ratio steered by the fill level: a slow
// PI loop, the P term holding the level, the I term learning the real
// rate difference.
using System;
using System.Collections.Generic;
using NAudio.Wave;

namespace PcmDecoder;

// One playback route, as listed by AudioPlayer.ListDevices(): a WinMM
// WaveOut device (AsioDriver null; index -1 is the system default), or
// an ASIO driver with the stereo pair to play on (one ASIO driver
// carries all of a card's channels, so the pair is part of the route).
public sealed record OutputDevice(string Name, int WaveOutIndex, string? AsioDriver, int AsioChannelOffset);

public sealed class AudioPlayer : IDisposable
{
    private readonly BufferedWaveProvider _buffer;
    private readonly AdaptiveResampler _resampler;
    private readonly IWavePlayer _output;
    // The source rate; the card itself always runs at OutputRate.
    public int SampleRate { get; }
    public const int OutputRate = 48000;

    // device: one row of ListDevices(). sampleRate: the rate the
    // decoded pairs arrive at (48000, 44100, 32000 or 16000).
    // playSync: the context of the thread this player was created on
    // (the UI thread); Play() is posted there, because an ASIO driver
    // wants its calls from the thread that opened it. targetMs
    // overrides the held buffer level (the window's Buffer box).
    public AudioPlayer(OutputDevice device, int sampleRate = 48000,
                       System.Threading.SynchronizationContext? playSync = null,
                       int? targetMs = null)
    {
        _playSync = playSync;
        SampleRate = sampleRate;
        bool asio = device.AsioDriver != null;
        // The level the resampler steers towards, and the start
        // threshold. Frames arrive in 40 ms bursts, so the level swings
        // that much on its own; 60 ms (a burst and a half of slack) is
        // the sensible floor. Under WaveOut the card adds ~100 ms of
        // its own buffering; under ASIO the held level IS the path, so
        // it sits lower by default. Chosen, not measured on the edge;
        // the underruns counter shows when a level is too low.
        _targetMs = targetMs ?? (asio ? 120 : 200);
        var format = new WaveFormat(sampleRate, 16, 2);
        _buffer = new BufferedWaveProvider(format)
        {
            // Samples arrive in 40 ms bursts (one DeckLink frame at a
            // time), so the fill level swings by that much on its own;
            // 600 ms leaves the resampler's target level room on both
            // sides.
            BufferDuration = TimeSpan.FromMilliseconds(600),
            DiscardOnBufferOverflow = true,
        };
        _resampler = new AdaptiveResampler(_buffer, this);
        if (asio)
        {
            // ChannelOffset must be set before Init: it picks which of
            // the driver's output channels this stereo stream lands on.
            var o = new AsioOut(device.AsioDriver) { ChannelOffset = device.AsioChannelOffset };
            o.Init(_resampler);
            _output = o;
        }
        else
        {
            // 20 ms reads (100 ms over five buffers) instead of the
            // default 150 ms (300 over two): a read finds the buffer
            // short far less often, and when it does the silence padded
            // in is 20 ms at most, not 150.
            var o = new WaveOutEvent { DeviceNumber = device.WaveOutIndex, DesiredLatency = 100, NumberOfBuffers = 5 };
            o.Init(_resampler);
            _output = o;
        }
    }

    // Playback only starts once this much is queued: started empty, the
    // fill level sat at zero and every burst boundary could run dry.
    // Also the level the resampler steers towards; set per route in the
    // constructor.
    private readonly int _targetMs;
    private readonly System.Threading.SynchronizationContext? _playSync;
    private bool _started;

    public double BufferedMs => _buffer.BufferedDuration.TotalMilliseconds;
    public double Ratio => _resampler.Ratio;

    // Soundcard reads that found the buffer short (silence padded in),
    // and bursts that arrived to a full buffer (discarded).
    public long Underruns { get; private set; }
    public long Overflows { get; private set; }

    public void Add(ReadOnlySpan<SamplePair> pairs)
    {
        if (pairs.Length == 0) return;
        Span<byte> bytes = stackalloc byte[pairs.Length * 4];
        for (int i = 0; i < pairs.Length; i++)
        {
            BitConverter.TryWriteBytes(bytes.Slice(i * 4, 2), pairs[i].Left);
            BitConverter.TryWriteBytes(bytes.Slice(i * 4 + 2, 2), pairs[i].Right);
        }
        if (_buffer.BufferedBytes + bytes.Length > _buffer.BufferLength) Overflows++;
        _buffer.AddSamples(bytes.ToArray(), 0, bytes.Length);
        if (!_started && BufferedMs >= _targetMs)
        {
            _started = true;
            // Add() runs on the capture callback thread; see playSync.
            if (_playSync != null) _playSync.Post(_ => _output.Play(), null);
            else _output.Play();
        }
    }

    public void Dispose()
    {
        // Stop() first, not just Dispose(): otherwise whatever is still
        // queued in _buffer keeps audibly playing out after the window
        // has already closed.
        _output.Stop();
        _output.Dispose();
    }

    // Linear interpolation from the buffered input to the output rate,
    // with Ratio input frames per output frame. Ratio 1 is a straight
    // copy; below 1 stretches the input out (the board delivering fewer
    // samples than the card plays), above 1 squeezes it in.
    private sealed class AdaptiveResampler : IWaveProvider
    {
        private readonly BufferedWaveProvider _inner;
        private readonly AudioPlayer _owner;
        // Input frames per output frame: nominally source rate over
        // OutputRate (1 for 48 kHz, 0.919 for 44.1, 2/3 for 32 kHz,
        // 1/3 for 16 kHz), steered +-2% around that by the fill level.
        private readonly double _nominal;
        public double Ratio { get; private set; }
        private double _baseRatio;   // the I term: the learned rate difference
        private double _phase;             // position between the two held input frames, 0..1
        private short _l0, _r0, _l1, _r1;  // the two most recent input frames
        private bool _primed;
        private DateTime _lastSteer = DateTime.MinValue;

        // Steering gains, per millisecond of level error, applied to a
        // smoothed level (the raw one swings 40 ms with every frame
        // burst). P alone holds the level with a standing error; I
        // removes that over a minute or so. Both small: a ratio change
        // is a pitch shift, and measured with P ten times this the
        // ratio sat at 0.992-0.995 on a 60 ms error, audibly flat.
        // Now a 60 ms error is 0.075%, ~1 cent.
        private readonly int _target;
        private readonly double _pGain, _iGain;
        private double _levelEma;

        public AdaptiveResampler(BufferedWaveProvider inner, AudioPlayer owner)
        {
            _inner = inner;
            _owner = owner;
            _nominal = (double)owner.SampleRate / OutputRate;
            Ratio = _nominal;
            _baseRatio = _nominal;
            _target = owner._targetMs;
            // The gains divide by the target; at the experimental 0 ms
            // setting they fall back to the 15 ms slope.
            double g = Math.Max(15, _target);
            _pGain = 1.0 / (g * 400.0);
            _iGain = 1.0 / (g * 2000.0);
            _levelEma = _target;
        }

        // What the card sees: always OutputRate, whatever rate the
        // buffered source runs at.
        public WaveFormat WaveFormat { get; } = new WaveFormat(OutputRate, 16, 2);

        private void Steer()
        {
            var now = DateTime.UtcNow;
            if ((now - _lastSteer).TotalMilliseconds < 250) return;
            _lastSteer = now;
            // ~1 s time constant at four updates a second.
            _levelEma += (_inner.BufferedDuration.TotalMilliseconds - _levelEma) * 0.25;
            double err = _levelEma - _target;
            double lo = _nominal * 0.98, hi = _nominal * 1.02;
            _baseRatio = Math.Clamp(_baseRatio + _nominal * _iGain * err * 0.25, lo, hi);
            Ratio = Math.Clamp(_baseRatio + _nominal * _pGain * err, lo, hi);
        }

        private bool NextInputFrame()
        {
            var tmp = new byte[4];
            if (_inner.BufferedBytes < 4) return false;
            _inner.Read(tmp, 0, 4);
            _l0 = _l1; _r0 = _r1;
            _l1 = BitConverter.ToInt16(tmp, 0);
            _r1 = BitConverter.ToInt16(tmp, 2);
            return true;
        }

        public int Read(byte[] buffer, int offset, int count)
        {
            Steer();
            int frames = count / 4;
            if (!_primed)
            {
                if (_inner.BufferedBytes < 8) { Array.Clear(buffer, offset, count); _owner.Underruns++; return count; }
                NextInputFrame();
                NextInputFrame();
                _primed = true;
            }
            bool short_ = false;
            for (int i = 0; i < frames; i++)
            {
                while (_phase >= 1.0)
                {
                    if (!NextInputFrame()) { short_ = true; break; }
                    _phase -= 1.0;
                }
                if (short_)
                {
                    Array.Clear(buffer, offset + i * 4, (frames - i) * 4);
                    break;
                }
                short l = (short)(_l0 + (_l1 - _l0) * _phase);
                short r = (short)(_r0 + (_r1 - _r0) * _phase);
                BitConverter.TryWriteBytes(buffer.AsSpan(offset + i * 4, 2), l);
                BitConverter.TryWriteBytes(buffer.AsSpan(offset + i * 4 + 2, 2), r);
                _phase += Ratio;
            }
            if (short_) _owner.Underruns++;
            return count;
        }
    }

    // Every playback route: the system default first, then every WinMM
    // WaveOut device, then every installed ASIO driver with one entry
    // per stereo output pair. Counting a driver's channels means
    // opening it briefly; a driver that will not open is listed once,
    // on its first pair.
    public static List<OutputDevice> ListDevices()
    {
        var list = new List<OutputDevice> { new("System default", -1, null, 0) };
        for (int i = 0; i < WaveOut.DeviceCount; i++)
        {
            list.Add(new(WaveOut.GetCapabilities(i).ProductName, i, null, 0));
        }
        foreach (string drv in AsioOut.GetDriverNames())
        {
            int channels = 2;
            try { using var probe = new AsioOut(drv); channels = probe.DriverOutputChannelCount; }
            catch { }
            for (int ch = 0; ch + 2 <= Math.Max(2, channels); ch += 2)
            {
                list.Add(new($"ASIO: {drv} [ch {ch + 1}&{ch + 2}]", -1, drv, ch));
            }
        }
        return list;
    }

    // A command-line device argument against `devices` (a ListDevices()
    // result): a plain index into that list, or a case-insensitive
    // substring of an entry's name ("ASIO: GIGAPORT [ch 3&4]" is
    // matched by "3&4"); null/empty keeps the system default (0).
    // Throws with the device list in the message if `arg` matches
    // nothing, rather than silently falling back to the wrong card.
    public static int ResolveDevice(string? arg, List<OutputDevice> devices)
    {
        if (string.IsNullOrEmpty(arg)) return 0;
        if (int.TryParse(arg, out int idx) && idx >= 0 && idx < devices.Count) return idx;
        for (int i = 0; i < devices.Count; i++)
        {
            if (devices[i].Name.IndexOf(arg, StringComparison.OrdinalIgnoreCase) >= 0) return i;
        }
        string listing = string.Join('\n', devices.ConvertAll(d => "  " + d.Name));
        throw new ArgumentException($"No soundcard matches \"{arg}\". Available:\n{listing}");
    }
}
