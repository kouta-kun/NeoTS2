#pragma once

namespace app {

// A line of the program's own status text (a printf, and a newline is the caller's): into the trace ring and host:neots2.log directly. A console's standard output stopped
// reaching the PC after a few lines, so what must be read goes this way.
void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));

// NeoTS2 on the PlayStation 2: a lot from a bank (see tools/simbank) and a baked scene (see tools/ts2bake), drawn and lived in. This is what the game's own
// main does for a lot, with the baked scene in place of the game's assets and the builder, and the console's pad in place of the mouse and keyboard.
//   <bank folder> <scene file>       both on a disc (cdrom0:), or host: while developing
// With no arguments they are on the disc (cdrom0:\BANK and cdrom0:\LOT.CSC), or else beside the program (host:bank and host:lot.cscene).
int runConsoleLot(int argc, char** argv);

} // namespace app
