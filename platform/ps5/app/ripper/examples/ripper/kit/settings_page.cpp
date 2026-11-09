// RipperPS: the Settings tab. See settings_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_page.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/tween.hpp"

namespace ripper {

namespace ui = hui::ui;
using hui::Action;
using hui::audio::Cue;
using hui::gfx::Color;
using hui::gfx::Rect;

namespace {

constexpr float kMargin = 96.0f;
constexpr float kListHeight = 7 * RowList::kRowHeight;
const Rect kSide{1240, 300, 584, 560};

constexpr int kAwakeMinutes[] = {0, 60, 180, 480};
constexpr const char *kAwakeLabels[] = {"Off", "1 hour", "3 hours", "8 hours"};

// "Living room NAS" -> "living-room-nas", unique among the destinations.
std::string new_id(const std::string &name, const std::vector<Destination> &taken) {
  std::string id;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!id.empty() && id.back() != '-')
      id += '-';
  }
  while (!id.empty() && id.back() == '-')
    id.pop_back();
  if (id.empty())
    id = "destination";
  std::string out = id;
  for (int n = 2;; ++n) {
    const bool used = std::any_of(taken.begin(), taken.end(), [&](const Destination &d) { return d.id == out; });
    if (!used)
      return out;
    out = id + "-" + std::to_string(n);
  }
}

const char *choice_label(const Option &o, const std::string &id) {
  for (const Choice &c : o.choices)
    if (c.id == id)
      return c.label.c_str();
  return o.choices.empty() ? "" : o.choices.front().label.c_str();
}

std::string step_choice(const Option &o, const std::string &id, int step) {
  const int n = static_cast<int>(o.choices.size());
  if (!n)
    return id;
  int at = 0;
  for (int i = 0; i < n; ++i)
    if (o.choices[static_cast<std::size_t>(i)].id == id)
      at = i;
  return o.choices[static_cast<std::size_t>(((at + step) % n + n) % n)].id;
}

bool truthy(const std::string &v) {
  return v == "1" || v == "true";
}

}  // namespace

void SettingsPage::enter() {
  mode_ = kMain;
  discard_armed_ = false;
  list_.set_focus(0);
  app().refresh_stats();
  app().refresh_drive_test();
  app().refresh_rommps();
}

std::vector<Row> SettingsPage::rows(std::vector<Item> *items) const {
  std::vector<Row> rows;
  std::vector<Item> local;
  std::vector<Item> &it = items ? *items : local;
  it.clear();
  if (mode_ == kDestination || mode_ == kPairing)
    destination_rows(rows, it);
  else if (mode_ == kProfile)
    profile_rows(rows, it);
  else
    main_rows(rows, it);
  return rows;
}

void SettingsPage::main_rows(std::vector<Row> &rows, std::vector<Item> &items) const {
  App &a = app();
  const Settings &s = a.settings();
  auto add = [&](Row r, Item i) {
    rows.push_back(std::move(r));
    items.push_back(std::move(i));
  };
  const Look &l = look(current_look());
  add({RowKind::header, "Look"}, {});
  add({RowKind::choice, "Theme", l.name, l.family}, {Item::look});
  add({RowKind::toggle, "Interface sounds", "", "", ui_sounds()}, {Item::sounds});

  add({RowKind::header, "Ripping"}, {});
  add({RowKind::toggle, "Notifications", "", "When a disc is ready and when a rip ends", s.notify}, {Item::notify});
  add({RowKind::toggle, "Eject when done", "", "", s.auto_eject}, {Item::eject});
  add({RowKind::toggle, "Rip as soon as a disc is ready", "", "With its profile, no questions", s.auto_start},
      {Item::auto_start});
  add({RowKind::toggle, "Stay in front when a disc goes in", "", "The console would show the disc instead",
       s.keep_front},
      {Item::keep_front});
  add({RowKind::choice, "Read retries", std::to_string(s.read_retries), "For a sector that won't read"},
      {Item::retries});
  add({RowKind::choice, "Keep the console awake", kAwakeLabels[awake_], "Rips keep it awake anyway"}, {Item::awake});

  add({RowKind::action, "Movie names and posters", s.tmdb_key_set ? "Key saved" : "Off",
       "A TMDB key (themoviedb.org) looks movies up"},
      {Item::tmdb});

  add({RowKind::header, "Destinations"}, {});
  for (std::size_t i = 0; i < s.destinations.size(); ++i) {
    const Destination &d = s.destinations[i];
    const DestType *t = a.catalog().dest_type(d.type);
    Row r{RowKind::action, d.name, t ? t->label : d.type, d.where};
    add(r, {Item::destination, static_cast<int>(i)});
  }
  add({RowKind::action, "Add a destination", "", "A folder, a network share, an FTP server or RomM"},
      {Item::add_destination});

  add({RowKind::header, "What each disc rips to"}, {});
  for (const System &sys : a.catalog().systems) {
    const auto found = s.profiles.find(sys.id);
    std::string value;
    if (found != s.profiles.end()) {
      const FormatInfo *f = sys.format(found->second.format);
      const Destination *d = s.destination(found->second.destination);
      value = (f ? f->label : found->second.format) + " to " + (d ? d->name : found->second.destination);
    }
    add({RowKind::action, sys.name, value}, {Item::profile, -1, sys.id});
  }

  // The drive test: discs one after another, for the drive report form.
  add({RowKind::header, "Drive test"}, {});
  const DriveTest &test = a.drive_test();
  if (test.active()) {
    Row r{RowKind::info, "Testing " + (test.drive_name.empty() ? test.drive_label : test.drive_name), "", test.prompt};
    if (test.state == "testing" && test.total > 0)
      r.progress = static_cast<float>(test.done / test.total);
    add(r, {});
    for (const Drive &d : a.drives())
      if (d.id == test.drive && d.has_disc && test.state == "waiting")
        add({RowKind::action, "Eject", "", "Then put in the next disc"}, {Item::test_eject, -1, d.id});
    add({RowKind::action, "Finish", "", "Pack the results to send"}, {Item::test_finish, -1, ""});
    if (test.discs.empty())
      add({RowKind::info, "Most wanted", "", "GameCube, Wii, Xbox 360 and Xbox discs, then Wii U, Dreamcast, CD games"},
          {});
  } else if (test.state == "done") {
    add({RowKind::info, "Results ready", "", "Scan the code on the right with a phone to send them"}, {});
  } else if (test.state == "failed") {
    add({RowKind::info, "The test went wrong", "", test.error}, {});
  }
  for (auto it = test.discs.rbegin(); it != test.discs.rend(); ++it) {
    Row r{RowKind::choice, "Disc " + std::to_string(it->n) + ": " + (it->kind.empty() ? "not recognized" : it->kind),
          it->said.empty() ? "What is it?" : it->said, it->outcome};
    if (it->result == "testing")
      r.detail = "Testing";
    add(r, {Item::test_disc, it->n, ""});
  }
  if (test.active()) {
    Row r{RowKind::action, discard_armed_ ? "Press again to throw it all away" : "Throw away", "",
          "Stop and delete this test"};
    r.danger = true;
    add(r, {Item::test_discard, -1, ""});
  } else {
    if (test.state == "done" || test.state == "failed")
      add({RowKind::action, "Close the test", "", "The zip stays in /data/ripper/diag"}, {Item::test_discard, -1, ""});
    for (const Drive &d : a.drives()) {
      Row r{RowKind::action, "Test " + d.model(), "",
            d.has_disc ? "Starts with the disc in it" : "Then put in discs, one after another"};
      r.enabled = d.state != "ripping";
      add(r, {Item::drive_test, -1, d.id});
    }
  }

  const Stats &st = a.stats();
  add({RowKind::header, "Ripped on this console"}, {});
  std::string top;
  for (std::size_t i = 0; i < st.systems.size() && i < 3; ++i) {
    const System *sys = a.catalog().system(st.systems[i].first);
    top += (top.empty() ? "" : ", ") + std::to_string(static_cast<int>(st.systems[i].second)) + " " +
           (sys ? sys->name : st.systems[i].first);
  }
  add({RowKind::info, "Discs", std::to_string(static_cast<int>(st.discs)), top}, {});
  add({RowKind::info, "Time spent ripping", duration(st.seconds)}, {});
  add({RowKind::info, "Read from discs", human_size(st.bytes)}, {});
  add({RowKind::info, "Sent to destinations", human_size(st.sent)}, {});
  if (st.failed > 0)
    add({RowKind::info, "Failed", std::to_string(static_cast<int>(st.failed))}, {});
}

void SettingsPage::destination_rows(std::vector<Row> &rows, std::vector<Item> &items) const {
  App &a = app();
  const DestType *t = a.catalog().dest_type(dest_.type);
  auto add = [&](Row r, Item i) {
    rows.push_back(std::move(r));
    items.push_back(std::move(i));
  };
  add({RowKind::header, dest_index_ < 0 ? "New destination" : dest_.name}, {});
  add({RowKind::action, "Name", dest_.name}, {Item::dest_name});
  if (dest_index_ < 0)
    add({RowKind::choice, "Kind", t ? t->label : dest_.type}, {Item::dest_type});
  else
    add({RowKind::info, "Kind", t ? t->label : dest_.type}, {});
  if (t)
    for (const Field &f : t->fields) {
      const auto v = dest_.values.find(f.id);
      const std::string value = v != dest_.values.end() ? v->second : f.def;
      if (f.type == "toggle") {
        add({RowKind::toggle, f.label, "", "", truthy(value)}, {Item::field, -1, f.id});
        continue;
      }
      Row r{RowKind::action, f.label};
      if (f.type == "secret") {
        const auto set = dest_.secret_set.find(f.id);
        r.value = !value.empty() ? "Typed" : set != dest_.secret_set.end() && set->second ? "Set" : "Not set";
        r.detail = f.placeholder;
      } else {
        r.value = value;
        r.detail = value.empty() ? f.placeholder : "";
        if (f.id == "url" && dest_.type == "romm" && a.rommps().found && value == a.rommps().url)
          r.detail = "From RommPS configure on this console";
      }
      add(r, {Item::field, -1, f.id});
    }
  if (dest_.type == "romm")
    add({RowKind::action, "Sign in to RomM", "", "Approve this console with a code, on your phone"}, {Item::pair});
  Row test{RowKind::action, "Test", tested_};
  add(test, {Item::test});
  add({RowKind::action, "Save"}, {Item::save});
  if (dest_index_ >= 0) {
    Row r{RowKind::action, armed_remove_ ? "Press again to remove" : "Remove"};
    r.danger = true;
    add(r, {Item::remove});
  }
}

void SettingsPage::profile_rows(std::vector<Row> &rows, std::vector<Item> &items) const {
  App &a = app();
  const System *sys = a.catalog().system(prof_system_);
  auto add = [&](Row r, Item i) {
    rows.push_back(std::move(r));
    items.push_back(std::move(i));
  };
  add({RowKind::header, sys ? sys->name : prof_system_}, {});
  const FormatInfo *f = sys ? sys->format(prof_.format) : nullptr;
  add({RowKind::choice, "Rips to", f ? f->label : prof_.format, f ? f->help : ""}, {Item::prof_format});
  const Destination *d = a.settings().destination(prof_.destination);
  add({RowKind::choice, "Saves to", d ? d->name : prof_.destination, d ? d->where : ""}, {Item::prof_destination});
  add({RowKind::action, "Folder", prof_.folder, "{system}, {name} and {artist} are filled in"}, {Item::prof_folder});
  if (f && !f->options.empty()) {
    add({RowKind::header, "Options"}, {});
    for (const Option &o : f->options) {
      const auto v = prof_.options.find(o.id);
      const std::string value = v != prof_.options.end() ? v->second : o.def;
      if (o.type == "toggle")
        add({RowKind::toggle, o.label, "", "", truthy(value)}, {Item::prof_option, -1, o.id});
      else
        add({RowKind::choice, o.label, choice_label(o, value)}, {Item::prof_option, -1, o.id});
    }
  }
  add({RowKind::action, "Save"}, {Item::save});
}

void SettingsPage::open_destination(int index, const std::string &type) {
  const Settings &s = app().settings();
  dest_index_ = index;
  tested_.clear();
  armed_remove_ = 0;
  if (index >= 0 && index < static_cast<int>(s.destinations.size()))
    dest_ = s.destinations[static_cast<std::size_t>(index)];
  else {
    dest_ = Destination{};
    dest_.type = type;
    dest_.name = "";
    if (const DestType *t = app().catalog().dest_type(type))
      for (const Field &f : t->fields)
        dest_.values[f.id] = f.def;
    const RommPS &rp = app().rommps();
    if (type == "romm" && rp.found) {
      dest_.values["url"] = rp.url;
      dest_.values["tls_verify"] = rp.tls_verify ? "1" : "0";
    }
  }
  // Secrets come back blank: typing one replaces it, leaving it keeps it.
  if (const DestType *t = app().catalog().dest_type(dest_.type))
    for (const Field &f : t->fields)
      if (f.type == "secret")
        dest_.values[f.id] = "";
  mode_ = kDestination;
  list_.set_focus(1);
}

void SettingsPage::open_profile(const std::string &system) {
  const auto found = app().settings().profiles.find(system);
  prof_system_ = system;
  prof_ = found != app().settings().profiles.end() ? found->second : Profile{};
  mode_ = kProfile;
  list_.set_focus(1);
}

void SettingsPage::back(ui::Feedback &feedback) {
  feedback.play(Cue::back);
  if (mode_ == kPairing) {
    app().pair_cancel();
    mode_ = kDestination;
    return;
  }
  mode_ = kMain;
  list_.set_focus(main_focus_);
}

void SettingsPage::update(const hui::InputFrame &input, float dt, ui::Feedback &feedback) {
  App &a = app();
  const Look &l = look(current_look());
  if (entry_.active()) {
    const int r = entry_.update(input, dt, feedback, l);
    if (r == 1) {
      const std::string text = entry_.text();
      if (editing_.kind == Item::dest_name)
        dest_.name = text;
      else if (editing_.kind == Item::field)
        dest_.values[editing_.key] = text;
      else if (editing_.kind == Item::prof_folder)
        prof_.folder = text;
      else if (editing_.kind == Item::tmdb) {
        const bool set = !text.empty();
        a.set_settings("\"tmdb_key\":" + json_string(text), [set](Settings &x) { x.tmdb_key_set = set; });
      }
      feedback.play(Cue::select);
    }
    return;
  }
  if (mode_ == kPairing) {
    const Pairing &p = a.pairing();
    if (p.status == "ok" && !p.token.empty()) {
      dest_.values["token"] = p.token;
      dest_.secret_set["token"] = true;
      tested_ = "Signed in";
      mode_ = kDestination;
      feedback.play(Cue::saved);
      a.pair_cancel();  // the token's taken: forget it there
    } else if (input.is_pressed(Action::back))
      back(feedback);
    return;
  }
  if (mode_ != kMain && input.is_pressed(Action::back)) {
    back(feedback);
    return;
  }
  std::vector<Item> items;
  const std::vector<Row> rws = rows(&items);
  const RowEvent e = list_.handle(rws, input, feedback);
  if (mode_ == kMain)
    main_focus_ = list_.focus();
  if (e.row >= 0 && e.row < static_cast<int>(items.size()))
    activate(items[static_cast<std::size_t>(e.row)], e.activated ? 0 : e.step, feedback);
  if (input.nav != hui::Direction::none)
    armed_remove_ = 0;  // moving off Remove disarms it
  list_.update(dt, static_cast<int>(rows(nullptr).size()), kListHeight);
}

void SettingsPage::activate(const Item &item, int step, ui::Feedback &feedback) {
  App &a = app();
  const Settings &s = a.settings();
  if (item.kind != Item::remove)
    armed_remove_ = 0;
  switch (item.kind) {
  case Item::look:
    set_look((current_look() + (step ? step : 1) + look_count()) % look_count());
    break;
  case Item::sounds:
    set_ui_sounds(!ui_sounds());
    break;
  case Item::notify: {
    const bool on = !s.notify;
    a.set_settings(std::string("\"notify\":") + (on ? "true" : "false"), [on](Settings &x) { x.notify = on; });
    break;
  }
  case Item::eject: {
    const bool on = !s.auto_eject;
    a.set_settings(std::string("\"auto_eject\":") + (on ? "true" : "false"), [on](Settings &x) { x.auto_eject = on; });
    break;
  }
  case Item::auto_start: {
    const bool on = !s.auto_start;
    a.set_settings(std::string("\"auto_start\":") + (on ? "true" : "false"), [on](Settings &x) { x.auto_start = on; });
    break;
  }
  case Item::keep_front: {
    const bool on = !s.keep_front;
    a.set_settings(std::string("\"keep_front\":") + (on ? "true" : "false"), [on](Settings &x) { x.keep_front = on; });
    break;
  }
  case Item::retries: {
    const int n = (s.read_retries + (step ? step : 1) + 11) % 11;  // 0 to 10
    a.set_settings("\"read_retries\":" + std::to_string(n), [n](Settings &x) { x.read_retries = n; });
    break;
  }
  case Item::awake:
    awake_ = (awake_ + (step ? step : 1) + 4) % 4;
    a.keep_awake(kAwakeMinutes[awake_]);
    break;
  case Item::drive_test:
    if (a.drive_test().active())
      list_.refuse(feedback);
    else
      a.start_drive_test(item.key);
    break;
  case Item::test_eject:
    a.eject(item.key);
    break;
  case Item::test_finish:
    a.finish_drive_test();
    break;
  case Item::test_discard:
    if (a.drive_test().active() && !discard_armed_) {
      discard_armed_ = true;
      break;
    }
    discard_armed_ = false;
    a.discard_drive_test();
    break;
  case Item::test_disc: {
    // What it really is: left and right step through the catalog's systems.
    std::vector<std::string> names{""};
    for (const System &sys : a.catalog().systems)
      names.push_back(sys.name);
    names.push_back("Not sure");
    names.push_back("Something else");
    std::string said;
    for (const TestDisc &d : a.drive_test().discs)
      if (d.n == item.index)
        said = d.said;
    const int n = static_cast<int>(names.size());
    int at = 0;
    for (int i = 0; i < n; ++i)
      if (names[static_cast<std::size_t>(i)] == said)
        at = i;
    at = ((at + (step ? step : 1)) % n + n) % n;
    a.label_test_disc(item.index, names[static_cast<std::size_t>(at)]);
    break;
  }
  case Item::tmdb:
    editing_ = item;
    entry_.open("TMDB key", "", true, "An API key or read token from themoviedb.org; empty turns it off");
    break;
  case Item::destination:
    open_destination(item.index, "");
    feedback.play(Cue::open);
    break;
  case Item::add_destination:
    open_destination(-1, "smb");
    feedback.play(Cue::open);
    break;
  case Item::profile:
    open_profile(item.key);
    feedback.play(Cue::open);
    break;
  case Item::dest_name:
    editing_ = item;
    entry_.open("Name", dest_.name);
    break;
  case Item::dest_type: {
    const auto &types = a.catalog().dest_types;
    if (types.empty())
      break;
    int at = 0;
    for (std::size_t i = 0; i < types.size(); ++i)
      if (types[i].type == dest_.type)
        at = static_cast<int>(i);
    const int n = static_cast<int>(types.size());
    open_destination(-1, types[static_cast<std::size_t>(((at + (step ? step : 1)) % n + n) % n)].type);
    list_.set_focus(2);
    break;
  }
  case Item::field: {
    const DestType *t = a.catalog().dest_type(dest_.type);
    const Field *f = nullptr;
    if (t)
      for (const Field &x : t->fields)
        if (x.id == item.key)
          f = &x;
    if (!f)
      break;
    if (f->type == "toggle")
      dest_.values[f->id] = truthy(dest_.values[f->id]) ? "0" : "1";
    else {
      editing_ = item;
      entry_.open(f->label, f->type == "secret" ? "" : dest_.values[f->id], f->type == "secret",
                  f->type == "secret" ? "Left empty, the one saved stays" : f->placeholder);
    }
    break;
  }
  case Item::pair: {
    const std::string url = dest_.values["url"];
    if (url.empty()) {
      tested_ = "Type the server's address first";
      list_.refuse(feedback);
      break;
    }
    a.pair_start(url, truthy(dest_.values.count("tls_verify") ? dest_.values["tls_verify"] : "1"));
    mode_ = kPairing;
    feedback.play(Cue::launch);
    break;
  }
  case Item::test:
    tested_ = "Testing...";
    a.test_destination(dest_, [this](bool ok, const std::string &what) {
      tested_ = what;
      test_ok_ = ok;
    });
    break;
  case Item::save:
    if (mode_ == kProfile) {
      a.save_profile(prof_system_, prof_);
      mode_ = kMain;
      list_.set_focus(main_focus_);
      feedback.play(Cue::saved);
      break;
    }
    if (dest_.name.empty()) {
      tested_ = "Give it a name first";
      list_.refuse(feedback);
      break;
    }
    {
      std::vector<Destination> all = s.destinations;
      if (dest_index_ >= 0 && dest_index_ < static_cast<int>(all.size()))
        all[static_cast<std::size_t>(dest_index_)] = dest_;
      else {
        dest_.id = new_id(dest_.name, all);
        all.push_back(dest_);
      }
      a.save_destinations(all, [this](bool ok) {
        if (ok) {
          mode_ = kMain;
          list_.set_focus(main_focus_);
        }
      });
    }
    feedback.play(Cue::saved);
    break;
  case Item::remove:
    if (!armed_remove_) {
      armed_remove_ = 1;
      feedback.play(Cue::tick);
      break;
    }
    {
      std::vector<Destination> all = s.destinations;
      if (dest_index_ >= 0 && dest_index_ < static_cast<int>(all.size()))
        all.erase(all.begin() + dest_index_);
      a.save_destinations(all, [this](bool ok) {
        if (ok) {
          mode_ = kMain;
          list_.set_focus(main_focus_);
        }
      });
      armed_remove_ = 0;
      feedback.play(Cue::back);
    }
    break;
  case Item::prof_format: {
    const System *sys = a.catalog().system(prof_system_);
    if (!sys || sys->formats.empty())
      break;
    std::vector<const FormatInfo *> usable;
    for (const FormatInfo &f : sys->formats)
      if (!f.planned)
        usable.push_back(&f);
    if (usable.empty())
      break;
    int at = 0;
    for (std::size_t i = 0; i < usable.size(); ++i)
      if (usable[i]->id == prof_.format)
        at = static_cast<int>(i);
    const int n = static_cast<int>(usable.size());
    prof_.format = usable[static_cast<std::size_t>(((at + (step ? step : 1)) % n + n) % n)]->id;
    prof_.options.clear();  // the new format's own defaults
    break;
  }
  case Item::prof_destination: {
    const auto &dests = s.destinations;
    if (dests.empty())
      break;
    int at = 0;
    for (std::size_t i = 0; i < dests.size(); ++i)
      if (dests[i].id == prof_.destination)
        at = static_cast<int>(i);
    const int n = static_cast<int>(dests.size());
    prof_.destination = dests[static_cast<std::size_t>(((at + (step ? step : 1)) % n + n) % n)].id;
    break;
  }
  case Item::prof_folder:
    editing_ = item;
    entry_.open("Folder", prof_.folder, false, "{system}, {name} and {artist} are filled in");
    break;
  case Item::prof_option: {
    const System *sys = a.catalog().system(prof_system_);
    const FormatInfo *f = sys ? sys->format(prof_.format) : nullptr;
    if (!f)
      break;
    for (const Option &o : f->options)
      if (o.id == item.key) {
        const auto v = prof_.options.find(o.id);
        const std::string cur = v != prof_.options.end() ? v->second : o.def;
        prof_.options[o.id] = o.type == "toggle" ? (truthy(cur) ? "0" : "1") : step_choice(o, cur, step ? step : 1);
      }
    break;
  }
  case Item::none:
    break;
  }
}

void SettingsPage::draw(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, float clock,
                        float age) const {
  const float in = hui::tween::stagger(age, 1, 0.08f, 0.6f);
  list.push_opacity(in);
  const char *title = mode_ == kMain ? "Settings" : mode_ == kProfile ? "Profile" : "Destination";
  ui::text(list, fonts.display, title, kMargin, 240, 64, look.text);
  list_.draw(list, fonts, look, rows(nullptr), {kMargin, 290 + 24 * (1.0f - in), 1060, kListHeight}, clock);
  if (mode_ == kPairing)
    draw_pairing(list, fonts, look);
  else
    draw_side(list, fonts, look);
  list.pop_opacity();
}

void SettingsPage::draw_overlay(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, float clock) const {
  entry_.draw(list, fonts, look, clock);
}

// Beside the list: the web UI's address (and its QR code), or about the page.
void SettingsPage::draw_side(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look) const {
  const App &a = app();
  const Status &s = a.status();
  draw_panel(list, look, kSide);
  const float x = kSide.x + 44;
  float y = kSide.y + 64;
  if (mode_ == kDestination) {
    const DestType *t = a.catalog().dest_type(dest_.type);
    ui::text(list, fonts.semibold, "DESTINATION", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(t ? t->label : dest_.type, 30, kSide.w - 88), x, y + 44, 30,
             look.panel_text);
    if (t && !t->help.empty())
      ui::paragraph(list, fonts.regular, t->help, x, y + 96, 22, kSide.w - 88, 32, look.panel_muted, 4);
    if (!tested_.empty())
      ui::paragraph(list, fonts.regular, tested_, x, y + 220, 22, kSide.w - 88, 32,
                    test_ok_ ? look.success : look.panel_text, 6);
    return;
  }
  if (mode_ == kProfile) {
    const System *sys = a.catalog().system(prof_system_);
    ui::text(list, fonts.semibold, "PROFILE", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(sys ? sys->name : prof_system_, 30, kSide.w - 88), x,
             y + 44, 30, look.panel_text);
    ui::paragraph(list, fonts.regular, "What these discs rip to. The confirm sheet starts from it.", x, y + 96, 22,
                  kSide.w - 88, 32, look.panel_muted, 3);
    return;
  }
  // A drive test: what to do now, and a code for a phone, where the zip is
  // downloaded and the report form opened.
  const DriveTest &t = a.drive_test();
  if (!t.state.empty() && t.state != "idle") {
    ui::text(list, fonts.semibold, "DRIVE TEST", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.semibold,
             fonts.semibold.font->fit(t.drive_name.empty() ? t.drive_label : t.drive_name, 26, kSide.w - 88), x,
             y + 40, 26, look.panel_text);
    ui::paragraph(list, fonts.regular, t.state == "failed" && !t.error.empty() ? t.error : t.prompt, x, y + 84, 22,
                  kSide.w - 88, 30, t.state == "failed" ? look.danger : look.panel_text, 3);
    if (!s.ip.empty()) {
      const std::string link = "http://" + s.ip + ":" + std::to_string(s.port) + "/#drivetest";
      if (qr_for_ != link) {
        qr_for_ = link;
        qr_ = QrCode::encode(link);
      }
      draw_qr(list, qr_, {x, y + 184, 200, 200});
      ui::paragraph(list, fonts.regular,
                    t.state == "done" ? "Scan this with a phone to download the zip and open the drive report form."
                                      : "On a phone: each disc's details, and the discs that help most.",
                    x, y + 400, 18, kSide.w - 88, 24, look.panel_muted, 3);
    }
    return;
  }
  const std::string url = s.ip.empty() ? "" : "http://" + s.ip + ":" + std::to_string(s.port);
  ui::text(list, fonts.semibold, "WEB UI", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
  ui::text(list, fonts.semibold, fonts.semibold.font->fit(url.empty() ? "-" : url, 26, kSide.w - 88), x, y + 40, 26,
           look.panel_text);
  if (!url.empty()) {
    if (qr_for_ != url) {
      qr_for_ = url;
      qr_ = QrCode::encode(url);
    }
    draw_qr(list, qr_, {x, y + 76, 240, 240});
    ui::text(list, fonts.regular, "Disc reports: Log, then Download reports", x, y + 340, 18, look.panel_muted);
  }
  ui::text(list, fonts.semibold, "VERSION", x, y + 380, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
  ui::text(list, fonts.semibold, s.version + (s.build.empty() || s.build == "dev" ? "" : " (" + s.build + ")"), x,
           y + 416, 24, look.panel_text);
  ui::text(list, fonts.regular, "Running for " + duration(s.uptime), x, y + 452, 20, look.panel_muted);
}

// RomM sign-in: the code, the address to enter it at, and a QR code for a phone.
void SettingsPage::draw_pairing(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look) const {
  const Pairing &p = app().pairing();
  draw_panel(list, look, kSide);
  const float x = kSide.x + 44;
  const float y = kSide.y + 64;
  ui::text(list, fonts.semibold, "SIGN IN TO ROMM", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
  if (p.status == "error") {
    ui::paragraph(list, fonts.regular, p.message.empty() ? "RomM didn't answer." : p.message, x, y + 50, 24,
                  kSide.w - 88, 34, look.danger, 6);
    return;
  }
  if (p.user_code.empty()) {
    ui::text(list, fonts.regular, "Asking RomM for a code", x, y + 50, 24, look.panel_text);
    return;
  }
  ui::paragraph(list, fonts.regular, "On your phone or computer, open this and enter the code:", x, y + 46, 22,
                kSide.w - 88, 30, look.panel_muted, 2);
  ui::text(list, fonts.semibold, fonts.semibold.font->fit(p.verification_url, 22, kSide.w - 88), x, y + 126, 22,
           look.panel_text);
  ui::text(list, fonts.display, p.user_code, x, y + 206, 56, look.accent);
  if (qr_for_ != p.verification_url) {
    qr_for_ = p.verification_url;
    qr_ = QrCode::encode(p.verification_url);
  }
  draw_qr(list, qr_, {x, y + 240, 220, 220});
}

std::vector<ui::Hint> SettingsPage::hints() const {
  if (entry_.active())
    return entry_.hints();
  if (mode_ == kPairing)
    return {{ui::Button::circle, "Cancel"}};
  std::vector<ui::Hint> hints;
  if (mode_ == kMain)
    hints.push_back({ui::Button::l1, "Pages", ui::Button::r1});
  std::vector<Item> items;
  const std::vector<Row> rws = rows(&items);
  const int focus = list_.focus();
  if (focus >= 0 && focus < static_cast<int>(rws.size())) {
    const Row &r = rws[static_cast<std::size_t>(focus)];
    if (r.kind == RowKind::choice)
      hints.push_back({ui::Button::dpad, "Change"});
    else if (r.kind == RowKind::toggle)
      hints.push_back({ui::Button::cross, r.on ? "Turn off" : "Turn on"});
    else if (r.kind == RowKind::action)
      hints.push_back({ui::Button::cross, "Choose"});
  }
  if (mode_ != kMain)
    hints.push_back({ui::Button::circle, "Back"});
  return hints;
}

}  // namespace ripper
