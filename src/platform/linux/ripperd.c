// The service on a PC: the same web UI and API as on the console, with a
// virtual drive for images. See platform.c for its environment variables.

#include "service.h"

int main(void) {
  return service_main();
}
