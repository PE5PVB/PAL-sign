// Writes a new firmware .uf2 to the board over USB, without anyone
// touching BOOTSEL by hand.
//
// Counterpart: Firmware/Firmware.ino, key g. That prints one line and
// calls the Arduino-Pico core's rp2040.rebootToBootloader(), the same
// reset_usb_boot() the ROM itself runs when BOOTSEL is held at power-on.
// The board's USB
// identity changes under us the moment that call runs: the serial port
// this class opens to send 'g' disappears, and a RPI-RP2 mass storage
// drive appears in its place a moment later. That drive always carries
// a file called INFO_UF2.TXT; that, not the volume label (which is
// harmless to rename), is what this class watches for.
//
// Once the drive is there, a plain file copy of the .uf2 onto it is
// the whole update: the RP2350 ROM bootloader resets into the copied
// firmware by itself as soon as the copy finishes, the same as dragging
// the file there in Explorer. Nothing here has to ask for that separately.
using System;
using System.IO;
using System.IO.Ports;
using System.Threading;

namespace PalSign;

public class FirmwareUpdateError : Exception
{
    public FirmwareUpdateError(string message) : base(message) { }
}

public static class BoardFirmwareUpdate
{
    /// <param name="report">every progress line</param>
    public static void Send(string portName, string uf2Path, Action<string> report,
                            CancellationToken stop)
    {
        if (!File.Exists(uf2Path)) throw new FirmwareUpdateError("file not found: " + uf2Path);

        report("rebooting into the USB bootloader");
        RebootToBootloader(portName);

        report("waiting for the bootloader drive to appear");
        string drive = WaitForBootloaderDrive(stop);
        report($"found it at {drive}");

        stop.ThrowIfCancellationRequested();
        string dest = Path.Combine(drive, Path.GetFileName(uf2Path));
        report("copying " + Path.GetFileName(uf2Path));
        File.Copy(uf2Path, dest, overwrite: true);

        report("copied; the board reboots into the new firmware on its own");
    }

    private static void RebootToBootloader(string portName)
    {
        var port = new SerialPort(portName, 115200)
        {
            WriteTimeout = 5000,
            // Same reasoning as BoardUpload.Send(): without DTR the
            // firmware's `if (!Serial) return;` guards can leave a
            // command silently ignored.
            DtrEnable = true,
            RtsEnable = true,
        };
        try
        {
            port.Open();
            port.Write("g");
            // Best effort only, not a requirement: the port can vanish
            // mid-read as USB re-enumerates into the mass storage
            // device, and that is not a failure, it is the point.
            try
            {
                port.ReadTimeout = 2000;
                port.ReadLine();
            }
            catch { /* the port going away here is expected, not an error */ }
        }
        finally
        {
            try { port.Close(); } catch { /* allowed to fail */ }
            port.Dispose();
        }
    }

    private static string WaitForBootloaderDrive(CancellationToken stop, int seconds = 20)
    {
        var end = DateTime.UtcNow.AddSeconds(seconds);
        while (DateTime.UtcNow < end)
        {
            stop.ThrowIfCancellationRequested();
            foreach (DriveInfo d in DriveInfo.GetDrives())
            {
                if (!d.IsReady) continue;
                try
                {
                    string marker = Path.Combine(d.RootDirectory.FullName, "INFO_UF2.TXT");
                    if (File.Exists(marker)) return d.RootDirectory.FullName;
                }
                catch { /* a drive that will not answer is not it either */ }
            }
            Thread.Sleep(250);
        }
        throw new FirmwareUpdateError(
            $"no bootloader drive appeared within {seconds} s; hold BOOTSEL on the board "
            + "and reconnect the USB cable by hand, then try again");
    }
}
