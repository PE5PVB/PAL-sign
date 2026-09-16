// The serial protocol to the board.
//
// Counterpart: Firmware/Firmware.ino, photoUpload(). The Pico
// acknowledges every sector with "A<n>" and only then may the next one
// follow: while writing, the board's interrupts are off and the USB is
// not serviced, so pushing on regardless overflows its buffer.
//
// The picture goes black during an upload.
using System;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;

namespace PalSign;

public class UploadError : Exception
{
    public UploadError(string message) : base(message) { }
}

public static class BoardUpload
{
    public const int Sector = 4096;

    public static string[] Ports() => SerialPort.GetPortNames();

    /// <summary>CRC32 the way zlib and the firmware work it out: reflected,
    /// polynomial 0xEDB88320, starts at 0xFFFFFFFF, inverted at the
    /// end.</summary>
    public static uint Crc32(byte[] data)
    {
        uint crc = 0xFFFFFFFFu;
        foreach (byte v in data)
        {
            crc ^= v;
            for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (uint)(-(int)(crc & 1)));
        }
        return crc ^ 0xFFFFFFFFu;
    }

    /// <param name="slot">1 through 14; see PHOTO_SLOTS in
    /// Firmware/src/photoflash.h, where that number belongs</param>
    /// <param name="report">every progress line</param>
    /// <param name="progress">(acknowledged sector, total)</param>
    public static void Send(string portName, byte[] data, string name, int slot,
                            Action<string> report, Action<int, int> progress,
                            CancellationToken stop)
    {
        uint crc = Crc32(data);
        bool wrong = false;
        int blocks = (data.Length + Sector - 1) / Sector;
        report($"upload to slot {slot} (Custom {slot}): {data.Length} bytes, " +
               $"{blocks} sectors, crc 0x{crc:X8}");

        var port = new SerialPort(portName, 115200)
        {
            ReadTimeout = 30000,
            WriteTimeout = 30000,
            NewLine = "\n",

            // DTR must be on. TinyUSB's tud_cdc_connected() (cdc_device.c)
            // is true only with DTR set: "DTR (bit 0) active is
            // considered as connected". The Arduino layer passes that
            // through operator bool(), and photoUpload() in Firmware.ino
            // starts with `if (!Serial) return;`, so without DTR the
            // board receives the 'u' but returns at once, silently.
            // .NET's SerialPort leaves DTR off by default; pyserial does not.
            //
            // At 1200 baud a DTR change restarts the board into UF2 mode
            // (SerialUSB.cpp); at 115200 it does not.
            DtrEnable = true,
            RtsEnable = true,
        };
        try
        {
            port.Open();
            port.DiscardInBuffer();
            port.Write("u");

            string line = WaitFor(port, "[upload] ready ", stop);
            string[] fields = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length < 3 || !int.TryParse(fields[2], out int expected))
                throw new UploadError("unintelligible reply from the board: " + line);
            if (expected != data.Length)
                throw new UploadError($"the firmware expects {expected} bytes, we have {data.Length}");

            // The line starts with the slot number, 1 through 14. The board
            // reads TWO digits, so "14 photo.jpg" ends up in slot 14 and not
            // in slot 1 with a file whose name starts with a 4.
            string shortName = name.Length > 30 ? name[..30] : name;
            port.Write($"{slot} {shortName}\n");

            report("erasing slot; that takes about ten seconds");
            // Erasing is the long step, up to 82 s.
            WaitFor(port, "[upload] erased", stop, 180);
            report("slot erased, sending...");

            for (int i = 0; i < blocks; i++)
            {
                stop.ThrowIfCancellationRequested();
                int from = i * Sector;
                int n = Math.Min(Sector, data.Length - from);
                port.Write(data, from, n);
                WaitFor(port, "A" + i, stop);
                progress(i + 1, blocks);
            }

            string reply = WaitFor(port, "[upload] done,", stop);
            report(reply.Trim());

            // The firmware reports the CRC it has read back out of the
            // FLASH, not that of what we sent. If they agree, then it
            // really is in there correctly.
            wrong = !reply.Contains($"0x{crc:X8}", StringComparison.OrdinalIgnoreCase);
            if (!wrong) report("CRC agrees; the photo is in flash and survives a power cut.");
        }
        finally
        {
            // Do not let it trip over a port that, for whatever reason,
            // is no longer there.
            try { port.Close(); } catch { /* allowed to fail */ }
            port.Dispose();
        }

        if (!wrong) return;

        // Emptied only after the port above has closed, since erasing
        // opens one of its own. Needed because the board has already
        // written the header and counts the slot as filled; its own
        // check only compares flash with flash, so this side, the only
        // one that knows what was meant to go in, undoes it.
        try { BoardSlots.Erase(portName, slot, report, stop); }
        catch (Exception e) { report("the slot could not be emptied: " + e.Message); }
        throw new UploadError("the firmware's CRC differs from ours; the slot has been emptied, "
                              + "do not trust what was sent");
    }


    /// <summary>The BBC Test Card F/W photo replacements. Counterpart:
    /// Firmware/Firmware.ino, portraitUploadFlow(), triggered over the
    /// "K" generic-setting line ("portrait.0=1"/"portrait.1=1") rather
    /// than the 'u' key Send() above uses, since the byte count differs
    /// per target and photoUpload() announces its (fixed) count before
    /// it would know which one was meant.</summary>
    /// <param name="target">0 for BBC Test Card F, 1 for W; see
    /// PORTRAIT_TESTCARDF/W in Firmware/src/portrait.h</param>
    public static void SendPortrait(string portName, byte[] data, int target,
                                    Action<string> report, Action<int, int> progress,
                                    CancellationToken stop)
    {
        uint crc = Crc32(data);
        bool wrong = false;
        int blocks = (data.Length + Sector - 1) / Sector;
        report($"upload to portrait {target}: {data.Length} bytes, {blocks} sectors, crc 0x{crc:X8}");

        var port = new SerialPort(portName, 115200)
        {
            ReadTimeout = 30000,
            WriteTimeout = 30000,
            NewLine = "\n",
            DtrEnable = true,   // see the note at Send() above; the same applies here
            RtsEnable = true,
        };
        try
        {
            port.Open();
            port.DiscardInBuffer();
            port.Write("K");
            WaitFor(port, "[cfg] type", stop);
            port.Write($"portrait.{target}=1\n");

            string line = WaitFor(port, "[upload] ready ", stop);
            string[] fields = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length < 3 || !int.TryParse(fields[2], out int expected))
                throw new UploadError("unintelligible reply from the board: " + line);
            if (expected != data.Length)
                throw new UploadError($"the firmware expects {expected} bytes, we have {data.Length}");

            report("erasing; that takes a few seconds");
            WaitFor(port, "[upload] erased", stop, 60);
            report("erased, sending...");

            for (int i = 0; i < blocks; i++)
            {
                stop.ThrowIfCancellationRequested();
                int from = i * Sector;
                int n = Math.Min(Sector, data.Length - from);
                port.Write(data, from, n);
                WaitFor(port, "A" + i, stop);
                progress(i + 1, blocks);
            }

            string reply = WaitFor(port, "[upload] done,", stop);
            report(reply.Trim());
            wrong = !reply.Contains($"0x{crc:X8}", StringComparison.OrdinalIgnoreCase);
            if (!wrong) report("CRC agrees; the photo is in flash and survives a power cut.");
        }
        finally
        {
            try { port.Close(); } catch { /* allowed to fail */ }
            port.Dispose();
        }

        if (wrong)
            throw new UploadError("the firmware's CRC differs from ours; try the upload again");
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
                if (r.StartsWith(start, StringComparison.Ordinal)) return r;
                {
                    string? bad = BoardWords.Refusal(r);
                    if (bad != null) throw new UploadError(bad);
                }
                continue;
            }
            if (c != '\r') line.Append((char)c);
        }
        throw new UploadError($"no reply to \"{start}\" within {seconds} s");
    }
}
