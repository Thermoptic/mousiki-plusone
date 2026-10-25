// PlusOne: the FOLDERS pane. F6 puts it in the place of the QUEUE pane, F7 brings the queue back. It is a small file explorer
// over the local music paths (Settings -> PATHS -> Local Path): ENTER on a folder lists the music files that lie directly in it
// in the LOCAL AUDIO FILES pane and, when the folder holds further folders (with music below them), goes into it; BACKSPACE goes
// one step back. Only folders that have music files in them or below them are listed. The last state is kept in
// plusone_state.txt next to config.txt.
#include "app.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

#include "path_utf8.h"
#include "settings.h"

namespace muisc {

namespace {

// The parts of a path without the root separators, for comparing: the same folder written with another slash or (on Windows)
// another case is the same folder.
std::vector<std::string> path_parts(const fs::path& p, std::vector<std::string>* original = nullptr) {
    std::vector<std::string> out;
    for (const auto& c : p.lexically_normal()) {
        std::string s = path_utf8(c);
        while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        if (s.empty() || s == ".") continue;
        if (original) original->push_back(s);
#ifdef _WIN32
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
        out.push_back(s);
    }
    return out;
}

std::string lower_ascii(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

fs::path state_file() { return config_path().parent_path() / "plusone_state.txt"; }

using SubMap = std::map<std::string, std::pair<int, std::set<std::string>>>;

}  // namespace

// The entries of the folder shown in the FOLDERS pane (cached until the folder or the library changes).
const std::vector<App::FxEntry>& App::fx_entries() const {
    const std::vector<std::string> roots = fx_roots();
    std::string key = fx_dir_ + "|" + std::to_string(all_local_tracks_.size());
    for (const auto& r : roots) key += "|" + r;
    if (key == fx_key_) return fx_entries_;
    fx_key_ = key;
    fx_entries_.clear();

    // One pass over the library for a base folder: its own files (into `self`) and the folders directly below it.
    // `names` keeps one entry per subfolder (lower case name -> entry), `deeper` the folders one level further down.
    auto scan = [&](const fs::path& base, std::map<std::string, FxEntry>& names, std::map<std::string, std::set<std::string>>& deeper,
                    FxEntry* self) {
        const std::vector<std::string> bp = path_parts(base);
        for (const auto& t : all_local_tracks_) {
            std::vector<std::string> orig;
            const std::vector<std::string> pp = path_parts(t.path.parent_path(), &orig);
            if (pp.size() < bp.size() || !std::equal(bp.begin(), bp.end(), pp.begin())) continue;
            if (self) ++self->total;
            if (pp.size() == bp.size()) {   // a file lying directly in the base folder
                if (self) { ++self->direct; if (self->filter.empty()) self->filter = path_utf8(t.path.parent_path()); }
                continue;
            }
            auto& e = names[pp[bp.size()]];
            if (e.name.empty()) { e.name = orig[bp.size()]; e.path = path_utf8(base / path_from_utf8(e.name)); }
            ++e.total;
            if (pp.size() == bp.size() + 1) { ++e.direct; if (e.filter.empty()) e.filter = path_utf8(t.path.parent_path()); }
            else deeper[pp[bp.size()]].insert(pp[bp.size() + 1]);
        }
    };

    if (fx_dir_.empty()) {
        for (const auto& r : roots) {
            std::map<std::string, FxEntry> names;
            std::map<std::string, std::set<std::string>> deeper;
            FxEntry e;
            e.path = r;
            const fs::path rp = path_from_utf8(r);
            e.name = path_utf8(rp.filename());
            if (e.name.empty()) e.name = r;
            scan(rp, names, deeper, &e);
            e.subs = static_cast<int>(names.size());
            fx_entries_.push_back(std::move(e));
        }
    } else {
        std::map<std::string, FxEntry> names;
        std::map<std::string, std::set<std::string>> deeper;
        scan(path_from_utf8(fx_dir_), names, deeper, nullptr);
        for (auto& [k, e] : names) {
            e.subs = static_cast<int>(deeper[k].size());
            fx_entries_.push_back(std::move(e));
        }
    }
    std::sort(fx_entries_.begin(), fx_entries_.end(),
              [](const FxEntry& a, const FxEntry& b) { return lower_ascii(a.name) < lower_ascii(b.name); });
    return fx_entries_;
}

// The local music paths of the user (the cache and download folders the app adds by itself are not folders to browse).
std::vector<std::string> App::fx_roots() const {
    std::vector<std::string> out;
    const auto skip = [&](const std::string& r) {
        const auto rp = path_parts(path_from_utf8(r));
        return rp == path_parts(cache_.cache_dir()) || rp == path_parts(cache_.download_dir());
    };
    for (const auto& r : library_roots()) if (!r.empty() && !skip(r)) out.push_back(r);
    return out;
}

void App::fx_follow() {
    const int n = static_cast<int>(fx_entries().size());
    const int rows = std::max(1, list_nav_rows());
    fx_selected_ = std::clamp(fx_selected_, 0, std::max(0, n - 1));
    if (fx_selected_ < fx_scroll_) fx_scroll_ = fx_selected_;
    if (fx_selected_ >= fx_scroll_ + rows) fx_scroll_ = fx_selected_ - rows + 1;
    fx_scroll_ = std::clamp(fx_scroll_, 0, std::max(0, n - rows));
}

void App::fx_open() {
    const auto& es = fx_entries();
    if (fx_selected_ < 0 || fx_selected_ >= static_cast<int>(es.size())) return;
    const FxEntry e = es[static_cast<size_t>(fx_selected_)];
    if (e.direct > 0 && !e.filter.empty()) {   // the music files of the folder into the LOCAL AUDIO FILES pane
        folder_filter_ = e.filter;
        last_local_query_.clear();
        local_sort_mode_ = 0;
        list_source_ = ListSource::Local;
        selected_ = 0;
        scroll_ = 0;
        refresh_local_view();
        log_event("folder: " + e.name);
    }
    if (e.subs > 0) {   // folders below it: they are shown here now
        fx_dir_ = e.path;
        fx_selected_ = fx_scroll_ = 0;
    } else if (e.direct == 0) {
        status_line_ = "no music files in that folder";
    }
    fx_follow();
    fx_save_state();
}

void App::fx_back() {
    if (fx_dir_.empty()) return;
    const std::vector<std::string> from = path_parts(path_from_utf8(fx_dir_));
    bool is_root = false;
    for (const auto& r : fx_roots()) if (path_parts(path_from_utf8(r)) == from) { is_root = true; break; }
    fx_dir_ = is_root ? std::string() : path_utf8(path_from_utf8(fx_dir_).parent_path());
    fx_selected_ = fx_scroll_ = 0;
    const auto& es = fx_entries();
    for (size_t i = 0; i < es.size(); ++i)   // the cursor stays on the folder one came out of
        if (path_parts(path_from_utf8(es[i].path)) == from) { fx_selected_ = static_cast<int>(i); break; }
    fx_follow();
    fx_save_state();
}

void App::fx_set_pane(bool on) {
    if (audiobook_) return;
    folders_pane_ = on;
    if (on) { queue_focus_ = true; fx_follow(); }
    fx_save_state();
}

// The keys of the FOLDERS pane while it has the focus. true = the key was used here.
bool App::fx_key(const std::string& action, int key) {
    const int n = static_cast<int>(fx_entries().size());
    if (action == "HKeyNavigateUp") fx_selected_ = std::max(0, fx_selected_ - 1);
    else if (action == "HKeyNavigateDown") fx_selected_ = std::min(std::max(0, n - 1), fx_selected_ + 1);
    else if (action == "HKeyNavigateFastUp") fx_selected_ = std::max(0, fx_selected_ - 10);
    else if (action == "HKeyNavigateFastDown") fx_selected_ = std::min(std::max(0, n - 1), fx_selected_ + 10);
    else if (action == "HKeyNavigateTop") fx_selected_ = 0;
    else if (action == "HKeyNavigateBottom") fx_selected_ = std::max(0, n - 1);
    else if (action == "HKeyPlay" || key == '\r' || key == '\n') { fx_open(); return true; }
    else if ((key == 127 || key == 8) && !last_key_was_arrow()) { fx_back(); return true; }
    else return false;
    fx_follow();
    return true;
}

void App::fx_save_state() const {
    if (audiobook_) return;
    std::ofstream out(state_file(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "FoldersPane=" << (folders_pane_ ? 1 : 0) << "\n";
    out << "FoldersDir=" << fx_dir_ << "\n";
}

void App::fx_load_state() {
    if (audiobook_) return;
    std::ifstream in(state_file(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("FoldersPane=", 0) == 0) folders_pane_ = line.substr(12) == "1";
        else if (line.rfind("FoldersDir=", 0) == 0) fx_dir_ = line.substr(11);
    }
    if (!fx_dir_.empty() && !fs::is_directory(path_from_utf8(fx_dir_))) fx_dir_.clear();
}

}  // namespace muisc
