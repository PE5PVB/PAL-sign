// Emptying a photograph slot, and the slideshow sequence.
//
// Counterpart: Firmware/Firmware.ino, photoEraseSlot()/setSequenceFromCommand().
// Uploading is the other direction, in BoardUpload.cs.
//
// No slot readback here: the Cards page already shows every card, photo
// slots included, as the board renders it, and can save that; key D on
// the board offers the raw bytes for anyone who wants them by hand.
//
// The picture goes black while a slot is read back or erased, same as
// during an upload.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;

namespace PalSign;

public static class BoardSlots
{
    /// <summary>Opens the port the way the board needs it. DTR MUST BE ON;
    /// see the long note in BoardUpload.Send().</summary>
    private static SerialPort Open(string portName, int readTimeoutMs = 30000)
    {
        var port = new SerialPort(portName, 115200)
        {
            ReadTimeout = readTimeoutMs,
            WriteTimeout = 30000,
            NewLine = "\n",
            DtrEnable = true,
            RtsEnable = true,
        };
        port.Open();
        port.DiscardInBuffer();
        return port;
    }

    /// <summary>The slideshow sequence the board holds, as plain text.
    ///
    /// It comes out of the same "l" listing as the slots do: that saves a
    /// command of its own, and reading it can then never be confused with
    /// writing it. WaitFor walks past the slot lines by itself.</summary>
    public static string ReadSequence(string portName, CancellationToken stop)
    {
        using var port = Open(portName);
        try
        {
            port.Write("l");
            string line = WaitFor(port, "[seq] now ", stop);
            string text = line["[seq] now ".Length..].Trim();
            return text == "(none)" ? "" : text;
        }
        finally { Close(port); }
    }

    /// <summary>Puts a sequence on the board. An empty text clears it, and
    /// the slideshow then takes every filled slot again.
    ///
    /// The board checks the text itself and changes nothing when it does
    /// not parse, so a typing mistake never leaves half a sequence
    /// behind.</summary>
    public static void WriteSequence(string portName, string text, Action<string> report,
                                     CancellationToken stop)
    {
        using var port = Open(portName);
        try
        {
            port.Write("Q");
            WaitFor(port, "[seq] form", stop);
            port.Write(text.Trim() + "\n");

            // "[seq] set ..." or "[seq] cleared ...". A refusal comes back
            // as "[seq] failed, ..." and WaitFor turns that into an
            // exception by itself, on the word "failed".
            string line = WaitFor(port, "[seq] ", stop);
            report(line.Trim());
        }
        finally { Close(port); }
    }

    /// <summary>Empties a slot. The board erases it and writes no header
    /// afterwards, so it reads as empty from then on.</summary>
    public static void Erase(string portName, int slot, Action<string> report,
                             CancellationToken stop)
    {
        using var port = Open(portName);
        try
        {
            port.Write("E");
            WaitFor(port, "[erase] ready", stop);
            port.Write($"{slot}\n");

            string line = WaitFor(port, "[erase] ", stop);
            report(line.Trim());
            if (line.Contains("was already empty", StringComparison.Ordinal)) return;

            // Erasing is the long step, up to 82 s.
            report(WaitFor(port, "[erase] done", stop, 180).Trim());
        }
        finally { Close(port); }
    }

    private static void Close(SerialPort port)
    {
        // Do not let it trip over a port that, for whatever reason, is no
        // longer there.
        try { port.Close(); } catch { /* allowed to fail */ }
    }


    private static string WaitFor(SerialPort port, string start, CancellationToken stop,
                                  int seconds = 30)
    {
        var end = DateTime.UtcNow.AddSeconds(seconds);
        var line = new StringBuilder();
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
                string r = line.ToString();
                line.Clear();
                Trace.Line("recv", r);
                // Checked before the prefix match: "[seq] failed, ..."
                // also starts with "[seq] ".
                string? bad = BoardWords.Refusal(r);
                if (bad != null) throw new UploadError(bad);
                if (r.StartsWith(start, StringComparison.Ordinal)) return r;
                continue;
            }
            if (c != '\r') line.Append((char)c);
        }
        throw new UploadError($"no reply to \"{start}\" within {seconds} s");
    }
}
