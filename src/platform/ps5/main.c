// PS5 payload: the ripper service. Send it to the HEN's ELF loader (port 9021);
// the web UI comes up on port 8790 and the PS5 app talks to the same API.

#include "service.h"

int main(void) {
  return service_main();
}
