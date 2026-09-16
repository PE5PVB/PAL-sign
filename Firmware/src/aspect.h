// Aspect ratio: the WSS word on line 23 (EN 300 294 / ITU-R BT.1119) and
// the letterbox that goes with it. The ADV7391 inserts the word itself,
// see advSetWss(); the letterbox is drawn here, the same for every card.
#ifndef ASPECT_H
#define ASPECT_H

#include <stdint.h>

static const int ASPECT_OFF = 0;
static const int ASPECT_COUNT = 9;

void aspectSet(int mode);
int aspectGet();
const char *aspectName(int mode);

// What the video layer calls instead of the pattern directly: black in
// the letterbox bars, the active test card everywhere else.
void patternRenderRow(uint8_t *base, int y);
#endif  // ASPECT_H
