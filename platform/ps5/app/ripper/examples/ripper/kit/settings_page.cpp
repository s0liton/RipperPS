// RipperPS: the Settings tab. See settings_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_page.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "avatar_png.h"
#include "cJSON.h"
#include "core/tween.hpp"
#include "stb_image.h"  // implemented by base/VulkanglTFModel.cpp

namespace ripper {

namespace ui = hui::ui;
using hui::Action;
using hui::audio::Cue;
using hui::gfx::Color;
using hui::gfx::Rect;

namespace {

constexpr float kMargin = 96.0f;
constexpr float kListHeight = 7 * RowList::kRowHeight;
const Rect kSide{1240, 290, 584, kListHeight};  // beside the list, clear of the hints
// The credits' QR code: where RipperPS's source and its issue forms are.
constexpr const char *kSourceUrl = "https://github.com/s0liton/RipperPS";


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

// "/data/ripper/rips" -> "/data/ripper"; a storage root's own parent is the list of roots ("").
std::string parent_of(const std::string &path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos || slash == 0 ? "" : path.substr(0, slash);
}

// The author's portrait, as a texture once the renderer is there (0 before).
std::uint32_t avatar_texture() {
  static std::uint32_t texture = 0;
  static bool tried = false;
  auto *renderer = static_cast<hui::gfx::Renderer *>(ripper_renderer());
  if (texture || tried || !renderer)
    return texture;
  tried = true;
  int w = 0, h = 0, channels = 0;
  unsigned char *rgba = stbi_load_from_memory(kAvatarPng, static_cast<int>(sizeof kAvatarPng), &w, &h, &channels, 4);
  if (rgba) {
    texture = renderer->create_texture(w, h, rgba);
    stbi_image_free(rgba);
  }
  return texture;
}

// 4447 -> "4,447".
std::string grouped(int n) {
  std::string s = std::to_string(n);
  for (int at = static_cast<int>(s.size()) - 3; at > 0; at -= 3)
    s.insert(static_cast<std::size_t>(at), ",");
  return s;
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
  if (mode_ == kFolder)
    folder_rows(rows, it);
  else if (mode_ == kDestination || mode_ == kPairing)
    destination_rows(rows, it);
  else if (mode_ == kProfiles)
    profiles_rows(rows, it);
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
  // Ripping, where rips go, the keys some discs need, the console's own
  // settings, then the drive test, which is used now and then.
  add({RowKind::header, "Ripping"}, {});
  {
    Row r{RowKind::action, "Default Rip Settings"};
    r.expander = 1;
    add(r, {Item::profiles});
  }
  add({RowKind::toggle, "Rip Automatically", "", "", s.auto_start}, {Item::auto_start});
  add({RowKind::toggle, "Eject When Done", "", "", s.auto_eject}, {Item::eject});
  add({RowKind::choice, "Retries for a Bad Sector", std::to_string(s.read_retries)}, {Item::retries});
  add({RowKind::action, "Movie Names and Posters", s.tmdb_key_set ? "TMDB" : "Add a TMDB API Key"}, {Item::tmdb});

  add({RowKind::header, "Destinations"}, {});
  for (std::size_t i = 0; i < s.destinations.size(); ++i) {
    const Destination &d = s.destinations[i];
    const DestType *t = a.catalog().dest_type(d.type);
    add({RowKind::action, d.name, !d.where.empty() ? d.where : t ? t->label : d.type},
        {Item::destination, static_cast<int>(i)});
  }
  add({RowKind::action, "Add a Destination"}, {Item::add_destination});

  if (!s.key_files.empty()) {
    add({RowKind::header, "Keys"}, {});
    for (std::size_t i = 0; i < s.key_files.size(); ++i) {
      const KeyFile &k = s.key_files[i];
      const std::string status = !k.found        ? "Not Found"
                                 : k.count == 1 ? "1 Key"
                                                : grouped(k.count) + " Keys";
      add({RowKind::action, k.label, status}, {Item::key_file, static_cast<int>(i)});
    }
  }

  const Look &l = look(current_look());
  add({RowKind::header, "Console"}, {});
  add({RowKind::toggle, "Notifications", "", "", s.notify}, {Item::notify});
  add({RowKind::choice, "Theme", l.name}, {Item::look});
  add({RowKind::toggle, "Interface Sounds", "", "", ui_sounds()}, {Item::sounds});

  // The drive test: discs one after another, for the drive report form.
  add({RowKind::header, "Drive Test"}, {});
  const DriveTest &test = a.drive_test();
  if (test.active()) {
    Row r{RowKind::info, "Testing " + (test.drive_name.empty() ? test.drive_label : test.drive_name), "", test.prompt};
    if (test.state == "testing" && test.total > 0)
      r.progress = static_cast<float>(test.done / test.total);
    add(r, {});
    for (const Drive &d : a.drives())
      if (d.id == test.drive && d.has_disc && test.state == "waiting")
        add({RowKind::action, "Eject"}, {Item::test_eject, -1, d.id});
    add({RowKind::action, "Finish"}, {Item::test_finish, -1, ""});
    if (test.discs.empty())
      add({RowKind::info, "Most Wanted", "", "GameCube, Wii, Xbox 360 and Xbox discs, then Wii U, 4K movies, Dreamcast, CD games"},
          {});
  } else if (test.state == "done") {
    add({RowKind::info, "Results Ready", "", "Scan the code on the right with a phone to send them"}, {});
  } else if (test.state == "failed") {
    add({RowKind::info, "The Test Went Wrong", "", test.error}, {});
  }
  for (auto it = test.discs.rbegin(); it != test.discs.rend(); ++it) {
    Row r{RowKind::choice, "Disc " + std::to_string(it->n) + ": " + (it->kind.empty() ? "not recognized" : it->kind),
          it->said.empty() ? "What is it?" : it->said, it->outcome};
    if (it->result == "testing")
      r.detail = "Testing";
    add(r, {Item::test_disc, it->n, ""});
  }
  if (test.active()) {
    Row r{RowKind::action, discard_armed_ ? "Press Again to Throw It All Away" : "Throw Away"};
    r.danger = true;
    add(r, {Item::test_discard, -1, ""});
  } else {
    if (test.state == "done" || test.state == "failed")
      add({RowKind::action, "Close the Test", "", "The zip stays in /data/ripper/diag"}, {Item::test_discard, -1, ""});
    for (const Drive &d : a.drives()) {
      Row r{RowKind::action, "Test " + d.model()};
      r.enabled = d.state != "ripping";
      add(r, {Item::drive_test, -1, d.id});
    }
  }

}

// Each kind of disc and what it rips to, by group.
void SettingsPage::profiles_rows(std::vector<Row> &rows, std::vector<Item> &items) const {
  App &a = app();
  const Settings &s = a.settings();
  auto add = [&](Row r, Item i) {
    rows.push_back(std::move(r));
    items.push_back(std::move(i));
  };
  static const char *const kGroups[][2] = {{"games", "Games"}, {"movies", "Movies"}, {"music", "Music"}, {"other", "Other"}};
  for (const auto &g : kGroups) {
    bool header = false;
    for (const System &sys : a.catalog().systems) {
      if (sys.group != g[0])
        continue;
      if (!header) {
        add({RowKind::header, g[1]}, {});
        header = true;
      }
      const auto found = s.profiles.find(sys.id);
      std::string value;
      if (found != s.profiles.end()) {
        const FormatInfo *f = sys.format(found->second.format);
        const Destination *d = s.destination(found->second.destination);
        value = (f ? f->label : found->second.format) + " to " + (d ? d->name : found->second.destination);
      }
      add({RowKind::action, sys.name, value}, {Item::profile, -1, sys.id});
    }
  }
}

void SettingsPage::destination_rows(std::vector<Row> &rows, std::vector<Item> &items) const {
  App &a = app();
  const DestType *t = a.catalog().dest_type(dest_.type);
  auto add = [&](Row r, Item i) {
    rows.push_back(std::move(r));
    items.push_back(std::move(i));
  };
  add({RowKind::header, dest_index_ < 0 ? "New Destination" : dest_.name}, {});
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
      if (f.type == "folder") {
        r.value = value;
      } else if (f.type == "secret") {
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
    add({RowKind::action, "Sign In to RomM"}, {Item::pair});
  Row test{RowKind::action, "Test", tested_};
  add(test, {Item::test});
  add({RowKind::action, "Save"}, {Item::save});
  if (dest_index_ >= 0) {
    Row r{RowKind::action, armed_remove_ ? "Press Again to Remove" : "Remove"};
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
  add({RowKind::choice, "Rips To", f ? f->label : prof_.format, f ? f->help : ""}, {Item::prof_format});
  const Destination *d = a.settings().destination(prof_.destination);
  add({RowKind::choice, "Saves To", d ? d->name : prof_.destination, d ? d->where : ""}, {Item::prof_destination});
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

void SettingsPage::folder_rows(std::vector<Row> &rows, std::vector<Item> &items) const {
  auto add = [&](Row r, Item i) {
    rows.push_back(std::move(r));
    items.push_back(std::move(i));
  };
  const bool usb = key_index_ < 0 && dest_.type == "usb";
  const KeyFile *key = key_index_ >= 0 && key_index_ < static_cast<int>(app().settings().key_files.size())
                           ? &app().settings().key_files[static_cast<std::size_t>(key_index_)]
                           : nullptr;
  if (key && key->path != key->def)
    add({RowKind::action, "Use the Default"}, {Item::key_default});
  // Headers are drawn in capitals: the path itself goes beside the list.
  add({RowKind::header, folder_path_.empty() ? (usb ? "USB drives" : "Storage") : "Folders"}, {});
  if (!folder_path_.empty()) {
    if (key_index_ < 0) {
      add({RowKind::action, "Use This Folder"}, {Item::folder_use});
      add({RowKind::action, "New Folder Here"}, {Item::folder_new});
    }
    add({RowKind::action, "Up"}, {Item::folder_up});
  }
  if (folder_loading_ && folders_.empty()) {
    add({RowKind::info, "Looking..."}, {});
    return;
  }
  if (!folder_error_.empty())
    add({RowKind::info, folder_error_}, {});
  if (folders_.empty() && folder_path_.empty())
    add({RowKind::info, usb ? "No USB drive is plugged in" : "Nothing to pick from"}, {});
  if (folder_path_.empty())
    add({RowKind::action, "Look Again"}, {Item::folder_up});
  for (std::size_t i = 0; i < folders_.size(); ++i) {
    Row r{RowKind::action, folders_[i].name};
    r.expander = 1;
    add(r, {Item::folder_open, static_cast<int>(i)});
  }
  if (!files_.empty()) {
    add({RowKind::header, "Files"}, {});
    for (std::size_t i = 0; i < files_.size(); ++i)
      add({RowKind::action, files_[i].name}, {Item::file_pick, static_cast<int>(i)});
  }
}

void SettingsPage::open_key_file(int index) {
  const std::vector<KeyFile> &keys = app().settings().key_files;
  if (index < 0 || index >= static_cast<int>(keys.size()))
    return;
  key_index_ = index;
  folders_.clear();
  files_.clear();
  folder_error_.clear();
  folder_path_.clear();
  mode_ = kFolder;
  list_.set_focus(1);
  list_folder(parent_of(keys[static_cast<std::size_t>(index)].path));
}

void SettingsPage::open_folders(const std::string &field) {
  key_index_ = -1;
  files_.clear();
  folder_field_ = field;
  folders_.clear();
  folder_error_.clear();
  folder_path_.clear();
  mode_ = kFolder;
  list_.set_focus(1);
  list_folder(dest_.values[field]);
}

// Lists a folder; one that isn't there (yet) lists the nearest that is.
void SettingsPage::list_folder(const std::string &path) {
  const int seq = ++folder_seq_;
  const std::string scope = key_index_ >= 0 ? "" : dest_.type == "usb" ? "usb" : "local";
  folder_loading_ = true;
  std::string q;
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.') {
      q += static_cast<char>(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", c);
      q += hex;
    }
  }
  app().get("/api/folders?scope=" + scope + (key_index_ >= 0 ? "&files=1" : "") + "&path=" + q, [this, seq, path](
                                                                                                   const Response &r) {
    if (seq != folder_seq_)
      return;
    cJSON *j = r.ok() ? r.json() : nullptr;
    if (!j) {
      if (!path.empty()) {
        list_folder(parent_of(path));
        return;
      }
      folder_loading_ = false;
      folder_error_ = r.message().empty() ? "Couldn't list the folders" : r.message();
      return;
    }
    folder_loading_ = false;
    folder_error_.clear();
    folders_.clear();
    files_.clear();
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "path");
    const cJSON *up = cJSON_GetObjectItemCaseSensitive(j, "parent");
    folder_path_ = cJSON_IsString(p) ? p->valuestring : "";
    folder_parent_ = cJSON_IsString(up) ? up->valuestring : "";
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(j, "folders")) {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(f, "name");
      const cJSON *at = cJSON_GetObjectItemCaseSensitive(f, "path");
      if (cJSON_IsString(name) && cJSON_IsString(at))
        folders_.push_back({plain_text(name->valuestring), at->valuestring});
    }
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(j, "files")) {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(f, "name");
      const cJSON *at = cJSON_GetObjectItemCaseSensitive(f, "path");
      if (cJSON_IsString(name) && cJSON_IsString(at))
        files_.push_back({plain_text(name->valuestring), at->valuestring});
    }
    auto by_name = [](const Folder &a, const Folder &b) {
      std::string x = a.name, y = b.name;
      std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return std::tolower(c); });
      std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return std::tolower(c); });
      return x < y;
    };
    std::sort(folders_.begin(), folders_.end(), by_name);
    std::sort(files_.begin(), files_.end(), by_name);
    cJSON_Delete(j);
    list_.set_focus(1);
  });
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
  if (mode_ == kFolder) {
    ++folder_seq_;  // an answer still coming is dropped
    if (key_index_ >= 0) {
      key_index_ = -1;
      mode_ = kMain;
      list_.set_focus(main_focus_);
    } else {
      mode_ = kDestination;
    }
    return;
  }
  if (mode_ == kProfile) {
    mode_ = kProfiles;
    list_.set_focus(profiles_focus_);
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
      else if (editing_.kind == Item::folder_new && !text.empty() && text.find('/') == std::string::npos &&
               text != "." && text != "..") {
        dest_.values[folder_field_] = folder_path_ + "/" + text;
        ++folder_seq_;
        mode_ = kDestination;
      }
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
  else if (mode_ == kProfiles)
    profiles_focus_ = list_.focus();
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
  case Item::retries: {
    const int n = (s.read_retries + (step ? step : 1) + 11) % 11;  // 0 to 10
    a.set_settings("\"read_retries\":" + std::to_string(n), [n](Settings &x) { x.read_retries = n; });
    break;
  }
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
    entry_.open("TMDB key", "", true, "For the best matches: a free themoviedb.org key. Empty: Wikipedia, best effort");
    break;
  case Item::destination:
    open_destination(item.index, "");
    feedback.play(Cue::open);
    break;
  case Item::add_destination:
    open_destination(-1, "smb");
    feedback.play(Cue::open);
    break;
  case Item::profiles:
    mode_ = kProfiles;
    list_.set_focus(profiles_focus_);
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
    else if (f->type == "folder") {
      open_folders(f->id);
      feedback.play(Cue::open);
    } else {
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
      mode_ = kProfiles;
      list_.set_focus(profiles_focus_);
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
  case Item::folder_open:
    if (item.index >= 0 && item.index < static_cast<int>(folders_.size()))
      list_folder(folders_[static_cast<std::size_t>(item.index)].path);
    feedback.play(Cue::open);
    break;
  case Item::folder_up:
    list_folder(folder_path_.empty() ? "" : folder_parent_);
    feedback.play(Cue::back);
    break;
  case Item::folder_use:
    dest_.values[folder_field_] = folder_path_;
    ++folder_seq_;
    mode_ = kDestination;
    feedback.play(Cue::select);
    break;
  case Item::key_file:
    main_focus_ = list_.focus();
    open_key_file(item.index);
    feedback.play(Cue::open);
    break;
  case Item::file_pick:
  case Item::key_default: {
    const std::vector<KeyFile> &keys = app().settings().key_files;
    if (key_index_ >= 0 && key_index_ < static_cast<int>(keys.size())) {
      const bool file = item.kind == Item::file_pick && item.index >= 0 && item.index < static_cast<int>(files_.size());
      app().set_key_file(keys[static_cast<std::size_t>(key_index_)].id,
                         file ? files_[static_cast<std::size_t>(item.index)].path : "");
    }
    ++folder_seq_;
    key_index_ = -1;
    mode_ = kMain;
    list_.set_focus(main_focus_);
    feedback.play(Cue::select);
    break;
  }
  case Item::folder_new:
    editing_ = item;
    entry_.open("New folder", "", false, "Its name, in " + folder_path_);
    break;
  case Item::none:
    break;
  }
}

void SettingsPage::draw(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, float clock,
                        float age) const {
  const float in = hui::tween::stagger(age, 1, 0.08f, 0.6f);
  list.push_opacity(in);
  const char *title = mode_ == kMain      ? "Settings"
                      : mode_ == kProfiles ? "Default Rip Settings"
                      : mode_ == kProfile ? "Default Rip Settings"
                      : mode_ == kFolder  ? (key_index_ >= 0 ? "Choose a File" : "Choose a Folder")
                                          : "Destination";
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
    ui::text(list, fonts.semibold, "DEFAULTS FOR", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(sys ? sys->name : prof_system_, 30, kSide.w - 88), x,
             y + 44, 30, look.panel_text);
    return;
  }
  if (mode_ == kFolder && key_index_ >= 0 && key_index_ < static_cast<int>(a.settings().key_files.size())) {
    const KeyFile &k = a.settings().key_files[static_cast<std::size_t>(key_index_)];
    ui::text(list, fonts.semibold, "FILE FOR", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(k.label, 30, kSide.w - 88), x, y + 44, 30, look.panel_text);
    ui::text(list, fonts.semibold, "NOW", x, y + 120, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::paragraph(list, fonts.regular, plain_text(k.path), x, y + 156, 24, kSide.w - 88, 32, look.panel_text, 3);
    ui::text(list, fonts.semibold, "IN", x, y + 280, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::paragraph(list, fonts.regular, folder_path_.empty() ? "-" : plain_text(folder_path_), x, y + 316, 24,
                  kSide.w - 88, 32, look.panel_text, 3);
    return;
  }
  if (mode_ == kFolder) {
    const DestType *t = a.catalog().dest_type(dest_.type);
    ui::text(list, fonts.semibold, "FOLDER FOR", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.semibold,
             fonts.semibold.font->fit(dest_.name.empty() ? (t ? t->label : dest_.type) : dest_.name, 30, kSide.w - 88), x,
             y + 44, 30, look.panel_text);
    ui::text(list, fonts.semibold, "IN", x, y + 120, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::paragraph(list, fonts.regular, folder_path_.empty() ? "-" : plain_text(folder_path_), x, y + 156, 24,
                  kSide.w - 88, 32, look.panel_text, 4);
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
  // About, as in RommPS: a few facts, what's been ripped, then who made it
  // and a code to the source.
  auto fact = [&](const char *label, const std::string &value, float fx, float w) {
    ui::text(list, fonts.semibold, label, fx, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
    ui::text(list, fonts.regular, fonts.regular.font->fit(value.empty() ? "-" : value, 24, w), fx, y + 36, 24,
             look.panel_text);
  };
  auto item = [&](const char *label, const std::string &value) {
    fact(label, value, x, kSide.w - 88);
    y += 76;
  };
  item("WEB UI", s.ip.empty() ? "" : "http://" + s.ip + ":" + std::to_string(s.port));
  item("VERSION", s.version + (s.build.empty() || s.build == "dev" ? "" : " (" + s.build + ")"));
  // The count, then the systems most ripped, on as many as two lines.
  const Stats &st = a.stats();
  std::string discs;
  if (st.known) {
    discs = std::to_string(static_cast<int>(st.discs));
    for (std::size_t i = 0; i < st.systems.size() && i < 3; ++i) {
      const System *sys = a.catalog().system(st.systems[i].first);
      discs += std::string(i ? ", " : ": ") + std::to_string(static_cast<int>(st.systems[i].second)) + " " +
               (sys ? sys->name : st.systems[i].first);
    }
  }
  ui::text(list, fonts.semibold, "DISCS RIPPED", x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
  ui::paragraph(list, fonts.regular, discs.empty() ? "-" : discs, x, y + 36, 24, kSide.w - 88, 32, look.panel_text, 2);
  y += 36 + 2 * 32 + 12;
  item("DATA RIPPED", st.known ? human_size(st.bytes) : "");
  list.line(kSide.x + 44, kSide.y + kSide.h - 196, kSide.x + kSide.w - 44, kSide.y + kSide.h - 196, 1.5f, look.outline);
  draw_credits(list, fonts, look, {x, kSide.y + kSide.h - 168, kSide.w - 88, 136});
}

// Who made it, and a QR code to the source (and its issue forms).
void SettingsPage::draw_credits(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, const Rect &r) const {
  if (qr_for_ != kSourceUrl) {
    qr_for_ = kSourceUrl;
    qr_ = QrCode::encode(kSourceUrl);
  }
  const Rect avatar{r.x, r.y + (r.h - 128) * 0.5f, 128, 128};
  if (const std::uint32_t texture = avatar_texture())
    list.image(texture, avatar, hui::gfx::kFullUv, Color::rgb(0xffffff), 64);
  else
    list.circle(avatar.cx(), avatar.cy(), 64, look.panel_muted.with_alpha(0.3f));
  const float tx = avatar.x + avatar.w + 24;
  ui::text(list, fonts.regular, "Made with love", tx, r.y + 52, 22, look.panel_muted);
  ui::text(list, fonts.semibold, "by s0liton", tx, r.y + 86, 26, look.panel_text);
  draw_qr(list, qr_, {r.x + r.w - r.h, r.y, r.h, r.h});
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
