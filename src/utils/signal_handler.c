#include "signal_handler.h"

#include <signal.h>

void setup_signals()
{
    signal(SIGPIPE, SIG_IGN);
}
