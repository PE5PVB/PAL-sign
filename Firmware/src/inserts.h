// The two insert boxes in the middle left and middle right of the PM5644
// G00 test card: the date on the left, the time on the right.
//
// The black boxes are part of the EPROM data, not the text overlay,
// baked in as three variants:
//
//   pat0  both boxes    -> date and time
//   pat1  right only    -> time only
//   pat2  no boxes      -> the grid simply carries on
//
// So leaving out the text is not enough; switching mode switches the
// variant too.
#ifndef INSERTS_H
#define INSERTS_H

#include "textoverlay.h"

static const int INSERT_NONE = 0;       // pat2: no boxes
static const int INSERT_TIME = 1;       // pat1: the time only
static const int INSERT_DATE_TIME = 2;  // pat0: date and time
static const int INSERT_TEXT = 3;       // pat0, with the two fixed texts

// Key i walks the first three; mode 3 is chosen over the port only.
static const int INSERT_CYCLE = 3;
static const int INSERT_COUNT = 4;

void insertSetMode(int m);

// Fills the two text lines for the current mode. Returns true when
// something changed. Call from the tick of the pattern.
bool insertPrepare(TextOverlay *left, TextOverlay *right, int lx0, int lx1, int rx0, int rx1,
                   int y0, int y1);

// Forces the next insertPrepare() to lay the text out again. Call from
// the init of a pattern.
void insertForceRefresh();
int insertGetMode();

// Does the card on screen really draw these boxes? Observed, not
// declared: a card says so by calling insertPrepare() from its tick.
bool insertInUse();

// The boxes of the card on screen, in picture coordinates. Meaningful
// only once insertInUse() is true.
void insertGetBoxes(int *lx0, int *lx1, int *rx0, int *rx1, int *y0, int *y1);

// Leaves date/time out while the PC tool fetches a picture; the black
// boxes stay, only what changes every second goes. Ask the pattern to
// tick again after setting this.
void insertSetBlank(bool on);
void insertClearInUse();
const char *insertModeName(int m);
#endif  // INSERTS_H
