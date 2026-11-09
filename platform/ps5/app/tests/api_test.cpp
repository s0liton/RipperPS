// The app's API client against a running host build of ripper
// (platform/ps5/app/tests/api-test.sh starts one).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "cJSON.h"
#include "ripper_api.hpp"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); failures++; } } while (0)

int main(int argc, char **argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 8793;

    // Blocking: a good answer, an error answer, nobody home.
    ripper::Response r = ripper::http_request("127.0.0.1", port, "GET", "/api/status");
    CHECK(r.ok(), "status: %d %s", r.status, r.message().c_str());
    cJSON *j = r.json();
    CHECK(j && cJSON_IsString(cJSON_GetObjectItem(j, "platform")), "status JSON has no platform");
    cJSON_Delete(j);
    r = ripper::http_request("127.0.0.1", port, "GET", "/api/nope");
    // An error reply's message comes from its JSON.
    CHECK(r.status >= 400 && r.message().find("HTTP") != 0 && !r.message().empty(), "error reply: %d '%s'", r.status, r.message().c_str());
    r = ripper::http_request("127.0.0.1", 1, "GET", "/api/status", "", 2000);
    CHECK(r.status == 0 && !r.message().empty(), "dead port: %d", r.status);

    // Async: twenty requests, all delivered by poll(), nothing left over.
    {
        ripper::Api api("127.0.0.1", port);
        int got = 0, good = 0;
        for (int i = 0; i < 20; i++)
            api.get(i % 2 ? "/api/status" : "/api/settings", [&](const ripper::Response &res) { got++; good += res.ok(); });
        for (int t = 0; t < 400 && got < 20; t++) {
            api.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(got == 20 && good == 20, "async: %d delivered, %d ok", got, good);
        CHECK(api.pending() == 0, "async: %d still pending", api.pending());
        bool posted = false;
        api.post("/api/settings", "{\"notify\":false}", [&](const ripper::Response &res) { posted = res.ok(); });
        for (int t = 0; t < 400 && !posted; t++) { api.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(posted, "post /api/settings");
        // Destroying the client with work queued must not hang or crash.
        for (int i = 0; i < 5; i++) api.get("/api/status", nullptr);
    }
    std::printf(failures ? "%d failure(s)\n" : "api test passed\n", failures);
    return failures != 0;
}
