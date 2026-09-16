// Generic capture devices (a Pinnacle Dazzle, a cheap USB grabber) as
// a frame source, through DirectShow: capture filter -> SampleGrabber
// -> null renderer, the crossbar routed to the composite input and the
// decoder asked for PAL. The grabber negotiates 8 bit 4:2:2 YUV; YUY2
// (Y0 U Y1 V) is byte-swapped to the UYVY order (U Y0 V Y1) the row
// decoders expect, UYVY passes straight through. No VBI: DirectShow
// only delivers the 576 picture rows, so the Sony card's control block
// depends entirely on the P+Q erasure repair here, and the Sony line
// mapping in DecoderSession is measured against the DeckLink's capture
// window, which a Dazzle need not share.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using DirectShowLib;

namespace PcmDecoder;

public sealed class DirectShowCapture : ICaptureSource, ISampleGrabberCB
{
    public const string Prefix = "DirectShow: ";

    public event EventHandler<FrameEventArgs>? FrameArrived;
    public long FramesSeen { get; private set; }
    public long DroppedFrames { get; private set; }
    public long NoInputFrames { get; private set; }
    public double HandlerMaxMs { get; private set; }
    public Dictionary<int, long> VbiRefused { get; } = new();

    private IFilterGraph2? _graph;
    private IMediaControl? _control;
    private ISampleGrabber? _grabber;
    private int _width, _height;
    private bool _uyvy;
    private double _lastTime = -1;
    private string _connectedInfo = "";
    // DirectShow delivers no blanking lines; one shared empty map.
    private readonly Dictionary<int, byte[]> _noVbi = new();

    public static List<string> ListDevices()
    {
        var names = new List<string>();
        foreach (var d in DsDevice.GetDevicesOfCat(FilterCategory.VideoInputDevice))
            names.Add(Prefix + d.Name);
        return names;
    }

    public string Open(string hint)
    {
        string wanted = hint.StartsWith(Prefix, StringComparison.Ordinal) ? hint.Substring(Prefix.Length) : hint;
        DsDevice? dev = null;
        foreach (var d in DsDevice.GetDevicesOfCat(FilterCategory.VideoInputDevice))
            if (dev == null && d.Name == wanted) dev = d;
        if (dev == null) throw new InvalidOperationException($"No DirectShow device named \"{wanted}\".");

        _graph = (IFilterGraph2)new FilterGraph();
        var builder = (ICaptureGraphBuilder2)new CaptureGraphBuilder2();
        DsError.ThrowExceptionForHR(builder.SetFiltergraph(_graph));
        DsError.ThrowExceptionForHR(_graph.AddSourceFilterForMoniker(dev.Mon, null, dev.Name, out IBaseFilter source));

        // The composite input on the crossbar, and PAL on the decoder:
        // both best effort, a device without the interface just keeps
        // its default.
        try { RouteComposite(builder, source); } catch (Exception) { }
        try
        {
            if (builder.FindInterface(null, null, source, typeof(IAMAnalogVideoDecoder).GUID, out object o) == 0)
                ((IAMAnalogVideoDecoder)o).put_TVFormat(AnalogVideoStandard.PAL_B);
        }
        catch (Exception) { }

        // 720x576 in a 4:2:2 format, asked for before the graph
        // connects; a device that cannot do it keeps its default and
        // the size check below decides.
        try { PickFormat(builder, source); } catch (Exception) { }

        _grabber = (ISampleGrabber)new SampleGrabber();
        var mt = new AMMediaType { majorType = MediaType.Video, subType = MediaSubType.YUY2 };
        _grabber.SetMediaType(mt);
        DsUtils.FreeAMMediaType(mt);
        _grabber.SetBufferSamples(false);
        _grabber.SetOneShot(false);
        _grabber.SetCallback(this, 1);   // 1 = BufferCB
        var grabberFilter = (IBaseFilter)_grabber;
        DsError.ThrowExceptionForHR(_graph.AddFilter(grabberFilter, "grabber"));
        var sink = (IBaseFilter)new NullRenderer();
        DsError.ThrowExceptionForHR(_graph.AddFilter(sink, "sink"));

        int hr = builder.RenderStream(PinCategory.Capture, MediaType.Video, source, grabberFilter, sink);
        if (hr != 0)
        {
            // Not every device connects in YUY2; take whatever it has
            // and let the subtype check below sort the byte order out.
            _grabber.SetMediaType(new AMMediaType { majorType = MediaType.Video });
            DsError.ThrowExceptionForHR(builder.RenderStream(PinCategory.Capture, MediaType.Video, source, grabberFilter, sink));
        }

        var connected = new AMMediaType();
        DsError.ThrowExceptionForHR(_grabber.GetConnectedMediaType(connected));
        try
        {
            if (connected.formatType != FormatType.VideoInfo || connected.formatPtr == IntPtr.Zero)
                throw new InvalidOperationException("The device did not connect with a plain video format.");
            var vih = Marshal.PtrToStructure<VideoInfoHeader>(connected.formatPtr)
                      ?? throw new InvalidOperationException("The connected format carries no video header.");
            _width = vih.BmiHeader.Width;
            _height = Math.Abs(vih.BmiHeader.Height);
            if (connected.subType == MediaSubType.UYVY) _uyvy = true;
            else if (connected.subType == MediaSubType.YUY2) _uyvy = false;
            else throw new InvalidOperationException("The device connected in a format that is not YUY2 or UYVY.");
            // What the driver actually promised, shown in the Started
            // line: a rate under 25 here means the source itself is the
            // bottleneck, not this graph.
            double fps = vih.AvgTimePerFrame > 0 ? 1e7 / vih.AvgTimePerFrame : 0;
            _connectedInfo = $" ({(_uyvy ? "UYVY" : "YUY2")} {fps:0.0} fps)";
        }
        finally
        {
            DsUtils.FreeAMMediaType(connected);
        }
        if (_width != 720 || _height != 576)
            throw new InvalidOperationException(
                $"The device delivers {_width}x{_height}; the decoder needs the full 720x576 PAL frame.");

        // No reference clock: with one, the null renderer schedules
        // every sample on its timestamp, and stamps that run a little
        // late block the capture filter's small buffer pool; measured
        // on a Dazzle as only 9-16 of 25 frames a second reaching the
        // callback, the rest dropped. Without a clock samples flow
        // through the moment they arrive.
        DsError.ThrowExceptionForHR(((IMediaFilter)_graph).SetSyncSource(null));

        _control = (IMediaControl)_graph;
        DsError.ThrowExceptionForHR(_control.Run());
        return Prefix + wanted + _connectedInfo;
    }

    // Every crossbar input pin of type composite routed to whatever
    // output accepts it; the usual single video decoder output takes
    // the first one.
    private static void RouteComposite(ICaptureGraphBuilder2 builder, IBaseFilter source)
    {
        if (builder.FindInterface(FindDirection.UpstreamOnly, null, source, typeof(IAMCrossbar).GUID, out object o) != 0)
            return;
        var bar = (IAMCrossbar)o;
        bar.get_PinCounts(out int outputs, out int inputs);
        for (int i = 0; i < inputs; i++)
        {
            bar.get_CrossbarPinInfo(true, i, out _, out PhysicalConnectorType type);
            if (type != PhysicalConnectorType.Video_Composite) continue;
            for (int j = 0; j < outputs; j++)
            {
                if (bar.CanRoute(j, i) == 0) { bar.Route(j, i); return; }
            }
        }
    }

    private static void PickFormat(ICaptureGraphBuilder2 builder, IBaseFilter source)
    {
        if (builder.FindInterface(PinCategory.Capture, MediaType.Video, source, typeof(IAMStreamConfig).GUID, out object o) != 0)
            return;
        var cfg = (IAMStreamConfig)o;
        cfg.GetNumberOfCapabilities(out int count, out int size);
        IntPtr scc = Marshal.AllocCoTaskMem(size);
        try
        {
            for (int i = 0; i < count; i++)
            {
                if (cfg.GetStreamCaps(i, out AMMediaType mt, scc) != 0) continue;
                bool take = false;
                if (mt.majorType == MediaType.Video && mt.formatType == FormatType.VideoInfo &&
                    (mt.subType == MediaSubType.YUY2 || mt.subType == MediaSubType.UYVY) &&
                    mt.formatPtr != IntPtr.Zero)
                {
                    var vih = Marshal.PtrToStructure<VideoInfoHeader>(mt.formatPtr);
                    take = vih != null && vih.BmiHeader.Width == 720 && Math.Abs(vih.BmiHeader.Height) == 576;
                    if (take)
                    {
                        // The full 25 fps asked for explicitly: a driver
                        // default of a lower rate in the capability would
                        // otherwise stand, and the decoder needs every
                        // frame for continuous audio.
                        vih!.AvgTimePerFrame = 400000;   // 100 ns units
                        Marshal.StructureToPtr(vih, mt.formatPtr, false);
                    }
                }
                if (take) cfg.SetFormat(mt);
                DsUtils.FreeAMMediaType(mt);
                if (take) return;
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(scc);
        }
    }

    // Streaming thread, one call per frame.
    public int BufferCB(double sampleTime, IntPtr pBuffer, int bufferLen)
    {
        var handler = FrameArrived;
        if (handler == null) return 0;
        FramesSeen++;
        if (_lastTime >= 0)
        {
            int step = (int)Math.Round((sampleTime - _lastTime) * 25.0);
            if (step > 1) DroppedFrames += step - 1;
        }
        _lastTime = sampleTime;
        var sw = System.Diagnostics.Stopwatch.StartNew();

        int rowBytes = _width * 2;
        int need = rowBytes * _height;
        if (bufferLen < need) return 0;
        var pixels = new byte[need];
        Marshal.Copy(pBuffer, pixels, 0, need);
        if (!_uyvy)
        {
            // YUY2 to UYVY: the two orders differ only by a swap within
            // every byte pair.
            for (int i = 0; i + 1 < need; i += 2)
            {
                byte t = pixels[i];
                pixels[i] = pixels[i + 1];
                pixels[i + 1] = t;
            }
        }
        handler(this, new FrameEventArgs
            { Pixels = pixels, Width = _width, Height = _height, RowBytes = rowBytes, Vbi = _noVbi });
        double ms = sw.Elapsed.TotalMilliseconds;
        if (ms > HandlerMaxMs) HandlerMaxMs = ms;
        return 0;
    }

    public int SampleCB(double sampleTime, IMediaSample pSample) => 0;

    public void Dispose()
    {
        try
        {
            _control?.Stop();
            _grabber?.SetCallback(null, 1);
        }
        catch (Exception)
        {
            // best effort; the process is exiting anyway
        }
    }
}
