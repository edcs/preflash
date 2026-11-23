// For builds with no SDL, the replay tool and the runtime's test suite: firmware's sound is
// counted and dropped, as on a Mac with no output device.
#include "host/sdl_audio.h"

namespace preflash::sdl_audio {

void listen_for_codec() {}
void open_if_asked() {}

}  // namespace preflash::sdl_audio
