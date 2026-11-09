#ifndef RIPPER_WEB_H_
#define RIPPER_WEB_H_

// The web UI and its JSON API on one port (8790 by default). The PS5 app
// uses the same API over 127.0.0.1.
//
//   GET  /api/status                 version, address, jobs running
//   GET  /api/catalog                disc types, formats and options, destination types
//   GET  /api/drives                 each drive, its state, its disc and offer
//   POST /api/drives/<id>/eject
//   POST /api/drives/<id>/identify   read the disc again
//   POST /api/drives/<id>/load       {"path": ...}: an image into a virtual drive
//   POST /api/rip                    start a rip (jobs.h has the request)
//   GET  /api/jobs
//   POST /api/jobs/<id>/cancel, /api/jobs/<id>/retry, /api/jobs/clear
//   GET  /api/settings, POST /api/settings
//   POST /api/destinations/test      {destination}: connect and check
//   GET  /api/folders?path=          folders under the storage roots
//   GET  /api/log

int web_start(int port);
// 1 once after the listener had to be reopened (rest mode), else 0.
int web_take_reopened(void);

#endif
