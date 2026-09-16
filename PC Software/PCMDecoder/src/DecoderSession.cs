// One running decode: a DeckLink capture feeding the Ham PCM row
// decoder and the Sony/EIAJ decoder, whichever locks, and the result
// to a soundcard. Everything the window shows comes out of Snapshot(),
// taken under a lock on the UI's timer; the capture callback thread
// never touches a control.
using System;
using System.Collections.Generic;

namespace PcmDecoder;

public sealed class Snapshot
{
    public bool Sony;
    public int Family;   // Ham PCM: 0 HQ, 1 LQ, 2 Voice, 3 Voice narrow
    public bool Sony16;
    public bool SonyInPicture;
    public bool SonyControlSeen;
    public double Seconds;
    public long TotalPairs;
    public string Text = "";
    // Ham PCM
    public long RowsOk, RowsRunIn, RowsSync, RowsCorrected, RowsErasure, RowsCoarse, RowsConcealed, RowsTailRepaired;
    public int LockPixel;
    public double Period;
    public int SyncShift;
    // Sony
    public long LinesOk, LinesNoSync, LinesCrc, LinesMissing, BlocksClean, BlocksRepaired, BlocksRepaired2, BlocksHeld;
    public double SonyStart;
    // Audio
    public int SampleRate;
    public double BufferedMs, Ratio;
    public long Underruns, Overflows;
    public int PeakLeft, PeakRight;   // 0..32767 since the last snapshot
    // The measured frequency of the decoded left channel (the 1 kHz
    // loopback test); 0 when nothing periodic is coming in. ToneDiag
    // carries the window's period statistics for the log.
    public double ToneHz;
    public string ToneDiag = "";
    // Capture
    public long Frames, Dropped, NoInput;
    public double HandlerMaxMs;
    public string VbiRefused = "";
    // Which capture rows lose their enhancement most: "row:count" for
    // the five worst, to tell a systematic row from noise.
    public string CoarseRows = "";
}

public sealed class DecoderSession : IDisposable
{
    private readonly RowDecoder _ham = new();
    private readonly SonyDecoder _sony = new();
    private readonly ICaptureSource _capture;
    // One soundcard stream at a time, at the rate of the format being
    // decoded: three streams side by side (48000, 44100, 32000, the
    // idle ones padding silence) left the card resampling all three and
    // the rate switch unreliable on a GIGAPORT eX.
    private readonly OutputDevice _soundcard;
    private readonly int? _bufferMs;   // held audio level; null = per route
    private readonly System.Threading.SynchronizationContext? _sync;
    private int _audioFailedRate = -1;

    // One line for the window's log (soundcard trouble); may fire on
    // the capture callback thread.
    public event Action<string>? LogLine;
    private AudioPlayer? _audio;
    private int _family;
    private readonly object _lock = new();
    private readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    private long _totalPairs;
    private bool _sonyMode;
    private long _framesInMode;
    private int _peakL, _peakR;
    private readonly FrequencyMeter _freq = new();
    // Per frame row 0..575: 0 no data, 1 ok, 2 degraded, 3 lost.
    private readonly byte[] _rowHealth = new byte[576];
    private readonly long[] _coarseByRow = new long[576];
    private readonly long[] _lockSumByRow = new long[576], _lockCountByRow = new long[576];
    private readonly long[] _weakByte = new long[8];
    private long _weakMarginSum, _weakCount;

    public string DeviceName { get; }
    // A frame for the eye, every fifth one: the callback hands over the
    // raw UYVY buffer, the window converts it.
    public event Action<FrameEventArgs>? PreviewFrame;

    public DecoderSession(string deckLinkHint, OutputDevice soundcard, int? bufferMs = null)
    {
        _soundcard = soundcard;
        _bufferMs = bufferMs;
        // The UI thread's context, captured here because the session is
        // built on it; Player() opens the soundcard through it.
        _sync = System.Threading.SynchronizationContext.Current;
        // The hint carries its own routing: a "DirectShow: " name goes
        // to the generic capture path, anything else to the DeckLink.
        _capture = deckLinkHint.StartsWith(DirectShowCapture.Prefix, StringComparison.Ordinal)
            ? new DirectShowCapture()
            : new DeckLinkCapture();
        _capture.FrameArrived += OnFrame;
        DeviceName = _capture.Open(deckLinkHint);
    }

    private void OnFrame(object? sender, FrameEventArgs e)
    {
        // The two formats never share a frame, and a full search by the
        // wrong decoder costs more than a frame (140 ms measured with
        // both run on every frame). So the format found is kept, and the
        // other decoder only gets a look every 250 frames.
        _framesInMode++;
        bool probeOwn = !_sonyMode || _framesInMode % 250 == 0;
        long ownRows = 0;
        var own = new List<SamplePair>(2000);
        if (probeOwn)
        {
            long okBefore = _ham.RowsOk;
            // Field order, not frame order: the board renders the even
            // rows (field 1) first and the odd rows (field 2) 20 ms later,
            // and the samples in each row are whatever the ADC delivered
            // at that moment. Reading the interleaved frame top to bottom
            // instead alternates between two moments 20 ms apart and
            // plays a pure tone an exact octave low.
            for (int field = 0; field < 2; field++)
            for (int y = field; y < e.Height; y += 2)
            {
                own.AddRange(_ham.DecodeRow(e.Pixels, y * e.RowBytes, e.RowBytes));
                if (y < _rowHealth.Length)
                {
                    if (_ham.LastStatus == RowStatus.Coarse)
                    {
                        _coarseByRow[y]++;
                        if (_ham.LastWeakByte >= 0 && _ham.LastWeakByte < 8) _weakByte[_ham.LastWeakByte]++;
                        _weakMarginSum += _ham.LastWeakMargin; _weakCount++;
                    }
                    if (_ham.LastStatus == RowStatus.Ok || _ham.LastStatus == RowStatus.Coarse)
                    {
                        _lockSumByRow[y] += _ham.LastLockPixel;
                        _lockCountByRow[y]++;
                    }
                    _rowHealth[y] = _ham.LastStatus switch
                    {
                        RowStatus.Ok => 1, RowStatus.Coarse => 2,
                        RowStatus.Concealed or RowStatus.NoSync => 3, _ => 0,
                    };
                }
            }
            ownRows = _ham.RowsOk - okBefore;
        }

        if (ownRows >= 100)
        {
            int family = _ham.LastFamily;
            int rate = family == 0 ? 48000 : family == 3 ? 16000 : 32000;
            lock (_lock)
            {
                if (_sonyMode) _framesInMode = 0;
                _sonyMode = false;
                _family = family;
                Peak(own);
                _freq.Feed(own, rate);
                _totalPairs += own.Count;
            }
            Player(rate)?.Add(own.ToArray());
        }
        else
        {
            // Line numbers 1..625 onto this frame: the card's row 0 is
            // the board's line 24 (measured with the Ham PCM card: its
            // rows 0/1 never arrive and the card's last two rows show
            // lines that do not exist), row 1 is line 337; the blanking
            // lines would come from the ancillary buffers, which this
            // card refuses (E_NOINTERFACE), so the decoder's P and Q
            // erasure correction fills them in.
            SonyDecoder.LineSource src = (int line, out byte[] px, out int off, out int rb) =>
            {
                rb = e.RowBytes;
                if (line >= 24 && line <= 24 + 287)
                {
                    px = e.Pixels; off = (2 * (line - 24)) * e.RowBytes; return true;
                }
                if (line >= 337 && line <= 337 + 287)
                {
                    px = e.Pixels; off = (2 * (line - 337) + 1) * e.RowBytes; return true;
                }
                if (e.Vbi.TryGetValue(line, out var lb))
                {
                    px = lb; off = 0; return true;
                }
                px = Array.Empty<byte>(); off = 0; return false;
            };
            long linesBefore = _sony.LinesOk;
            var pairs = _sony.DecodeFrame(src);
            if (_sony.LinesOk - linesBefore >= 100)
            {
                for (int y = 0; y < 576; y++)
                {
                    int line = (y & 1) == 0 ? 24 + y / 2 : 337 + y / 2;
                    _rowHealth[y] = _sony.LineStatus[line];
                }
                lock (_lock)
                {
                    if (!_sonyMode) _framesInMode = 0;
                    _sonyMode = true;
                    Peak(pairs);
                    _freq.Feed(pairs, SonyDecoder.SampleRate);
                    _totalPairs += pairs.Count;
                }
                Player(SonyDecoder.SampleRate)?.Add(pairs.ToArray());
            }
        }
        // Every frame; the window converts it on a thread of its own and
        // drops what it cannot keep up with.
        PreviewFrame?.Invoke(e);
    }

    // The player for this source rate, opened on demand; a change of
    // rate closes the old one first, so its buffer does not play out
    // under the new one. The card itself always runs at 48 kHz
    // (AudioPlayer.OutputRate); rate here is what the decoder delivers.
    //
    // Opened on the UI thread via _sync: an ASIO driver wants to be
    // created and started from an STA thread, and this runs on the
    // DeckLink callback thread. Opened there, the ASIO route gave no
    // sound and froze the capture (30 August 2026). A route that fails
    // to open is reported once through LogLine and left silent, so
    // decoding carries on.
    private AudioPlayer? Player(int rate)
    {
        var a = _audio;
        if (a != null && a.SampleRate == rate) return a;
        if (rate == _audioFailedRate) return null;
        AudioPlayer? made = null;
        Exception? fail = null;
        void Make()
        {
            try { a?.Dispose(); made = new AudioPlayer(_soundcard, rate, _sync, _bufferMs); }
            catch (Exception ex) { fail = ex; }
        }
        if (_sync != null) _sync.Send(_ => Make(), null); else Make();
        if (made == null)
        {
            _audioFailedRate = rate;
            lock (_lock) _audio = null;
            LogLine?.Invoke($"Soundcard failed at {rate} Hz: {fail?.Message}");
            return null;
        }
        lock (_lock) _audio = made;
        return made;
    }

    private void Peak(List<SamplePair> pairs)
    {
        foreach (var p in pairs)
        {
            int l = Math.Abs((int)p.Left), r = Math.Abs((int)p.Right);
            if (l > _peakL) _peakL = l;
            if (r > _peakR) _peakR = r;
        }
    }

    // The peaks since the last call, for a meter that moves faster than
    // the counters.
    public (int Left, int Right) TakePeaks()
    {
        lock (_lock)
        {
            var r = (_peakL, _peakR);
            _peakL = _peakR = 0;
            return r;
        }
    }

    public void CopyRowHealth(byte[] dst)
    {
        lock (_lock) Array.Copy(_rowHealth, dst, Math.Min(dst.Length, _rowHealth.Length));
    }

    public Snapshot Snapshot()
    {
        lock (_lock)
        {
            var audio = _audio;
            string refused = "";
            foreach (var kv in _capture.VbiRefused) refused += (refused.Length == 0 ? "" : ",") + kv.Key;
            var worst = new List<int>();
            for (int y = 0; y < 576; y++) if (_coarseByRow[y] > 0) worst.Add(y);
            worst.Sort((a, b) => _coarseByRow[b].CompareTo(_coarseByRow[a]));
            string coarseRows = "";
            // Each with the row's mean lock pixel, against the mean over all
            // rows: a later start means a row whose tail leaves the window.
            long lsum = 0, lcnt = 0;
            for (int y = 0; y < 576; y++) { lsum += _lockSumByRow[y]; lcnt += _lockCountByRow[y]; }
            for (int i = 0; i < Math.Min(5, worst.Count); i++)
            {
                int y = worst[i];
                double lk = _lockCountByRow[y] > 0 ? (double)_lockSumByRow[y] / _lockCountByRow[y] : 0;
                coarseRows += (i > 0 ? " " : "") + $"{y}:{_coarseByRow[y]}@{lk:0.0}";
            }
            if (worst.Count > 5) coarseRows += $" (+{worst.Count - 5} rows)";
            if (lcnt > 0) coarseRows += $" mean lock {(double)lsum / lcnt:0.00}";
            if (_weakCount > 0)
            {
                coarseRows += " weakest tail byte:";
                for (int i = 0; i < 7; i++) coarseRows += $" {i}={_weakByte[i]}";
                coarseRows += $" margin {(double)_weakMarginSum / _weakCount:0}";
            }
            var s = new Snapshot
            {
                Sony = _sonyMode, Family = _family, Sony16 = _sony.Mode16, SonyInPicture = _sony.InPicture,
                SonyControlSeen = _sony.ControlSeen,
                Seconds = _clock.Elapsed.TotalSeconds, TotalPairs = _totalPairs, Text = _ham.Text,
                RowsOk = _ham.RowsOk, RowsRunIn = _ham.RowsRunInMismatch, RowsSync = _ham.RowsSyncMismatch,
                RowsCorrected = _ham.RowsCorrected, RowsErasure = _ham.RowsErasureFixed,
                RowsCoarse = _ham.RowsCoarse, RowsConcealed = _ham.RowsConcealed, RowsTailRepaired = _ham.RowsTailRepaired,
                LockPixel = _ham.LastLockPixel, Period = _ham.LastLockPeriod, SyncShift = _ham.LastSyncShift,
                LinesOk = _sony.LinesOk, LinesNoSync = _sony.LinesNoSync, LinesCrc = _sony.LinesCrcBad,
                LinesMissing = _sony.LinesMissing, BlocksClean = _sony.BlocksClean,
                BlocksRepaired = _sony.BlocksRepaired, BlocksRepaired2 = _sony.BlocksRepaired2,
                BlocksHeld = _sony.BlocksHeld, SonyStart = _sony.LastStart,
                SampleRate = audio?.SampleRate ?? (_sonyMode ? SonyDecoder.SampleRate : _family == 0 ? 48000 : _family == 3 ? 16000 : 32000),
                BufferedMs = audio?.BufferedMs ?? 0, Ratio = audio?.Ratio ?? 1.0,
                Underruns = audio?.Underruns ?? 0, Overflows = audio?.Overflows ?? 0,
                PeakLeft = 0, PeakRight = 0, ToneHz = _freq.Hz, ToneDiag = _freq.Diag,
                Frames = _capture.FramesSeen, Dropped = _capture.DroppedFrames, NoInput = _capture.NoInputFrames,
                HandlerMaxMs = _capture.HandlerMaxMs, VbiRefused = refused, CoarseRows = coarseRows,
            };
            return s;
        }
    }

    public void Dispose()
    {
        // Audio first: stopping the DeckLink takes a moment, and the
        // buffer plus output latency would otherwise keep playing.
        _capture.FrameArrived -= OnFrame;
        _audio?.Dispose();
        _audio = null;
        _capture.Dispose();
    }
}
