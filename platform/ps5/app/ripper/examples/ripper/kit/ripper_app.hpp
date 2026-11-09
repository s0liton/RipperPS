// RipperPS: what the screen shows, kept up to date from the ripper payload.
//
// The payload does the work (it watches the drives, rips and delivers) and
// serves its state on http://127.0.0.1:<port>/api (src/service/web.c in the
// repository). This keeps a copy for the screen: status, drives, the rip
// queue, the catalog, settings and stats, refreshed on timers through the API
// client (platform/ps5/app/src/ripper_api.hpp) so a frame never waits for it.
// The same shape as RommPS's App, so both apps debug alike.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gfx/draw_list.hpp"
#include "gfx/renderer.hpp"
#include "ripper_api.hpp"

namespace hui::ui {
struct Fonts;
}

namespace ripper {

using hui::gfx::Color;

// The backdrop's colours, from the system of the disc in focus.
struct Palette {
  Color dark = Color::rgb(0x14182e);
  Color mid = Color::rgb(0x2b3466);
  Color accent = Color::rgb(0x76d6ff);
};

// What a disc can be ripped to (/api/drives' offer.formats).
struct Format {
  std::string id, label;
  std::string availability;  // "available", or why not
  std::string reason;        // the payload's sentence for it
  bool available = false;
};

struct Disc {
  std::string kind, profile, title, title_id, volume_id;
  double size = 0;
  bool css = false;
  int tracks = 0, audio_tracks = 0;
  // Looked up once the disc is identified (details.c in the payload).
  std::string display, subtitle, artist;
  std::string details;  // "looking", "done" or ""
  std::string cover;    // the API path of its cover, "" if none
};

// What the payload suggests for a disc: the system's profile from Settings.
struct Suggestion {
  std::string format, destination, folder, romm_platform, name;
  std::string options;  // a JSON object, as the payload sent it
};

// The console installing a PS4/PS5 disc's game, which a dump waits for.
struct Install {
  double done = 0, total = 0, rate = 0, eta = 0;
  bool playable = false;  // the system lets it be played while the rest copies
  float progress() const {
    return total > 0 ? static_cast<float>(done / total) : 0.0f;
  }
};

struct Drive {
  std::string id, label, source, vendor, product, revision, reads, message;
  // empty, loading, identifying, ready, unsupported, installing, ripping or done
  std::string state;
  Install install;  // while installing
  int disc_seq = 0;
  bool has_disc = false;
  Disc disc;
  bool has_offer = false;
  std::string system, system_name, group;
  std::vector<Format> formats;
  Suggestion suggestion;
  int job = 0;  // the rip running from it, if any
  bool internal() const {
    return source == "internal";
  }
  bool reading() const {
    return state == "loading" || state == "identifying";
  }
  bool rippable() const {
    return has_offer && (state == "ready" || state == "done");
  }
  // What the drive is: "SH-224DB", or the label when it says nothing.
  std::string model() const;
  // The disc's name to show: the one looked up, its title, its volume id, its kind.
  std::string disc_name() const;
};

struct Job {
  int id = 0;
  // ripping, delivering, done, failed or cancelled
  std::string state;
  std::string phase, name, system, system_name, format, format_label, destination, destination_name, drive, drive_label,
      where, error, note;
  double done = 0, total = 0, rate = 0, eta = -1, started = 0, finished = 0;
  bool retryable = false;
  bool ripped = false;  // the rip stage finished: a failure was in the transfer
  // The two stages: reading the disc, then taking the rip to its destination.
  std::string stage;  // "rip", "transfer", "" once it ended
  double rip_done = 0, rip_total = 0, rip_seconds = 0;
  bool has_transfer = false;
  std::string transfer_kind;  // "move" or "upload"
  double transfer_done = 0, transfer_total = 0;
  bool active() const {
    return state == "ripping" || state == "delivering";
  }
  bool ended() const {
    return !active();
  }
  float progress() const {
    return total > 0 ? static_cast<float>(done / total) : 0.0f;
  }
  float rip_progress() const {
    return state == "done" || stage == "transfer" ? 1.0f
           : rip_total > 0                        ? static_cast<float>(rip_done / rip_total)
                                                  : 0.0f;
  }
  float transfer_progress() const {
    return state == "done" ? 1.0f : transfer_total > 0 ? static_cast<float>(transfer_done / transfer_total) : 0.0f;
  }
};

// The catalog (/api/catalog): what each kind of disc can become, the options
// of each format, and the kinds of destination with their fields.
struct Choice {
  std::string id, label;
  std::string only;  // the one system it's for ("ps4"), "" for all
};

struct Option {
  std::string id, label, type;  // type: "choice" or "toggle"
  std::vector<Choice> choices;
  std::string def;  // the default, as text ("5", "1")
};

struct FormatInfo {
  std::string id, label, help;
  bool planned = false;
  std::vector<Option> options;
};

struct System {
  std::string id, name, group, romm_slug;
  std::vector<FormatInfo> formats;
  const FormatInfo *format(const std::string &id) const;
};

struct Field {
  std::string id, label, type, placeholder, def;  // type: text, secret, toggle, folder
};

struct DestType {
  std::string type, label, help;
  bool available = true;
  std::vector<Field> fields;
};

struct Catalog {
  bool known = false;
  std::vector<System> systems;
  std::vector<DestType> dest_types;
  const System *system(const std::string &id) const;
  const DestType *dest_type(const std::string &type) const;
};

struct Destination {
  std::string id, type, name, where;          // where: a folder or a server, for the sheet
  std::map<std::string, std::string> values;  // every field, as text ("1"/"0" for toggles)
  std::map<std::string, bool> secret_set;     // a secret the payload keeps (never sent back)
};

// A system's profile: what its discs rip to by default.
struct Profile {
  std::string format, destination, folder, romm_platform;
  std::map<std::string, std::string> options;
};

struct Status {
  bool known = false;      // an answer arrived at least once
  bool reachable = false;  // the payload answered the last time
  std::string version, build, platform, ip, error;
  int port = 8790;
  double uptime = 0;
  int jobs_active = 0;
};

// The payload's settings that RipperPS shows and changes (/api/settings).
struct Settings {
  bool known = false;
  bool notify = true, auto_eject = true, auto_start = false, keep_front = true;
  bool tmdb_key_set = false;  // a key for movie names and posters is saved
  int read_retries = 3;
  std::vector<Destination> destinations;
  std::map<std::string, Profile> profiles;
  const Destination *destination(const std::string &id) const {
    for (const Destination &d : destinations)
      if (d.id == id)
        return &d;
    return nullptr;
  }
};

// What this console has ripped (/api/stats).
struct Stats {
  bool known = false;
  double discs = 0, failed = 0, bytes = 0, seconds = 0, sent = 0, since = 0;
  std::vector<std::pair<std::string, double>> systems;  // most first
};

// Signing in to RomM (/api/romm/pair): a code to approve on another device.
struct Pairing {
  std::string status;  // "", "pending", "ok", "error", "idle"
  std::string message, user_code, verification_url;
  std::string token;  // handed over once, when it went through
};

// The drive test (/api/diagnose): what the drive is and how it reads, zipped
// to send to whoever asked for it.
struct DriveTest {
  std::string state;  // idle, running, done, failed, cancelled
  std::string drive, step, file, error, drive_name, disc, findings;
  double done = 0, total = 0, commands = 0, answered = 0;
};

// An installed PS4 or PS5 game, to dump (/api/games).
struct Game {
  std::string title_id, title, name, system;
  double size = 0;
  bool complete = false, mounted = false, game_running = false;
  bool partial = false;  // parts never installed here (other languages): it can't be dumped yet
};

// A disc's cover as a texture.
struct Cover {
  std::uint32_t texture = 0;  // 0 until it's loaded
  float aspect = 1.0f;
  Palette palette;
  bool requested = false, failed = false;
  double used = 0;
};

struct RommPS {
  bool found = false;
  std::string url, username;
  bool tls_verify = true;
};

class App {
 public:
  App();

  // Once per frame: delivers finished requests, starts the timed ones and
  // turns fetched covers into textures (renderer may be null in tests).
  void tick(float dt, hui::gfx::Renderer *renderer);

  const Status &status() const {
    return status_;
  }
  // RipperPS carries the ripper payload of its own build
  // (assets/ripper/ripper.elf) and starts it when none is running.
  void start_payload();  // sends the payload to the HEN's loader
  // Sending it, or ripper is running and not answering yet (starting up,
  // or waking from rest mode): either way it'll answer in a moment.
  bool payload_starting() const {
    return start_state_ == 1 || payload_waiting_;
  }
  const std::string &payload_error() const {
    return start_error_;
  }

  const std::vector<Drive> &drives() const {
    return drives_;
  }
  const Drive *drive(const std::string &id) const;
  const std::vector<Job> &jobs() const {
    return jobs_;
  }
  const Job *job(int id) const;
  const Settings &settings() const {
    return settings_;
  }
  const Catalog &catalog() const {
    return catalog_;
  }
  const Stats &stats() const {
    return stats_;
  }
  const Pairing &pairing() const {
    return pairing_;
  }
  const RommPS &rommps() const {
    return rommps_;
  }
  const std::vector<Game> &games() const {
    return games_;
  }
  const DriveTest &drive_test() const {
    return test_;
  }
  void start_drive_test(const std::string &drive);
  void refresh_drive_test();
  void cancel_drive_test();
  bool games_loading() const {
    return games_busy_;
  }
  void refresh_games();
  // Dumps an installed game (format "dump"); done gets whether it started.
  void dump(const Game &g, const std::string &destination, const std::string &name, const std::string &folder,
            const std::string &options, std::function<void(bool)> done = nullptr);

  // A drive's disc cover; asks for it the first time. nullptr while none.
  const Cover *cover(const Drive &d);

  void eject(const std::string &drive);
  void identify(const std::string &drive);
  // Looks the disc up again by a name (a movie's real title, for its poster).
  void lookup(const std::string &drive, const std::string &name);
  // Starts a rip of the drive's disc. options: a JSON object ("{}" for the
  // format's defaults). done gets whether the payload took it.
  void rip(const Drive &d, const std::string &format, const std::string &destination, const std::string &name,
           const std::string &folder, const std::string &options, std::function<void(bool)> done = nullptr);
  void cancel_job(int id);
  void retry_job(int id);
  void clear_jobs();
  // Changes settings: fields is a JSON object's members ("\"notify\":true").
  // The copy here changes at once; a refusal puts the payload's back.
  void set_settings(const std::string &fields, const std::function<void(Settings &)> &apply);
  // Saves the whole list of destinations (one added, changed or removed).
  void save_destinations(const std::vector<Destination> &list, std::function<void(bool)> done = nullptr);
  // Checks a destination as edited; done gets whether it worked and what was found.
  void test_destination(const Destination &d, std::function<void(bool, const std::string &)> done);
  void save_profile(const std::string &system, const Profile &p);
  // RomM sign-in: a code to approve, followed until it's approved (pairing().token).
  void pair_start(const std::string &url, bool tls_verify);
  void pair_cancel();
  void refresh_stats();
  void refresh_rommps();
  // Keeps the console out of rest mode for that long (0 ends it).
  void keep_awake(int minutes);

  // Any other call to the payload.
  void get(const std::string &path, Api::Callback done) {
    api_.get(path, std::move(done));
  }
  void post(const std::string &path, const std::string &body, Api::Callback done) {
    api_.post(path, body, std::move(done));
  }
  // A notification: the console's own, through the payload; drawn in the
  // app when the payload can't be reached.
  void say(const std::string &message);
  // Asks for everything again now rather than at the next tick.
  void refresh_now();

  // A message for the screen to show for a few seconds ("" for none).
  const std::string &toast() const {
    return toast_;
  }
  float toast_age() const {
    return toast_age_;
  }
  double clock() const {
    return clock_;
  }

 private:
  void refresh_status();
  void refresh_drives();
  void refresh_jobs();
  void refresh_settings();
  void refresh_catalog();
  void refresh_pairing();
  // Whether a ripper is running, from the lock it holds while it runs:
  // 1 yes, 0 no, -1 can't tell.
  static int payload_lock();
  const std::string &bundled_payload();
  // The build of the ripper RipperPS carries ("" if it can't tell).
  std::string bundled_build();
  bool replaced_ = false;  // a different build was running and got replaced (once a launch)
  void action(const std::string &path, const std::string &body, const std::string &failed);

  Api api_;
  Api poll_api_;   // the timed refreshes, so an action never waits behind them
  Api cover_api_;  // covers, so a big one never holds up the rest
  Status status_;
  std::vector<Drive> drives_;
  std::vector<Job> jobs_;
  Settings settings_;
  Catalog catalog_;
  Stats stats_;
  Pairing pairing_;
  RommPS rommps_;
  std::vector<Game> games_;
  bool games_busy_ = false;
  DriveTest test_;
  float test_timer_ = 0;
  bool test_busy_ = false;
  std::map<std::string, Cover> covers_;  // by the cover's API path
  struct Pending {
    std::string path, bytes;
  };
  std::vector<Pending> to_decode_;
  double clock_ = 0;
  float status_timer_ = 0, drives_timer_ = 0, jobs_timer_ = 0, pair_timer_ = 0;
  bool status_busy_ = false, drives_busy_ = false, jobs_busy_ = false, settings_busy_ = false, catalog_busy_ = false,
       pair_busy_ = false;
  std::string bundled_;
  bool bundled_read_ = false;
  std::atomic<int> start_state_{0};  // 0 idle, 1 sending, 2 sent, 3 failed
  std::string start_error_;          // written before start_state_ becomes 3
  bool tried_start_ = false;
  bool payload_waiting_ = false;
  int unanswered_ = 0;  // status requests in a row ripper didn't answer
  std::string toast_;
  float toast_age_ = 0;
};

// The program's one App, created on first use.
App &app();

// Text the screen's fonts can draw: the characters they have stay, the others
// fold to their nearest ASCII, and only one with no stand-in at all reads "?".
std::string plain_text(const char *text);
inline std::string plain_text(const std::string &text) {
  return plain_text(text.c_str());
}

// The faces plain_text asks: the ones the screen draws text with. Set once the
// kit has loaded them, before the payload's first answer is read.
void set_text_fonts(const hui::ui::Fonts &fonts);

// "4.2 GB", "700 MB".
std::string human_size(double bytes);
// "1 h 05 min", "3 min 20 s".
std::string duration(double seconds);
// A system's colours (the backdrop, in Disc colours), by its group and id.
Palette system_palette(const std::string &group, const std::string &system);
// A JSON string literal ("\"text\""), escaped.
std::string json_string(const std::string &s);

// The first byte at or after at that starts a character of UTF-8 text: where
// to cut it without halving one.
inline std::size_t char_boundary(const std::string &text, std::size_t at) {
  while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
    ++at;
  return at;
}

}  // namespace ripper

// The program hands the kit's renderer over (ripper.cpp is compiled in a
// namespace of its own, so this is plain C).
extern "C" void ripper_set_renderer(void *renderer);
extern "C" void *ripper_renderer();
