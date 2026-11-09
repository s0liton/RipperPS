#ifndef RIPPER_SERVICE_H_
#define RIPPER_SERVICE_H_

// Runs the service until the process ends: settings, drive watcher, jobs, web
// UI. Each platform's main() calls it.
int service_main(void);

#endif
