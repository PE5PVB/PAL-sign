// The contest page's 90 row Inter, shared with whoever else wants to
// letter in the same face at the same size (Mixed bars; see
// mixedbar.cpp), instead of a second #include of contest_font.h, which
// is static there and would duplicate the tokens in flash.
#ifndef CONTEST_FACE_H
#define CONTEST_FACE_H

#include "../runglyph.h"

const ContestGlyph *contestGlyphFor(const char **pp);
int contestFaceTop();
int contestFaceBottom();
#endif  // CONTEST_FACE_H
