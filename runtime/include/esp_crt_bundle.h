#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Accepted and left alone: the HTTP stand-in checks servers against the Mac's own
// certificate store, so there is no bundle to attach.
esp_err_t esp_crt_bundle_attach(void* conf);
void esp_crt_bundle_detach(void* conf);

#ifdef __cplusplus
}
#endif
