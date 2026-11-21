// update.h: checks for a newer version in the GitHub releases
#pragma once
#include <stdbool.h>

// starts the check in the background (once)
void update_start_check(void);

bool update_available(void);      // a newer version exists
const char *update_latest(void);  // latest release tag, e.g. "v1.1.0"

// downloads the .3dsx of the latest release and replaces the file on the SD.
// blocking (a few seconds). 0 if ok, negative otherwise.
int update_download(void);
