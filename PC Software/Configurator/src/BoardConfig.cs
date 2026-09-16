// Reading every setting off the board and putting one back.
//
// Protocol: key A dumps everything, key K reads one "name=value" line
// back in; a card scoped name carries its number, e.g. card.7.tickery=530.
// Counterpart: Firmware/Firmware.ino, printSettings()/setSettingFromCommand().
//
// The board owns the names of the aspect ratios and the two filters; this
// file keeps no copy of them.
//
// Nothing reaches flash until Save() (key * on the board). A plain Set()
// is instant and free; a Save blacks the picture for about 56 ms.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;

namespace PalSign;

/// <summary>What the board keeps for one test card.</summary>
public class CardInfo
{
    public int Number;          // 1 based, as the ? list shows it
    public string Name = "";
    public bool Photo;          // an uploadable slot: no transparent ticker
    public bool ShowId;         // the first text line, per card
    public bool ShowSub;        // the second text line, per card
    public int Ticker;          // 0 off, 1 a bar of its own, 2 over the card
    public int TickerY;
    public int TickerFont;
    // Negative walks left, positive right; the magnitude is the rung on
    // the board's speed ladder. Zero is never sent.
    public int TickerSpeed = -4;
    // Which of the five fixed sizes the big ticker faces (Inter, Doto)
    // draw at: 0 extra small .. 4 extra large, 2 medium. Meaningless on
    // the raster faces. Wire name kept as "tickerscale" for protocol
    // continuity; see the board's own TICKER_SIZE_MIN/MAX in settings.h.
    public int TickerSize = 2;
    // Index into the board's font list, for this card's own lettering
    // (identification, clock, inserts). The ticker's own face is TickerFont.
    public int TextFont;
    // True where the card's lettering has no font choice at all, e.g.
    // Mixed bars, fixed to Inter.
    public bool FixedFont;
    public int Aspect;
    public int Insert;

    // 0 none, 1 the PM5644 16:9 chroma bars, 2 AP1/AP2 on BBC Card G,
    // 3 the ATV Contest fields, 4 BBC Test Card F/W's own photo switch,
    // 5 the Sony PCM card's 16 bit and pre-emphasis switches.
    public int Own;

    // Own == 4 only: show the uploaded replacement instead of the
    // built-in photo. See PortraitDialog.cs.
    public bool CustomPhoto;

    // The card's own switches as a bit field, meaning per Own: for the
    // Sony PCM card (Own == 5) bit 0 is 16 bit, bit 1 pre-emphasis.
    public int Option;

    // 0 not known yet, 1 no, 2 yes. Filled in as the board shows each card,
    // so known for all of them after a Connect.
    public int HasId;

    // 1 no, 2 yes.
    public int HasBox;

    // A white stripe sweeping through a black box, corner to corner, one
    // second each way. Only a card with such a box has the switch at all.
    public bool Moving;

    // 1 no, 2 yes.
    public int HasLine;

    // Off: skipped by p, P, the knob and the slideshow fallback on the
    // board. Typing the card's number on the serial port still works.
    public bool Enabled = true;

    // 1 no, 2 yes. Three of the identification cards only have the first line.
    public int HasSub;

    // RRGGBB as the board sends it; white is the factory setting.
    public string TickerColor = "FFFFFF";
    public string IdColor = "FFFFFF";
    public string SubColor = "FFFFFF";
    public string InsColor = "FFFFFF";

    // What a fetched picture of this card depends on, so an unchanged
    // picture can be left alone. Not a checksum of the image; see
    // cfgCardCrc() in the firmware.
    public uint Crc;

    public override string ToString() => $"{Number,2}  {Name}";
}

/// <summary>One of the analogue controls of the encoder, with the ends of
/// its scale, so a slider can be built without this side knowing what any
/// of them are.</summary>
public class ControlInfo
{
    public int Index;
    public string Name = "";
    public int Min, Max, Neutral, Step = 1, Value;
}

/// <summary>Everything the board holds, as one dump.</summary>
public class BoardSettings
{
    public string TextId = "", TextSub = "", TickerText = "";
    public string Contest1 = "", Contest2 = "";
    public string PcmText = "";   // the Ham PCM text channel, 10 characters
    // The two fixed lines for the insert boxes, shown instead of the
    // date and time.
    public string InsertLeft = "", InsertRight = "";

    // The teletext page, one entry per row (1 based in the protocol, 0
    // based here). An empty row means untouched: the board falls back
    // to its own built-in default page for that row.
    public string[] TeletextRows = Array.Empty<string>();
    public int ContestNr, Luma, Chroma, CurrentCard;

    // Kept in flash beside the card, so a running show resumes by itself
    // after a power cut.
    public bool SlideshowOn;
    public bool Teletext, Its, Vbi, G924Bars, TcgAp2, Dirty;
    public bool SlideFadeOn = true;   // matches CFG_SLIDE_FADE_ON in the firmware
    public bool DstAutoOn;            // matches CFG_DST_AUTO_ON (false) in the firmware
    public int DateFormat;
    public int DisplayBright = 100;   // the little panel, 0 to 100
    public int Proto;

    // The board's own clock, not this computer's: no battery, so it
    // starts again at the time in config.h after a power cut.
    public DateTime Clock = DateTime.UtcNow;

    public readonly List<string> AspectNames = new();
    public readonly List<string> LumaNames = new();
    public readonly List<string> ChromaNames = new();
    public readonly List<CardInfo> Cards = new();
    public readonly List<ControlInfo> Controls = new();
}

public static class BoardConfig
{
    // Protocol shape this tool speaks. Bump together with the matching
    // number in the firmware's printSettings().
    public const int Speaks = 26;

    /// <summary>Opens the port the way the board needs it. DTR must be on;
    /// see BoardUpload.Send().</summary>
    private static SerialPort Open(string portName, int timeoutMs = 4000)
    {
        var port = new SerialPort(portName, 115200)
        {
            ReadTimeout = timeoutMs,
            WriteTimeout = timeoutMs,
            NewLine = "\n",
            DtrEnable = true,
            RtsEnable = true,
        };
        port.Open();
        port.DiscardInBuffer();
        return port;
    }

    private static void Close(SerialPort port)
    {
        try { port.Close(); } catch { /* allowed to fail */ }
    }

    /// <summary>Everything the board has. Reads until it says it is done.</summary>
    public static BoardSettings Read(string portName, CancellationToken stop)
    {
        using var port = Open(portName);
        try
        {
            Trace.Say("reading the settings: sending A");
            port.Write("A");
            var s = new BoardSettings();
            WaitFor(port, "[cfg] begin", stop);
            var end = DateTime.UtcNow.AddSeconds(20);
            while (DateTime.UtcNow < end)
            {
                string line = ReadLine(port, stop);
                if (line.StartsWith("[cfg] end", StringComparison.Ordinal))
                {
                    if (s.Proto != Speaks)
                        throw new UploadError(
                            s.Proto == 0
                                ? "board firmware predates this tool; flash matching firmware"
                                : $"board v{s.Proto} != tool v{Speaks}; flash matching firmware");
                    return s;
                }
                Take(s, line);
            }
            throw new UploadError("the board did not finish its list of settings");
        }
        finally { Close(port); }
    }

    // One line of the dump. Anything not recognised is ignored: a newer
    // firmware may send more than this program knows.
    private static void Take(BoardSettings s, string line)
    {
        if (line.StartsWith("[card] ", StringComparison.Ordinal)) { TakeCard(s, line[7..]); return; }
        if (line.StartsWith("[aspect] ", StringComparison.Ordinal)) { TakeName(s.AspectNames, line[9..]); return; }
        if (line.StartsWith("[luma] ", StringComparison.Ordinal)) { TakeName(s.LumaNames, line[7..]); return; }
        if (line.StartsWith("[chroma] ", StringComparison.Ordinal)) { TakeName(s.ChromaNames, line[9..]); return; }
        if (line.StartsWith("[control] ", StringComparison.Ordinal)) { TakeControl(s, line[10..]); return; }
        if (!line.StartsWith("[cfg] ", StringComparison.Ordinal)) return;

        string rest = line[6..];
        int sp = rest.IndexOf(' ');
        if (sp <= 0) return;
        string name = rest[..sp];
        string value = rest[(sp + 1)..].TrimEnd();
        int num = Number(value);

        // teletext.<row>, 1 based; teletextrows sizes the array first, so
        // it must arrive before the rows do (printSettings() sends it early).
        if (name == "teletextrows")
        {
            s.TeletextRows = new string[Math.Max(0, num)];
            return;
        }
        if (name.StartsWith("teletext.", StringComparison.Ordinal))
        {
            if (int.TryParse(name.AsSpan(9), out int row) &&
                row >= 1 && row <= s.TeletextRows.Length)
                s.TeletextRows[row - 1] = value;
            return;
        }

        // The count lines (cards, aspects, inserts, fonts, slots) are not
        // taken up: each count follows from the length of the list itself.
        switch (name)
        {
            case "textid": s.TextId = value; break;
            case "textsub": s.TextSub = value; break;
            case "tickertext": s.TickerText = value; break;
            case "contest1": s.Contest1 = value; break;
            case "contest2": s.Contest2 = value; break;
            case "pcmtext": s.PcmText = value; break;
            case "insertleft": s.InsertLeft = value; break;
            case "insertright": s.InsertRight = value; break;
            case "contestnr": s.ContestNr = num; break;
            case "teletext": s.Teletext = num != 0; break;
            case "its": s.Its = num != 0; break;
            case "vbi": s.Vbi = num != 0; break;
            case "luma": s.Luma = num; break;
            case "chroma": s.Chroma = num; break;
            case "g924bars": s.G924Bars = num != 0; break;
            case "tcgap2": s.TcgAp2 = num != 0; break;
            case "slidefade": s.SlideFadeOn = num != 0; break;
            case "dstauto": s.DstAutoOn = num != 0; break;
            case "slideshow": s.SlideshowOn = num != 0; break;
            case "card": s.CurrentCard = num; break;
            case "dirty": s.Dirty = num != 0; break;
            case "proto": s.Proto = num; break;
            case "datefmt": s.DateFormat = num; break;
            case "displaybright": s.DisplayBright = num; break;
            case "clock":
                if (DateTime.TryParseExact(value, "yyyy-MM-dd HH:mm:ss",
                                           CultureInfo.InvariantCulture,
                                           DateTimeStyles.None, out DateTime t))
                    s.Clock = t;
                break;
        }
    }

    // "<index> <name>", name may hold spaces. Index read rather than
    // assumed, so a gap in the numbering shows as a missing entry.
    private static void TakeName(List<string> into, string rest)
    {
        int sp = rest.IndexOf(' ');
        if (sp <= 0) return;
        if (!int.TryParse(rest[..sp], out int idx) || idx < 0 || idx > 64) return;
        while (into.Count <= idx) into.Add("");
        into[idx] = rest[(sp + 1)..].TrimEnd();
    }

    // "<index> min=.. max=.. neutral=.. step=.. value=.. <name>", name last.
    private static void TakeControl(BoardSettings s, string rest)
    {
        string[] f = rest.Split(' ');
        if (f.Length < 2 || !int.TryParse(f[0], out int index)) return;
        var c = new ControlInfo { Index = index };
        int i = 1;
        for (; i < f.Length; i++)
        {
            int eq = f[i].IndexOf('=');
            if (eq <= 0) break;
            int v = Number(f[i][(eq + 1)..]);
            switch (f[i][..eq])
            {
                case "min": c.Min = v; break;
                case "max": c.Max = v; break;
                case "neutral": c.Neutral = v; break;
                case "step": c.Step = Math.Max(1, v); break;
                case "value": c.Value = v; break;
            }
        }
        c.Name = string.Join(' ', f[i..]).Trim();
        s.Controls.Add(c);
    }

    // "<number> field=value field=value ... <name>", name last.
    private static void TakeCard(BoardSettings s, string rest)
    {
        string[] f = rest.Split(' ');
        if (f.Length < 2 || !int.TryParse(f[0], out int number)) return;
        var c = new CardInfo { Number = number };
        int i = 1;
        for (; i < f.Length; i++)
        {
            int eq = f[i].IndexOf('=');
            if (eq <= 0) break;                      // no more fields: the name starts here
            string key = f[i][..eq];
            int v = Number(f[i][(eq + 1)..]);
            switch (key)
            {
                case "showid": c.ShowId = v != 0; break;
                case "showsub": c.ShowSub = v != 0; break;
                case "hassub": c.HasSub = v; break;
                case "ticker": c.Ticker = v; break;
                case "tickery": c.TickerY = v; break;
                case "tickerfont": c.TickerFont = v; break;
                case "tickerspeed": c.TickerSpeed = v; break;
                case "tickerscale": c.TickerSize = v; break;
                case "textfont": c.TextFont = v; break;
                case "fixedfont": c.FixedFont = v != 0; break;
                case "aspect": c.Aspect = v; break;
                case "insert": c.Insert = v; break;
                case "photo": c.Photo = v != 0; break;
                case "own": c.Own = v; break;
                case "hasid": c.HasId = v; break;
                case "hasbox": c.HasBox = v; break;
                case "moving": c.Moving = v != 0; break;
                case "customphoto": c.CustomPhoto = v != 0; break;
                case "option": c.Option = v; break;
                case "hasline": c.HasLine = v; break;
                case "enabled": c.Enabled = v != 0; break;
                // Kept as the hex text the board sent; only the swatch
                // and the preview read them, and both want a colour.
                case "tickercolor": c.TickerColor = f[i][(eq + 1)..]; break;
                case "idcolor": c.IdColor = f[i][(eq + 1)..]; break;
                case "subcolor": c.SubColor = f[i][(eq + 1)..]; break;
                case "inscolor": c.InsColor = f[i][(eq + 1)..]; break;
                // In hex, read as hex rather than through Number(), which
                // would quietly make it zero.
                case "crc":
                    c.Crc = uint.TryParse(f[i][(eq + 1)..], NumberStyles.HexNumber,
                                          CultureInfo.InvariantCulture, out uint u) ? u : 0;
                    break;
            }
        }
        c.Name = string.Join(' ', f[i..]).Trim();
        s.Cards.Add(c);
    }

    private static int Number(string s) =>
        int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? v : 0;

    /// <summary>Sets one thing. Costs nothing on the board; it is not in
    /// flash until Save().</summary>
    public static void Set(string portName, string name, string value, CancellationToken stop)
    {
        using var port = Open(portName);
        try { SetOn(port, name, value, stop); }
        finally { Close(port); }
    }

    /// <returns>The board's answer. A card scoped set carries the new
    /// checksum of that card, "crc=1A2B3C4D", so this side can tell
    /// whether the picture it keeps is still good without asking for the
    /// whole dump.</returns>
    private static string SetOn(SerialPort port, string name, string value,
                                CancellationToken stop, int seconds = 20)
    {
        Trace.Line("send", "K");
        port.Write("K");
        WaitFor(port, "[cfg] type", stop, seconds);
        Trace.Line("send", name + "=" + value + "\n");
        port.Write(name + "=" + value + "\n");
        return WaitFor(port, "[cfg] set", stop, seconds);
    }

    /// <summary>The checksum out of that answer, or null when there is
    /// none: only a card scoped set carries one.</summary>
    public static uint? CrcIn(string reply)
    {
        int at = reply.IndexOf("crc=", StringComparison.Ordinal);
        if (at < 0) return null;
        string hex = reply[(at + 4)..].Trim();
        return uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture,
                             out uint v) ? v : null;
    }

    /// <summary>The card number out of a "currentcard" answer (1
    /// based, matching CardInfo.Number), or null when there is none.
    /// </summary>
    public static int? CurrentCardIn(string reply)
    {
        int at = reply.IndexOf("card=", StringComparison.Ordinal);
        if (at < 0) return null;
        string digits = reply[(at + 5)..].Trim();
        return int.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture,
                            out int v) ? v : null;
    }

    // --- a connection that stays open --------------------------------------
    //
    // Held open while something is being dragged, so every step of a
    // slider really goes over instead of paying tens of milliseconds per
    // open/close.
    public static SerialPort OpenLive(string portName) => Open(portName, 1000);

    public static string SetLive(SerialPort port, string name, string value) =>
        SetOn(port, name, value, CancellationToken.None, 1);

    public static void CloseLive(SerialPort port) => Close(port);

    /// <summary>Puts the board's clock right. Done at every Connect, so
    /// the insert boxes and any card fetched afterwards show this
    /// computer's time. Not kept in flash and marks nothing unsaved.
    /// </summary>
    public static void SetClock(string portName, DateTime now, CancellationToken stop)
    {
        using var port = Open(portName);
        try
        {
            Trace.Line("send", "T");
            port.Write("T");
            WaitFor(port, "[clock] type", stop);
            string line = now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);
            Trace.Line("send", line + "\n");
            port.Write(line + "\n");
            WaitFor(port, "[clock] set", stop);
        }
        finally { Close(port); }
    }

    /// <summary>Writes what has been set into flash. The picture goes black
    /// for about 56 ms while it happens.</summary>
    public static string Save(string portName, CancellationToken stop)
    {
        using var port = Open(portName);
        try
        {
            port.Write("*");
            return WaitFor(port, "[storage] ", stop).Trim();
        }
        finally { Close(port); }
    }

    // The caller's own deadline: a fixed timeout here would make a
    // shorter caller deadline meaningless.
    private static string ReadLine(SerialPort port, CancellationToken stop, int seconds = 20)
    {
        var line = new StringBuilder();
        var end = DateTime.UtcNow.AddSeconds(seconds);
        while (DateTime.UtcNow < end)
        {
            stop.ThrowIfCancellationRequested();
            int c;
            try { c = port.ReadByte(); }
            catch (TimeoutException) { continue; }
            catch (IOException e) { throw new UploadError("connection lost: " + e.Message); }
            if (c < 0) continue;
            if (c == '\n')
            {
                Trace.Line("recv", line.ToString());
                return line.ToString();
            }
            if (c != '\r') line.Append((char)c);
        }
        throw new UploadError("the board went quiet");
    }

    private static string WaitFor(SerialPort port, string start, CancellationToken stop,
                                  int seconds = 20)
    {
        var end = DateTime.UtcNow.AddSeconds(seconds);
        while (DateTime.UtcNow < end)
        {
            string r = ReadLine(port, stop, seconds);
            // Checked before the prefix match: a line can both start with
            // what is being waited for and say it went wrong, e.g.
            // "[font] 0 does not exist" starts with "[font] ".
            string? bad = BoardWords.Refusal(r);
            if (bad != null) throw new UploadError(bad);
            if (r.StartsWith(start, StringComparison.Ordinal)) return r;
        }
        throw new UploadError($"no reply to \"{start}\" within {seconds} s");
    }
}
