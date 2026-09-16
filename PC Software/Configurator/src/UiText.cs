// All UI text in one place: captions, labels, tooltips, titles, fixed
// messages. {0}-style holes are filled with string.Format; keep the hole
// count when editing a text.
//
// Not here: the serial protocol's own words (BoardWords.cs), log/trace
// lines, and plain data displays.
namespace PalSign;

public static class UiText
{
    // --- the window and the bar -------------------------------------
    public const string AppTitle = "PAL-sign";
    public const string About = "About";
    public const string AboutAppName = "PAL-sign configurator";
    public const string Save = "Save";
    public const string Cancel = "Cancel";
    public const string Connect = "Connect";
    public const string Disconnect = "Disconnect";
    public const string Search = "Search";
    public const string AlwaysOnTop = "Always on top";
    public const string PortLabel = "Port:";
    public const string SavingTitle = "Saving";
    public const string SavingText = "Writing settings to board";
    public const string UnsavedTitle = "Unsaved changes";
    public const string UnsavedAsk =
        "There are changes not yet written to the board's flash.\nSave them before disconnecting?";
    public const string UploadingTitle = "Uploading";
    public const string UploadingFmt = "writing {0} to board";
    public const string ErasingFmt = "emptying {0}";

    // --- the tabs ----------------------------------------------------
    public const string TabCards = "Cards";
    public const string TabSlideshow = "Slideshow";
    public const string TabSystem = "System";

    // --- the Cards page ----------------------------------------------
    public const string ShowFirstLine = "Show first line on this card";
    public const string ShowSecondLine = "Show second line on this card";
    public const string MovingLineSwitch = "Moving line on this card";
    public const string CardEnabledSwitch = "Card enabled";
    public const string CardIsOff = "This card is disabled!";
    public const string SlotIsEmpty = "An empty card has nothing to show";
    public const string ChromaBars = "Chroma test bars";
    public const string Ap2Variant = "AP2 variant";
    public const string CustomPhotoSwitch = "Show custom photo";
    public const string PcmResolution = "Resolution";
    public const string Pcm14Bit = "14 bit (EIAJ standard)";
    public const string Pcm16Bit = "16 bit (Sony PCM-F1)";
    public const string PcmPreEmphasis = "Pre-emphasis 50/15 us";
    public const string PcmCtrlInPicture = "Control line in picture";
    public const string PcmText = "PCM text";
    public const string HamQuality = "Quality";
    public const string HamHq = "HQ: 48 kHz, 14 bit";
    public const string HamLq = "LQ: 32 kHz, 12 bit";
    public const string HamVoice = "Voice: 32 kHz, 8 bit A-law";
    public const string HamNarrow = "Narrow: 16 kHz mono, A-law";
    public const string TipHamQuality =
        "HQ is the full row (3.4 Mbit/s). LQ and Voice run at 32 kHz with fewer bits a sample and a shorter row, " +
        "2.6 and 2.0 Mbit/s, for less FM bandwidth; Voice puts every bit inside the error correction. " +
        "Narrow is mono at 16 kHz, 1.4 Mbit/s.";
    public const string TipPcmText =
        "Up to 10 characters sent inside the audio rows, protected like the audio: capitals, digits and punctuation.";
    public const string TipPcmCtrlInPicture =
        "Moves the control line and the data down to the first line a capture card records, for software decoders " +
        "that never see the blanking. Off is the standard placement; on, a real adaptor still plays, correcting all the time.";
    public const string TipPcm16Bit =
        "Off: the 14 bit EIAJ/IEC 60841 standard with P and Q correction, playable on every adaptor. " +
        "On: Sony's 16 bit variant, two more bits per sample in place of the Q word.";
    public const string TipPcmPreEmphasis =
        "Boosts the treble before encoding, as Sony recorders always did; the control block tells a decoder to undo it. " +
        "Off sends the audio flat.";
    public const string UploadPortrait = "Upload portrait...";
    public const string InsertBoxesLabel = "Insert boxes";
    public const string TipTickerColor = "The color of the ticker on this card. The outline stays black.";
    public const string TipIdColor = "The color of the first line on this card.";
    public const string TipSubColor = "The color of the second line on this card.";
    public const string TipInsColor =
        "The color of the insert boxes on this card: the date & time, or the "
        + "fixed texts. The black boxes themselves stay black.";
    public const string PreviewTitle = "Preview screen";
    public const string PreviewNoPicture = "no picture fetched yet";
    public const string PreviewEmptySlot = "empty slot";
    public const string ContestTopLine = "Top line";
    public const string ContestBottomLine = "Bottom line";
    public const string ContestNumber = "Number";

    public const string UploadPicture = "Upload a picture...";
    public const string EmptyThisSlot = "Empty this slot";
    public const string SaveAsPicture = "Save as picture...";
    public const string LogButton = "Log";

    public const string TickerOff = "Off";
    public const string TickerSolid = "Solid";
    public const string TickerTransparent = "Transparent";
    public const string TickerSizeXSmall = "Extra small";
    public const string TickerSizeSmall = "Small";
    public const string TickerSizeMedium = "Medium";
    public const string TickerSizeLarge = "Large";
    public const string TickerSizeXLarge = "Extra large";
    public const string InsertNone = "No boxes, the grid carries on";
    public const string InsertTime = "Time only";
    public const string InsertDateTime = "Date & time";
    public const string InsertFixedTexts = "The two fixed texts, from the System tab";
    public const string FontPm5544 = "PM5544";
    public const string FontPm8546 = "PM8546";
    public const string FontInter = "Inter";
    public const string FontSquare = "Doto";

    public const string TipCardEnabled =
        "Off: p, P, the knob on the board and the slideshow skip this card, and "
        + "double click here does not show it either. Typing its number on the "
        + "serial port still works. An empty upload slot is always off.";
    public const string TipMovingLine =
        "A white stripe sweeping through a black box of the card, corner to corner "
        + "in a second each way. Only a card with a box for it has this switch. The "
        + "preview animates the stripe itself; the fetched picture does not carry "
        + "it, because it moves.";
    public const string TipCardList =
        "■  ticker in a bar of its own\n"
        + "□  ticker over the picture\n"
        + "      nothing: no ticker on that card\n\n"
        + "►  the card on the television now\n"
        + "A grayed name is an upload slot with nothing in it, or a card "
        + "that is switched off.\n"
        + "Double click puts a card on the television; a card that is "
        + "switched off is refused.";
    public const string TipChromaBars =
        "The PM5644 16:9 only. Turning them off unavoidably means date and time in "
        + "the boxes: the EPROM holds no other variant without the bars.";
    public const string TipAp2 =
        "BBC Card G only. AP1 and AP2 differ only in the small block at the top left.";
    public const string TipCustomPhoto =
        "BBC Test Card F/W only. Shows the photo uploaded with \"Upload portrait...\" "
        + "instead of the original, once one has been uploaded.";

    public const string PressConnectFirst = "Press Connect to read the board";
    public const string NoCardChosen = "No card selected";
    public const string NoPortChosen = "No port selected";
    public const string StillBusy = "Still working on the last request";
    public const string LetGoOfBoard = "Disconnect";
    public const string NoPictureYet = "No picture for this card yet";
    public const string WrittenToFmt = "Written to {0}";
    public const string CardOnScreenFmt = "Card {0} on screen";
    public const string SlotEmptiedFmt = "{0} emptied";

    public const string TalkingTitle = "Communicating";
    public const string FetchingCountFmt = "Fetching {0} cards";
    public const string FetchingCardOfFmt = "Fetching {0}, {1} of {2}";
    public const string FetchingOneFmt = "Fetching {0}";

    public const string EmptySlotAskFmt = "Empty Custom {0} ({1})?";
    public const string EmptySlotTitle = "Empty the slot";
    public const string SaveCardTitle = "Save the card as a picture";
    public const string PngFilter = "PNG (*.png)|*.png";

    // --- the photograph window ---------------------------------------
    public const string PictureForFmt = "Picture for {0}";
    public const string UploadOverwriteAskFmt = "{0} already has a picture. Replace it?";
    public const string UploadOverwriteTitle = "Replace the picture";
    public const string NoPhotoChosen = "No photo chosen yet";
    public const string SettingsGroup = "Settings";
    public const string CropUntilFills = "Crop picture";
    public const string ForceSquarePixels = "Force square-pixel scaling on an exact 720x576 source";
    public const string FlickerLabel = "Flicker filter:";
    public const string FlickerFull = "Twitter fully removed";
    public const string FlickerOff = "Off: sharp, but may twitter";
    public const string FlickerPartial = "Partial";
    public const string ChoosePhoto = "Choose photo...";
    public const string UploadButton = "Upload";
    public const string SourceShownFmt = "Source {0}x{1}  ->  720x576, shown here at screen ratio 4:3";
    public const string CannotProcess = "Cannot process the photo:\n";
    public const string ChoosePortFirst = "Choose a port first.";
    public const string ImagesFilter = "Images|*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff|All files|*.*";

    // --- the portrait window (BBC Test Card F/W's own photo) ----------
    public const string UploadPortraitTitle = "Upload portrait";
    public const string PortraitExplain =
        "One photo, cropped once here to fill 720x576. Both outlines show where "
        + "Test Card F (blue) and Test Card W (orange) each cut their own oval "
        + "window out of it; drag the picture and use the zoom slider until a "
        + "face sits inside both.";
    public const string PortraitZoom = "Zoom";
    public const string UploadingPortraitF = "Uploading to BBC Test Card F...";
    public const string UploadingPortraitW = "Uploading to BBC Test Card W...";
    public const string PortraitUploadDone =
        "Done. Turn on \"Show custom photo\" on BBC Test Card F and/or W to use it.";
    public const string CustomSlotFmt = "Custom {0}";

    // --- the Slideshow page ------------------------------------------
    public const string ReadFromBoard = "Read from board";
    public const string WriteToBoard = "Write to board";
    public const string ClearButton = "Clear";
    public const string SlideshowExplain =
        "Drag a card into the right hand list, or double click it, to add a step;\n"
        + "drag steps up and down to change the order. The seconds box belongs to the\n"
        + "selected step, and a card dropped in takes its current value. Double click\n"
        + "a step, or press Delete, to remove it. Press Connect first: the card names\n"
        + "come off the board.";
    public const string SlideshowCardsLabel = "Cards";
    public const string SlideshowStepsLabel = "Sequence";
    public const string SecondsLabel = "Seconds for the selected step:";
    public const string RemoveStep = "Remove step";
    public const string StartSlideshow = "Start slideshow";
    public const string StopSlideshow = "Stop slideshow";
    public const string StepsFullFmt = "The board holds at most {0} steps";
    public const string StepNotUnderstoodFmt = "Step \"{0}\" was not understood";
    public const string SlideshowEmptyAsk =
        "The sequence is empty. Writing that clears it on the board,\n"
        + "and the slideshow then takes every filled photograph slot again.\n\nGo on?";
    public const string SlideshowClearTitle = "Clear the sequence";
    public const string SlideshowClearAsk = "Clear the whole sequence?";
    public const string SlideshowUnwrittenTitle = "Unwritten sequence";
    public const string SlideshowUnwrittenAsk =
        "This sequence has not been written to the board yet.\nDiscard it?";

    // --- the System page ---------------------------------------------
    public const string GroupTexts = "Texts";
    public const string GroupSignal = "Signal";
    public const string GroupSwitches = "Switches";
    public const string GroupBrightness = "Display brightness";
    public const string GroupControls = "Picture controls";
    public const string LabelFirstLine = "First line";
    public const string LabelSecondLine = "Second line";
    public const string LabelTickerText = "Ticker text";
    public const string LabelTextDate = "Text date";
    public const string LabelTextTime = "Text time";
    public const string LabelLumaFilter = "Luma filter";
    public const string LabelChromaFilter = "Chroma filter";
    public const string NeutralButton = "Neutral";
    public const string TeletextSwitch = "Teletext on lines 7-22 and 320-335";
    public const string TeletextEditButton = "Teletext page...";
    public const string TeletextDialogTitle = "Teletext page";
    public const string TeletextDialogHint =
        "40 characters per row. A row left blank keeps the board's own default page.";
    public const string TeletextSavingText = "Writing the teletext page to board";
    public const string ItsSwitch = "Insertion test signals on 17, 18, 330, 331";
    public const string VbiSwitch = "Blanking open (without this the two above go nowhere)";
    public const string SlideFadeSwitch = "Fade between slides (off cuts straight to black and back)";
    public const string DstAutoSwitch = "Automatic summer/winter time (EU rule)";
    public const string FacePm5544 = "PM5544, fixed width, 30 rows";
    public const string FaceInter = "Inter, proportional, 24 rows";
    public const string FacePm8546 = "PM8546, proportional, 24 rows";
    public const string FaceSquare = "Doto, proportional, 31 rows";
    public const string FetchAllAgain = "Fetch all again";

    // --- Firmware update ----------------------------------------------
    public const string FirmwareUpdateButton = "Update firmware...";
    public const string FirmwareUpdateTitle = "Update firmware";
    public const string FirmwareUpdateFilter = "Firmware (*.uf2)|*.uf2";
    public const string FirmwareUpdateAskFmt =
        "Write {0} to the board as new firmware?\n\n"
        + "The picture stops while this runs. If anything goes wrong the board "
        + "is not bricked: hold its BOOTSEL button while reconnecting the USB "
        + "cable and try again.";
    public const string FirmwareUpdateDone =
        "Firmware written. The board restarts into it by itself.";
}
