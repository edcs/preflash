#pragma once

namespace preflash {

// Makes the next join attempts fail the way a wrong password does, so a project's
// failure handling can be seen. An attempt already under way finishes as it began.
void wifi_force_join_failure(bool on);

// Online is the default. Offline, HTTP fails to connect and time never syncs, as with
// no route out, while the Wi-Fi link itself stays as the firmware left it.
void net_set_online(bool online);
bool net_online();

}  // namespace preflash
