#include "thing.h"

#include "sdkconfig.h"

int thing_value(void) { return CONFIG_THING_VALUE * 2; }
