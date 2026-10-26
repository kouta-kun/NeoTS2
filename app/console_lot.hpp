#pragma once

namespace app {

// NeoTS2 on the PlayStation 2: a lot from a bank (see tools/simbank) and a baked scene (see tools/ts2bake), drawn and lived in. This is what the game's own
// main does for a lot, with the baked scene in place of the game's assets and the builder, and the console's pad in place of the mouse and keyboard.
//   <bank folder> <scene file>       both on a disc (cdrom0:), or host: while developing
// With no arguments they are on the disc (cdrom0:\BANK and cdrom0:\LOT.CSC), or else beside the program (host:bank and host:lot.cscene).
int runConsoleLot(int argc, char** argv);

} // namespace app
