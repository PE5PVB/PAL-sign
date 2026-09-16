// Whether the board has a setting that is not yet written to flash; makes
// the Save button blink. Volatile, no event: set from the port thread,
// read from the drawing thread on the beat it already blinks on.
namespace PalSign;

public static class Session
{
    private static volatile bool _dirty;

    /// <summary>Is there something on the board that is not in flash?</summary>
    public static bool Dirty
    {
        get => _dirty;
        set => _dirty = value;
    }
}
