// Wraps a Blackmagic DeckLink card (Intensity Pro or similar) as a PAL,
// 8 bit YUV 4:2:2 frame source. IDeckLinkVideoBuffer.GetBytes() (not
// IDeckLinkVideoInputFrame.GetBytes(), removed from the current SDK,
// kept only on the _v14_2_1 legacy interfaces) is how a frame's pixels
// are reached now; StartAccess/EndAccess bracket that the way the SDK
// documents.
using System;
using System.Collections.Generic;
using DeckLinkAPI;

namespace PcmDecoder;

public sealed class FrameEventArgs : EventArgs
{
    public required byte[] Pixels { get; init; }
    public required int Width { get; init; }
    public required int Height { get; init; }
    public required int RowBytes { get; init; }
    // Vertical-blanking lines by line number (1..625), each RowBytes
    // long in the same 8 bit YUV layout as a picture row; only the
    // lines the card delivered. The Sony/EIAJ card puts its control
    // block and first data lines there (lines 10-23 and 322-336).
    public required System.Collections.Generic.Dictionary<int, byte[]> Vbi { get; init; }
}

// What DecoderSession needs from a frame source, whichever driver
// stack delivers it: the DeckLink SDK, or DirectShow for generic
// capture devices (DirectShowCapture.cs).
public interface ICaptureSource : IDisposable
{
    event EventHandler<FrameEventArgs>? FrameArrived;
    string Open(string hint);
    long FramesSeen { get; }
    long DroppedFrames { get; }
    long NoInputFrames { get; }
    double HandlerMaxMs { get; }
    Dictionary<int, long> VbiRefused { get; }
}

public sealed class DeckLinkCapture : IDeckLinkInputCallback, ICaptureSource
{
    private IDeckLinkInput? _input;
    public event EventHandler<FrameEventArgs>? FrameArrived;

    // Frames the card reports but this callback never saw: consecutive
    // stream times should be exactly one frame duration apart, and a
    // larger step means the card dropped frames in between -- which is
    // what happens when the callback takes longer than a frame (40 ms).
    // HandlerMaxMs/HandlerOverruns measure that side directly.
    public long DroppedFrames { get; private set; }
    public long FramesSeen { get; private set; }
    public long NoInputFrames { get; private set; }
    public double HandlerMaxMs { get; private set; }
    public long HandlerOverruns { get; private set; }
    private long _lastStreamTime = -1;
    private const long TimeScale = 25000;  // 1000 ticks a PAL frame

    // Which blanking lines to ask for: what the Sony/EIAJ card uses,
    // plus one either side to learn what the card can actually give.
    private static readonly int[] VbiWanted = BuildVbiWanted();
    private static int[] BuildVbiWanted()
    {
        var l = new System.Collections.Generic.List<int>();
        for (int i = 7; i <= 23; i++) l.Add(i);
        for (int i = 320; i <= 336; i++) l.Add(i);
        return l.ToArray();
    }
    // Per line number: how often the card refused it. Read by the status
    // line so a missing line is a reported fact, not a silent gap.
    public System.Collections.Generic.Dictionary<int, long> VbiRefused { get; } = new();
    // The last error the ancillary interface gave, so a refusal is a
    // quoted HRESULT rather than a guess.
    public string VbiError { get; private set; } = "";

    // The model names of every DeckLink device present, in iterator
    // order; what the window offers as capture sources.
    public static System.Collections.Generic.List<string> ListDevices()
    {
        var names = new System.Collections.Generic.List<string>();
        var iterator = new CDeckLinkIteratorClass();
        while (true)
        {
            IDeckLink device;
            try { iterator.Next(out device); }
            catch (Exception) { break; }
            if (device == null) break;
            device.GetModelName(out string name);
            names.Add(name);
        }
        return names;
    }

    // Picks the first device whose model name contains `modelHint`
    // (case insensitive), or the first device at all if `modelHint` is
    // empty. IDeckLinkIterator.Next() throws (COM S_FALSE maps to a
    // .NET exception on this void-returning signature) once devices run
    // out; that is the normal way to end the loop, not an error.
    public string Open(string modelHint)
    {
        var iterator = new CDeckLinkIteratorClass();
        IDeckLink? chosen = null;
        string chosenName = "";
        while (true)
        {
            IDeckLink device;
            try
            {
                iterator.Next(out device);
            }
            catch (Exception)
            {
                break;
            }
            if (device == null) break;

            device.GetModelName(out string name);
            if (chosen == null && (modelHint.Length == 0 ||
                name.IndexOf(modelHint, StringComparison.OrdinalIgnoreCase) >= 0))
            {
                chosen = device;
                chosenName = name;
            }
        }
        if (chosen == null)
        {
            throw new InvalidOperationException(
                modelHint.Length == 0
                    ? "No DeckLink device found."
                    : $"No DeckLink device found matching \"{modelHint}\".");
        }

        // The Intensity Pro has more than one input connector (HDMI,
        // component, composite/S-Video); EnableVideoInput() does not
        // pick one by itself; without this every captured frame is
        // whatever the card's default connector happens to show, not
        // necessarily composite -- measured on the board: 100% of rows
        // failed the run-in check with this missing, not just some.
        var config = (IDeckLinkConfiguration)chosen;
        config.SetInt(_BMDDeckLinkConfigurationID.bmdDeckLinkConfigVideoInputConnection,
            (long)_BMDVideoConnection.bmdVideoConnectionComposite);

        _input = (IDeckLinkInput)chosen;
        _input.SetCallback(this);
        // PAL, 8 bit 4:2:2 YUV: same byte order as this firmware's own
        // Cb,Y0,Cr,Y1 packing (gfx.h), U=Cb and V=Cr, so a captured row
        // can be read directly without a colour space conversion step.
        _input.EnableVideoInput(_BMDDisplayMode.bmdModePAL,
            _BMDPixelFormat.bmdFormat8BitYUV,
            _BMDVideoInputFlags.bmdVideoInputFlagDefault);
        _input.StartStreams();
        return chosenName;
    }

    public void VideoInputFrameArrived(IDeckLinkVideoInputFrame videoFrame, IDeckLinkAudioInputPacket audioPacket)
    {
        if (videoFrame == null) return;
        var handler = FrameArrived;
        if (handler == null) return;   // nobody listening, skip the copy

        FramesSeen++;
        if ((videoFrame.GetFlags() & _BMDFrameFlags.bmdFrameHasNoInputSource) != 0) NoInputFrames++;
        videoFrame.GetStreamTime(out long streamTime, out long frameDuration, TimeScale);
        if (_lastStreamTime >= 0 && frameDuration > 0)
        {
            long step = (streamTime - _lastStreamTime) / frameDuration;
            if (step > 1) DroppedFrames += step - 1;
        }
        _lastStreamTime = streamTime;
        var sw = System.Diagnostics.Stopwatch.StartNew();

        int width = videoFrame.GetWidth();
        int height = videoFrame.GetHeight();
        int rowBytes = videoFrame.GetRowBytes();

        // Blanking lines through the ancillary interface, the SDK's way
        // to reach analogue VBI in 8 bit YUV: one buffer per line, same
        // layout as a picture row. A line the card does not provide
        // throws; that is counted, not fatal.
        var vbi = new System.Collections.Generic.Dictionary<int, byte[]>();
        try
        {
            IDeckLinkVideoFrameAncillary anc;
            try
            {
                videoFrame.GetAncillaryData(out anc);
            }
            catch (InvalidCastException)
            {
                // The current interface's call was refused with E_NOINTERFACE
                // on an Intensity Pro (Desktop Video 12.x): the legacy
                // v14.2.1 frame interface still carries the same call.
                ((IDeckLinkVideoInputFrame_v14_2_1)videoFrame).GetAncillaryData(out anc);
            }
            if (anc != null)
            {
                foreach (int line in VbiWanted)
                {
                    try
                    {
                        anc.GetBufferForVerticalBlankingLine((uint)line, out IntPtr lp);
                        if (lp == IntPtr.Zero) throw new InvalidOperationException();
                        var lb = new byte[rowBytes];
                        System.Runtime.InteropServices.Marshal.Copy(lp, lb, 0, rowBytes);
                        vbi[line] = lb;
                    }
                    catch (Exception ex)
                    {
                        VbiRefused[line] = VbiRefused.GetValueOrDefault(line) + 1;
                        VbiError = $"line {line}: {ex.GetType().Name} 0x{ex.HResult:X8}";
                    }
                }
            }
        }
        catch (Exception ex)
        {
            VbiRefused[0] = VbiRefused.GetValueOrDefault(0) + 1;   // no ancillary data at all
            VbiError = $"{ex.GetType().Name} 0x{ex.HResult:X8} {ex.Message}";
        }

        var buffer = (IDeckLinkVideoBuffer)videoFrame;
        buffer.StartAccess(_BMDBufferAccessFlags.bmdBufferAccessRead);
        try
        {
            buffer.GetBytes(out IntPtr ptr);
            var pixels = new byte[rowBytes * height];
            System.Runtime.InteropServices.Marshal.Copy(ptr, pixels, 0, pixels.Length);
            handler(this, new FrameEventArgs { Pixels = pixels, Width = width, Height = height, RowBytes = rowBytes, Vbi = vbi });
        }
        finally
        {
            buffer.EndAccess(_BMDBufferAccessFlags.bmdBufferAccessRead);
        }
        double ms = sw.Elapsed.TotalMilliseconds;
        if (ms > HandlerMaxMs) HandlerMaxMs = ms;
        if (ms > 40.0) HandlerOverruns++;
    }

    // Required by the interface; format changes (someone unplugging the
    // cable, a different standard arriving) are not handled, this tool
    // expects a fixed PAL composite feed from the PAL-sign board.
    public void VideoInputFormatChanged(_BMDVideoInputFormatChangedEvents notificationEvents,
        IDeckLinkDisplayMode newDisplayMode, _BMDDetectedVideoInputFormatFlags detectedSignalFlags)
    {
    }

    public void Dispose()
    {
        try
        {
            _input?.StopStreams();
            _input?.DisableVideoInput();
        }
        catch (Exception)
        {
            // best effort; the process is exiting anyway
        }
    }
}
