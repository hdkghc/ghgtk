/*
 * ghgtk - GitHub GTK Client
 *
 * Copyright (C) 2026 hdkghc & DeepSeek
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// ghgtk.cpp - GitHub GTK Client
// Build: g++ -o ghgtk ghgtk.cpp `pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1` -ljson-c -lcmark -std=c++17 -pthread

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <json-c/json.h>
#include <cmark.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

using namespace std;

// ============ Data Structures ============

struct Asset {
    string name;
    string size;
    int64_t size_bytes;
};

struct Release {
    string tag;
    string published;
    string body;
    vector<Asset> assets;
    int64_t total_size_bytes;
};

struct Repo {
    string name;
    string owner;
    string description;
    int stars;
};

// ============ Global State ============

vector<Repo> repos;
vector<Release> releases;
string currentOwner, currentRepo;
atomic<bool> isWorking{false};
atomic<bool> shouldStop{false};
atomic<int64_t> downloaded_bytes{0};
atomic<int> downloaded_files{0};
atomic<pid_t> gh_pid{0};
atomic<double> current_speed{0.0};
atomic<double> current_eta{0.0};
mutex releasesMutex;
mutex speed_mutex;

GtkWidget *window;
GtkWidget *searchEntry;
GtkWidget *repoList;
GtkWidget *releaseList;
GtkWidget *assetList;
GtkWidget *statusBar;
GtkWidget *bodyWebView;
GtkWidget *debugView;
GtkWidget *progressDialog;
GtkWidget *progressBar;
GtkWidget *progressBar2;
GtkWidget *progressLabel;
GtkWidget *progressLabel2;
GtkWidget *speedLabel;
GtkWidget *spinner;
GtkWidget *notebook;

GtkListStore *repoStore;
GtkListStore *releaseStore;
GtkListStore *assetStore;
GtkTextBuffer *debugBuffer = nullptr;

// ============ Safe String ============

string safe(const string& s) {
    string r;
    r.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 32 && c <= 126) {
            r.push_back((char)c);
        } else if (c == '\n' || c == '\r') {
            r.push_back('\n');
        }
    }
    return r;
}

// ============ Utility Functions ============

string exec_cmd(const string& cmd) {
    char buf[4096];
    string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result += buf;
    }
    pclose(pipe);
    return result;
}

string format_size(int64_t bytes) {
    if (bytes < 0) return "N/A";
    if (bytes < 1024) return to_string(bytes) + " B";
    if (bytes < 1024 * 1024) {
        double kb = bytes / 1024.0;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f KB", kb);
        return string(buf);
    }
    if (bytes < 1024 * 1024 * 1024) {
        double mb = bytes / (1024.0 * 1024.0);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return string(buf);
    }
    double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f GB", gb);
    return string(buf);
}

string format_speed(double kb_s) {
    if (kb_s < 0) return "N/A";
    if (kb_s < 1.0) {
        double b_s = kb_s * 1024;
        return to_string((int)b_s) + " B/s";
    }
    if (kb_s < 1024) {
        return to_string((int)kb_s) + " KB/s";
    }
    double mb_s = kb_s / 1024.0;
    if (mb_s < 1024) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f MB/s", mb_s);
        return string(buf);
    }
    double gb_s = mb_s / 1024.0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f GB/s", gb_s);
    return string(buf);
}

string format_eta(double seconds) {
    if (seconds < 0 || seconds > 86400) return "> 24h";
    if (seconds < 60) return to_string((int)seconds) + "s";
    if (seconds < 3600) {
        int mins = (int)(seconds / 60);
        int secs = (int)(seconds) % 60;
        return to_string(mins) + "m " + to_string(secs) + "s";
    }
    if (seconds < 86400) {
        int hours = (int)(seconds / 3600);
        int mins = (int)((seconds - hours * 3600) / 60);
        return to_string(hours) + "h " + to_string(mins) + "m";
    }
    return "> 24h";
}

void set_status(const string& msg) {
    string s = safe(msg);
    gtk_statusbar_push(GTK_STATUSBAR(statusBar),
        gtk_statusbar_get_context_id(GTK_STATUSBAR(statusBar), "status"),
        s.c_str());
}

// ===== 修复 show_debug：使用 g_idle_add 确保主线程执行 =====

void show_debug(const string& msg) {
    if (!debugBuffer) return;
    
    g_idle_add([](gpointer data) -> gboolean {
        string* msg_ptr = static_cast<string*>(data);
        GtkTextIter iter;
        gtk_text_buffer_get_end_iter(debugBuffer, &iter);
        gtk_text_buffer_insert(debugBuffer, &iter, msg_ptr->c_str(), -1);
        gtk_text_buffer_insert(debugBuffer, &iter, "\n", -1);
        
        GtkTextMark *mark = gtk_text_buffer_create_mark(debugBuffer, "end", &iter, false);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(debugView), mark, 0, false, 0, 0);
        gtk_text_buffer_delete_mark(debugBuffer, mark);
        
        delete msg_ptr;
        return G_SOURCE_REMOVE;
    }, new string(safe(msg)));
}

// ===== Single progress bar =====

void update_single_progress(const string& label, double frac) {
    string s = safe(label);
    gtk_label_set_text(GTK_LABEL(progressLabel), s.c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar), frac);
    gtk_widget_hide(progressLabel2);
    gtk_widget_hide(progressBar2);
    gtk_widget_hide(speedLabel);
}

void show_single_progress(const string& label, double frac) {
    g_idle_add([](gpointer data) -> gboolean {
        auto* args = static_cast<tuple<string, double>*>(data);
        update_single_progress(get<0>(*args), get<1>(*args));
        delete args;
        return G_SOURCE_REMOVE;
    }, new tuple<string, double>(label, frac));
}

// ===== Double progress bars with speed/ETA =====

void update_double_progress(const string& label1, double frac1,
                            const string& label2, double frac2) {
    gtk_label_set_text(GTK_LABEL(progressLabel), safe(label1).c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar), frac1);
    gtk_widget_show(progressLabel2);
    gtk_widget_show(progressBar2);
    gtk_label_set_text(GTK_LABEL(progressLabel2), safe(label2).c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar2), frac2);

    double speed = current_speed.load();
    double eta = current_eta.load();
    if (speed > 0) {
        string info = format_speed(speed) + "  ETA: " + format_eta(eta);
        gtk_label_set_text(GTK_LABEL(speedLabel), info.c_str());
        gtk_widget_show(speedLabel);
    } else if (frac2 > 0 && frac2 < 1) {
        gtk_label_set_text(GTK_LABEL(speedLabel), "Calculating speed...");
        gtk_widget_show(speedLabel);
    } else {
        gtk_widget_hide(speedLabel);
    }
}

void show_double_progress(const string& label1, double frac1,
                          const string& label2, double frac2) {
    g_idle_add([](gpointer data) -> gboolean {
        auto* args = static_cast<tuple<string, double, string, double>*>(data);
        update_double_progress(get<0>(*args), get<1>(*args),
                               get<2>(*args), get<3>(*args));
        delete args;
        return G_SOURCE_REMOVE;
    }, new tuple<string, double, string, double>(label1, frac1, label2, frac2));
}

// ===== Progress Dialog =====

void show_progress_dialog(const string& title, bool single = true) {
    if (!progressDialog) {
        progressDialog = gtk_dialog_new_with_buttons(
            safe(title).c_str(), GTK_WINDOW(window),
            (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            "_Cancel", GTK_RESPONSE_CANCEL, NULL);
        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(progressDialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), 10);

        progressLabel = gtk_label_new("Starting...");
        gtk_box_pack_start(GTK_BOX(content), progressLabel, false, false, 2);
        progressBar = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar), 0.0);
        gtk_widget_set_size_request(progressBar, 400, 20);
        gtk_box_pack_start(GTK_BOX(content), progressBar, false, false, 2);

        progressLabel2 = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(content), progressLabel2, false, false, 2);
        progressBar2 = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar2), 0.0);
        gtk_widget_set_size_request(progressBar2, 400, 20);
        gtk_box_pack_start(GTK_BOX(content), progressBar2, false, false, 2);

        speedLabel = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(content), speedLabel, false, false, 2);

        if (single) {
            gtk_widget_hide(progressLabel2);
            gtk_widget_hide(progressBar2);
            gtk_widget_hide(speedLabel);
        }

        gtk_widget_show_all(content);

        g_signal_connect(progressDialog, "response",
            G_CALLBACK(+[](GtkDialog *d, int resp, gpointer) {
                if (resp == GTK_RESPONSE_CANCEL) {
                    shouldStop = true;
                    pid_t pid = gh_pid.exchange(0);
                    if (pid > 0) {
                        kill(pid, SIGTERM);
                        this_thread::sleep_for(chrono::milliseconds(200));
                        kill(pid, SIGKILL);
                        set_status("Cancelled");
                    }
                }
            }), nullptr);
    } else {
        gtk_window_set_title(GTK_WINDOW(progressDialog), safe(title).c_str());
        if (single) {
            gtk_widget_hide(progressLabel2);
            gtk_widget_hide(progressBar2);
            gtk_widget_hide(speedLabel);
        } else {
            gtk_widget_show(progressLabel2);
            gtk_widget_show(progressBar2);
            gtk_widget_show(speedLabel);
        }
    }

    if (single) {
        show_single_progress("Starting...", 0.0);
    } else {
        show_double_progress("Files: 0/0", 0.0, "Size: 0 B / 0 B", 0.0);
        gtk_label_set_text(GTK_LABEL(speedLabel), "Calculating...");
    }
    gtk_widget_show(progressDialog);
}

void hide_progress_dialog() {
    if (progressDialog) {
        gtk_widget_hide(progressDialog);
        shouldStop = false;
        downloaded_bytes = 0;
        downloaded_files = 0;
        gh_pid = 0;
        current_speed = 0.0;
        current_eta = 0.0;
    }
}

void set_working(bool working) {
    isWorking = working;
    if (working) {
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_widget_show(spinner);
        gtk_widget_set_sensitive(GTK_WIDGET(window), false);
    } else {
        gtk_spinner_stop(GTK_SPINNER(spinner));
        gtk_widget_hide(spinner);
        gtk_widget_set_sensitive(GTK_WIDGET(window), true);
        hide_progress_dialog();
    }
}

// ============ Markdown Rendering ============

string render_markdown_html(const string& body) {
    string safe_body = safe(body);
    if (safe_body.empty()) {
        return "<p><em>No release notes</em></p>";
    }

    char* html = cmark_markdown_to_html(safe_body.c_str(), safe_body.size(), CMARK_OPT_DEFAULT);
    string result = html ? string(html) : "";
    if (html) free(html);

    return result;
}

static void on_mouse_target_changed(WebKitWebView *web_view,
                                    WebKitHitTestResult *hit_test_result,
                                    guint modifiers, gpointer user_data) {
    if (webkit_hit_test_result_context_is_link(hit_test_result)) {
        const char* uri = webkit_hit_test_result_get_link_uri(hit_test_result);
        if (uri) {
            set_status("URL: " + string(uri));
            return;
        }
    }
    set_status("Ready");
}

void render_markdown(const string& body) {
    if (!bodyWebView) return;

    string html_content = render_markdown_html(body);

    string full_html = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <style>
        body {
            font-family: 'Ubuntu Sans Mono', 'Cascadia Mono', 'Consolas', monospace;
            font-size: 14px;
            line-height: 1.6;
            margin: 15px;
            padding: 0;
            color: #24292e;
            background: #ffffff;
            overflow-wrap: break-word;
            word-wrap: break-word;
        }
        h1 { font-size: 2em; border-bottom: 2px solid #eaecef; padding-bottom: 0.3em; margin: 0.5em 0; }
        h2 { font-size: 1.5em; border-bottom: 1px solid #eaecef; padding-bottom: 0.3em; margin: 0.5em 0; }
        h3 { font-size: 1.25em; margin: 0.5em 0; }
        h4 { font-size: 1.1em; margin: 0.5em 0; }
        h5 { font-size: 1.0em; margin: 0.5em 0; }
        h6 { font-size: 0.9em; margin: 0.5em 0; }
        p { margin: 0.5em 0; }
        pre {
            background: #f6f8fa;
            padding: 12px;
            border-radius: 6px;
            overflow: auto;
            border: 1px solid #e1e4e8;
            font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', 'Consolas', monospace;
            font-size: 13px;
            white-space: pre-wrap;
            word-break: break-all;
        }
        code {
            background: #f6f8fa;
            padding: 2px 6px;
            border-radius: 3px;
            font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', 'Consolas', monospace;
            font-size: 0.9em;
        }
        pre code {
            background: transparent;
            padding: 0;
            border-radius: 0;
            font-size: inherit;
            font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', 'Consolas', monospace;
        }
        ul, ol {
            padding-left: 25px;
            margin: 0.3em 0;
        }
        li {
            margin: 2px 0;
        }
        li > ul, li > ol {
            margin: 0;
            padding-left: 20px;
        }
        blockquote {
            border-left: 4px solid #dfe2e5;
            padding-left: 16px;
            color: #6a737d;
            margin: 10px 0;
            background: #f8f9fa;
            padding: 8px 16px;
        }
        a {
            color: #0366d6;
            text-decoration: underline;
            cursor: pointer;
        }
        a:hover {
            color: #0056b3;
        }
        table {
            border-collapse: collapse;
            width: 100%;
            margin: 10px 0;
        }
        th, td {
            border: 1px solid #dfe2e5;
            padding: 6px 13px;
        }
        th {
            background: #f6f8fa;
        }
        img {
            max-width: 100%;
        }
        hr {
            border: 0;
            border-top: 1px solid #eaecef;
            margin: 15px 0;
        }
        .highlight {
            background: #f6f8fa;
        }
    </style>
</head>
<body>
    )" + html_content + R"(
</body>
</html>
)";

    webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), full_html.c_str(), NULL);
}

// ============ GitHub API ============

vector<Repo> searchRepos(const string& query, function<void(double)> progress) {
    vector<Repo> result;
    if (query.empty() || shouldStop) return result;
    progress(0.1);
    string out = exec_cmd("gh search repos " + query + " --limit 100 --json name,owner,description,stargazersCount 2>&1");
    if (out.empty() || shouldStop) return result;
    progress(0.5);
    json_object* root = json_tokener_parse(out.c_str());
    if (!root) return result;
    if (json_object_get_type(root) != json_type_array) {
        json_object_put(root);
        return result;
    }
    int len = json_object_array_length(root);
    for (int i = 0; i < len && !shouldStop; i++) {
        json_object* item = json_object_array_get_idx(root, i);
        Repo r;
        json_object* obj;
        if (json_object_object_get_ex(item, "name", &obj)) {
            const char* v = json_object_get_string(obj);
            r.name = v ? safe(v) : "";
        }
        if (json_object_object_get_ex(item, "owner", &obj)) {
            json_object* login;
            if (json_object_object_get_ex(obj, "login", &login)) {
                const char* v = json_object_get_string(login);
                r.owner = v ? safe(v) : "";
            }
        }
        if (json_object_object_get_ex(item, "description", &obj)) {
            const char* v = json_object_get_string(obj);
            r.description = v ? safe(v) : "";
        }
        if (json_object_object_get_ex(item, "stargazersCount", &obj))
            r.stars = json_object_get_int(obj);
        if (!r.name.empty() && !r.owner.empty())
            result.push_back(r);
        progress(0.7 + 0.3 * (i / (double)len));
    }
    json_object_put(root);
    progress(1.0);
    return result;
}

void fetch_release_details(const string& owner, const string& repo, const string& tag,
                           Release& release, function<void(int,int)> progress) {
    if (shouldStop) return;

    string cmd = "gh release view " + tag + " -R " + owner + "/" + repo +
                 " --json assets,body 2>&1";
    string out = exec_cmd(cmd);
    if (out.empty()) return;

    json_object* root = json_tokener_parse(out.c_str());
    if (!root) return;

    json_object* bodyObj;
    if (json_object_object_get_ex(root, "body", &bodyObj)) {
        const char* v = json_object_get_string(bodyObj);
        release.body = v ? safe(v) : "";
    }

    json_object* assets;
    release.total_size_bytes = 0;
    if (json_object_object_get_ex(root, "assets", &assets)) {
        int len = json_object_array_length(assets);
        for (int i = 0; i < len && !shouldStop; i++) {
            json_object* a = json_object_array_get_idx(assets, i);
            json_object* nameObj;
            json_object* sizeObj;
            Asset asset;
            if (json_object_object_get_ex(a, "name", &nameObj)) {
                const char* v = json_object_get_string(nameObj);
                asset.name = v ? safe(v) : "";
            }
            if (json_object_object_get_ex(a, "size", &sizeObj)) {
                int64_t size_bytes = json_object_get_int64(sizeObj);
                asset.size_bytes = size_bytes;
                asset.size = format_size(size_bytes);
                release.total_size_bytes += size_bytes;
            }
            if (!asset.name.empty()) {
                release.assets.push_back(asset);
            }
        }
    }
    json_object_put(root);
}

vector<Release> getReleases(const string& owner, const string& repo, function<void(double)> progress) {
    vector<Release> result;
    if (shouldStop) return result;
    progress(0.1);

    string out = exec_cmd("gh release list -R " + owner + "/" + repo + " --limit 100 --json tagName,publishedAt 2>&1");
    if (out.empty() || shouldStop) return result;
    progress(0.2);

    json_object* root = json_tokener_parse(out.c_str());
    if (!root) return result;
    if (json_object_get_type(root) != json_type_array) {
        json_object_put(root);
        return result;
    }

    int len = json_object_array_length(root);
    for (int i = 0; i < len && !shouldStop; i++) {
        json_object* item = json_object_array_get_idx(root, i);
        Release r;
        r.total_size_bytes = 0;
        json_object* obj;
        if (json_object_object_get_ex(item, "tagName", &obj)) {
            const char* v = json_object_get_string(obj);
            r.tag = v ? safe(v) : "";
        }
        if (json_object_object_get_ex(item, "publishedAt", &obj)) {
            const char* v = json_object_get_string(obj);
            r.published = v ? safe(v) : "";
        }
        if (!r.tag.empty()) result.push_back(r);
    }
    json_object_put(root);
    progress(0.3);

    int total = result.size();
    if (total == 0) { progress(1.0); return result; }

    vector<thread> threads;
    int completed = 0;
    mutex completedMutex;

    for (int i = 0; i < total && !shouldStop; i++) {
        threads.emplace_back([&, i]() {
            fetch_release_details(owner, repo, result[i].tag, result[i],
                [&](int done, int total) {
                    lock_guard<mutex> lock(completedMutex);
                    completed = done;
                    progress(0.3 + 0.7 * (completed / (double)total));
                });
            lock_guard<mutex> lock(completedMutex);
            completed++;
            progress(0.3 + 0.7 * (completed / (double)total));
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    progress(1.0);
    return result;
}

// ============ File Utilities ============

bool file_exists(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

int64_t file_size(const string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return st.st_size;
    }
    return -1;
}

// ============ Download with Speed/ETA Monitoring ============

void do_download_async(const string& owner, const string& repo, const string& tag,
                       const vector<Asset>& assets, bool create_folder, const string& base_path) {
    if (isWorking) return;

    downloaded_bytes = 0;
    downloaded_files = 0;
    shouldStop = false;
    gh_pid = 0;
    current_speed = 0.0;
    current_eta = 0.0;

    isWorking = true;
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_show(spinner);
    gtk_widget_set_sensitive(GTK_WIDGET(window), false);

    show_progress_dialog("Downloading...", false);

    int total_files = assets.size();
    int64_t total_bytes = 0;
    for (auto& a : assets) total_bytes += a.size_bytes;

    thread([owner, repo, tag, assets, total_files, total_bytes,
            create_folder, base_path]() {
        chdir(base_path.c_str());

        string target_dir = "";
        if (create_folder) {
            target_dir = repo + "-" + tag;
            mkdir(target_dir.c_str(), 0755);
        }

        chrono::steady_clock::time_point last_sample_time = chrono::steady_clock::now();
        int64_t last_sample_bytes = 0;
        int64_t accumulated_bytes = 0;

        for (int i = 0; i < total_files && !shouldStop; i++) {
            string filename = assets[i].name;
            string filepath = target_dir.empty() ? filename : target_dir + "/" + filename;
            int64_t total_size = assets[i].size_bytes;

            // Skip if file already exists and has correct size
            if (file_exists(filepath) && file_size(filepath) >= total_size) {
                show_debug("Skipping existing: " + filename);
                downloaded_files++;
                downloaded_bytes += total_size;
                accumulated_bytes += total_size;
                double file_progress = downloaded_files / (double)total_files;
                double byte_progress = (total_bytes > 0) ?
                    (accumulated_bytes / (double)total_bytes) : file_progress;
                show_double_progress(
                    "Files: " + to_string(downloaded_files) + "/" + to_string(total_files),
                    file_progress,
                    "Skipped: " + filename, 1.0
                );
                continue;
            }

            show_debug("Downloading: " + filename);

            string cmd = "gh release download -R " + owner + "/" + repo +
                        " " + tag + " --pattern '" + filename + "' --clobber 2>/dev/null";
            if (!target_dir.empty()) {
                cmd = "cd '" + target_dir + "' && " + cmd;
            }

            pid_t pid = fork();
            if (pid == 0) {
                execl("/bin/sh", "sh", "-c", cmd.c_str(), NULL);
                exit(1);
            } else if (pid > 0) {
                gh_pid = pid;
                bool file_created = false;
                int64_t last_size = 0;

                // For speed calculation, initialize last_sample_bytes to existing file size
                int64_t initial_size = file_exists(filepath) ? file_size(filepath) : 0;
                if (initial_size > 0) {
                    last_sample_bytes = initial_size;
                }

                // Monitor this file's download progress
                while (!shouldStop) {
                    if (file_exists(filepath)) {
                        int64_t current_size = file_size(filepath);
                        if (current_size >= 0) {
                            file_created = true;
                            double single_progress = (total_size > 0) ?
                                min(current_size / (double)total_size, 0.999) : 0.0;

                            // Calculate speed
                            auto now = chrono::steady_clock::now();
                            double elapsed = chrono::duration<double>(now - last_sample_time).count();
                            if (elapsed >= 0.5) {
                                int64_t delta_bytes = current_size - last_sample_bytes;
                                if (delta_bytes < 0) delta_bytes = 0;
                                double speed_kb_s = (delta_bytes / 1024.0) / elapsed;
                                if (speed_kb_s > 0 && elapsed > 0) {
                                    current_speed = speed_kb_s;
                                    int64_t remaining_bytes = total_bytes - accumulated_bytes - current_size;
                                    if (remaining_bytes < 0) remaining_bytes = 0;
                                    if (remaining_bytes > 0 && speed_kb_s > 0.1) {
                                        current_eta = (remaining_bytes / 1024.0) / speed_kb_s;
                                    } else if (remaining_bytes <= 0) {
                                        current_eta = 0.0;
                                    }
                                }
                                last_sample_time = now;
                                last_sample_bytes = current_size;
                            }

                            string label2 = filename + ": " + format_size(current_size) +
                                           " / " + format_size(total_size);
                            show_double_progress(
                                "Files: " + to_string(downloaded_files) + "/" + to_string(total_files),
                                downloaded_files / (double)total_files,
                                label2, single_progress
                            );
                            last_size = current_size;

                            if (current_size >= total_size) {
                                break;
                            }
                        }
                    } else {
                        if (file_created) {
                            show_double_progress(
                                "Files: " + to_string(downloaded_files) + "/" + to_string(total_files),
                                downloaded_files / (double)total_files,
                                "Waiting: " + filename, 0.0
                            );
                        }
                    }

                    // Check if gh process finished
                    int status;
                    pid_t result = waitpid(pid, &status, WNOHANG);
                    if (result == pid) {
                        if (file_exists(filepath) && file_size(filepath) >= total_size) {
                            break;
                        }
                        if (file_created) {
                            break;
                        }
                        show_debug("Download failed: " + filename);
                        break;
                    }

                    this_thread::sleep_for(chrono::milliseconds(200));
                }

                waitpid(pid, NULL, 0);
                if (gh_pid == pid) gh_pid = 0;
            } else {
                // fork failed
                show_debug("Fork failed for: " + filename);
                continue;
            }

            downloaded_files++;
            downloaded_bytes += total_size;
            accumulated_bytes += total_size;

            double file_progress = downloaded_files / (double)total_files;
            double byte_progress = (total_bytes > 0) ?
                (accumulated_bytes / (double)total_bytes) : file_progress;
            show_double_progress(
                "Files: " + to_string(downloaded_files) + "/" + to_string(total_files),
                file_progress,
                "Done: " + filename, 1.0
            );
        }

        // Final update
        show_double_progress(
            "Files: " + to_string(total_files) + "/" + to_string(total_files),
            1.0,
            "Size: " + format_size(total_bytes) + " / " + format_size(total_bytes),
            1.0
        );
        gtk_label_set_text(GTK_LABEL(speedLabel), "Complete!");

        show_debug("Download thread finished");

        g_idle_add([](gpointer) -> gboolean {
            if (shouldStop) {
                set_status("Download cancelled");
            } else {
                set_status("Download complete!");
            }
            set_working(false);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }).detach();
}

void do_clone_async(const string& repo) {
    if (isWorking) return;
    set_working(true);
    show_progress_dialog("Cloning...", true);
    thread([repo]() {
        show_single_progress("Cloning: " + repo, 0.5);
        exec_cmd("gh repo clone " + repo + " 2>&1");
        g_idle_add([](gpointer) -> gboolean {
            set_status("Clone complete!");
            set_working(false);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }).detach();
}

// ============ Async Operations ============

void do_search_async(const string& query) {
    if (isWorking) return;
    set_working(true);
    show_progress_dialog("Searching...", true);
    gtk_list_store_clear(repoStore);
    gtk_list_store_clear(releaseStore);
    gtk_list_store_clear(assetStore);
    if (bodyWebView) webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), "", NULL);
    thread([query]() {
        auto progress = [](double f) { show_single_progress("Searching...", f); };
        repos = searchRepos(query, progress);
        g_idle_add([](gpointer) -> gboolean {
            for (auto& r : repos) {
                GtkTreeIter it;
                gtk_list_store_append(repoStore, &it);
                string d = r.owner + "/" + r.name;
                string desc = r.description;
                if (desc.length() > 50) desc = desc.substr(0, 47) + "...";
                string info = to_string(r.stars) + " ★";
                gtk_list_store_set(repoStore, &it, 0, d.c_str(), 1, desc.c_str(), 2, info.c_str(), -1);
            }
            set_status("Found " + to_string(repos.size()) + " repos");
            set_working(false);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }).detach();
}

void do_releases_async(const string& owner, const string& repo) {
    if (isWorking) return;
    set_working(true);
    show_progress_dialog("Loading releases...", true);
    gtk_list_store_clear(releaseStore);
    gtk_list_store_clear(assetStore);
    if (bodyWebView) webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), "", NULL);
    thread([owner, repo]() {
        auto progress = [](double f) { show_single_progress("Loading releases...", f); };
        releases = getReleases(owner, repo, progress);
        g_idle_add([](gpointer) -> gboolean {
            for (auto& r : releases) {
                GtkTreeIter it;
                gtk_list_store_append(releaseStore, &it);
                string assets = to_string(r.assets.size()) + " files";
                string date = r.published.substr(0, 10);
                string size = format_size(r.total_size_bytes);
                gtk_list_store_set(releaseStore, &it, 0, r.tag.c_str(), 1, date.c_str(), 2, assets.c_str(), -1);
            }
            set_status("Loaded " + to_string(releases.size()) + " releases");
            set_working(false);
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }).detach();
}

// ============ Callbacks ============

void on_search(GtkWidget*, gpointer) {
    const char* q = gtk_entry_get_text(GTK_ENTRY(searchEntry));
    if (strlen(q) == 0) { set_status("Enter search term"); return; }
    do_search_async(q);
}

void on_repo_activate(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn*, gpointer) {
    if (isWorking) return;
    GtkTreeIter it;
    GtkTreeModel* model = gtk_tree_view_get_model(view);
    if (!gtk_tree_model_get_iter(model, &it, path)) return;
    char* display;
    gtk_tree_model_get(model, &it, 0, &display, -1);
    if (!display) return;
    string full = display;
    g_free(display);
    size_t slash = full.find('/');
    if (slash == string::npos) return;
    currentOwner = full.substr(0, slash);
    currentRepo = full.substr(slash + 1);
    do_releases_async(currentOwner, currentRepo);
}

void on_release_select(GtkTreeSelection* sel, gpointer) {
    GtkTreeIter it;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return;
    char* tag;
    gtk_tree_model_get(model, &it, 0, &tag, -1);
    if (!tag) return;
    string tagStr = tag;
    g_free(tag);
    int idx = -1;
    for (int i = 0; i < (int)releases.size(); i++) {
        if (releases[i].tag == tagStr) { idx = i; break; }
    }
    if (idx < 0) return;

    gtk_list_store_clear(assetStore);
    for (auto& a : releases[idx].assets) {
        GtkTreeIter it2;
        gtk_list_store_append(assetStore, &it2);
        string display = a.name + "  (" + a.size + ")";
        gtk_list_store_set(assetStore, &it2, 0, display.c_str(), -1);
    }

    render_markdown(releases[idx].body);
}

void on_download(GtkWidget*, gpointer) {
    if (isWorking) return;

    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(releaseList));
    GtkTreeIter it;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        set_status("Select a release first");
        return;
    }
    char* tag;
    gtk_tree_model_get(model, &it, 0, &tag, -1);
    string tagStr = tag ? tag : "";
    g_free(tag);
    if (tagStr.empty()) return;

    vector<Asset> selected_assets;
    GtkTreeSelection* asel = gtk_tree_view_get_selection(GTK_TREE_VIEW(assetList));
    GtkTreeIter ait;
    GtkTreeModel* amodel;

    bool create_folder = true;

    if (gtk_tree_selection_get_selected(asel, &amodel, &ait)) {
        char* display;
        gtk_tree_model_get(amodel, &ait, 0, &display, -1);
        if (display) {
            string disp = display;
            g_free(display);
            size_t paren = disp.find("  (");
            string name = (paren != string::npos) ? disp.substr(0, paren) : disp;
            int idx = -1;
            for (int i = 0; i < (int)releases.size(); i++) {
                if (releases[i].tag == tagStr) { idx = i; break; }
            }
            if (idx >= 0) {
                for (auto& a : releases[idx].assets) {
                    if (a.name == name) {
                        selected_assets.push_back(a);
                        break;
                    }
                }
            }
            create_folder = false;
        }
    } else {
        int idx = -1;
        for (int i = 0; i < (int)releases.size(); i++) {
            if (releases[i].tag == tagStr) { idx = i; break; }
        }
        if (idx >= 0) {
            selected_assets = releases[idx].assets;
        }
        create_folder = true;
    }

    if (selected_assets.empty()) {
        set_status("No assets to download");
        return;
    }

    GtkWidget* fc = gtk_file_chooser_dialog_new("Select download directory", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), ".");

    int response = gtk_dialog_run(GTK_DIALOG(fc));
    if (response == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        gtk_widget_destroy(fc);
        if (path) {
            string download_path = string(path);
            g_free(path);
            do_download_async(currentOwner, currentRepo, tagStr, selected_assets, create_folder, download_path);
        }
    } else {
        gtk_widget_destroy(fc);
    }
}

void on_clone(GtkWidget*, gpointer) {
    if (isWorking) return;
    if (currentOwner.empty() || currentRepo.empty()) {
        set_status("Select a repo first");
        return;
    }
    GtkWidget* fc = gtk_file_chooser_dialog_new("Clone directory", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (path) { chdir(path); g_free(path); }
        do_clone_async(currentOwner + "/" + currentRepo);
    }
    gtk_widget_destroy(fc);
}

void on_back(GtkWidget*, gpointer) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
}

void clear_debug(GtkWidget*, gpointer) {
    if (debugBuffer) gtk_text_buffer_set_text(debugBuffer, "", -1);
}

// ============ Main ============

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    string check = exec_cmd("gh --version 2>/dev/null");
    if (check.empty()) {
        GtkWidget* dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "gh not found. Install: sudo apt install gh");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return 1;
    }

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GitHub GTK");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget* toolbar = gtk_toolbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, false, false, 0);
    spinner = gtk_spinner_new();
    gtk_widget_hide(spinner);
    GtkToolItem* spinItem = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(spinItem), spinner);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spinItem, -1);

    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, true, true, 0);

    // ===== Tab 0: Search =====
    GtkWidget* searchPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(searchPage), 10);
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("Search:"), false, false, 0);
    searchEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), "Repo name (e.g. neovim)");
    gtk_box_pack_start(GTK_BOX(hbox), searchEntry, true, true, 0);
    GtkWidget* searchBtn = gtk_button_new_with_label("Search");
    g_signal_connect(searchBtn, "clicked", G_CALLBACK(on_search), NULL);
    g_signal_connect(searchEntry, "activate", G_CALLBACK(on_search), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), searchBtn, false, false, 0);
    gtk_box_pack_start(GTK_BOX(searchPage), hbox, false, false, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(searchPage), scroll, true, true, 0);

    repoStore = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    repoList = gtk_tree_view_new_with_model(GTK_TREE_MODEL(repoStore));
    GtkCellRenderer* rend = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes("Repository", rend, "text", 0, NULL);
    gtk_tree_view_column_set_fixed_width(col, 250);
    gtk_tree_view_append_column(GTK_TREE_VIEW(repoList), col);
    col = gtk_tree_view_column_new_with_attributes("Description", rend, "text", 1, NULL);
    gtk_tree_view_column_set_expand(col, true);
    gtk_tree_view_append_column(GTK_TREE_VIEW(repoList), col);
    col = gtk_tree_view_column_new_with_attributes("Stars", rend, "text", 2, NULL);
    gtk_tree_view_column_set_fixed_width(col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(repoList), col);

    g_signal_connect(repoList, "row-activated", G_CALLBACK(on_repo_activate), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), repoList);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), searchPage, gtk_label_new("Search"));

    // ===== Tab 1: Releases =====
    GtkWidget* releasePage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(releasePage), 10);

    GtkWidget* btnBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget* backBtn = gtk_button_new_with_label("← Back");
    g_signal_connect(backBtn, "clicked", G_CALLBACK(on_back), NULL);
    gtk_box_pack_start(GTK_BOX(btnBox), backBtn, false, false, 0);
    GtkWidget* cloneBtn = gtk_button_new_with_label("Clone");
    g_signal_connect(cloneBtn, "clicked", G_CALLBACK(on_clone), NULL);
    gtk_box_pack_start(GTK_BOX(btnBox), cloneBtn, false, false, 0);
    GtkWidget* downloadBtn = gtk_button_new_with_label("Download");
    g_signal_connect(downloadBtn, "clicked", G_CALLBACK(on_download), NULL);
    gtk_box_pack_start(GTK_BOX(btnBox), downloadBtn, false, false, 0);
    gtk_box_pack_start(GTK_BOX(releasePage), btnBox, false, false, 0);

    GtkWidget* hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_size_request(hpaned, -1, 500);
    gtk_box_pack_start(GTK_BOX(releasePage), hpaned, true, true, 0);

    GtkWidget* leftBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* scrollR = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollR), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(leftBox), scrollR, true, true, 0);
    releaseStore = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    releaseList = gtk_tree_view_new_with_model(GTK_TREE_MODEL(releaseStore));
    rend = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Tag", rend, "text", 0, NULL);
    gtk_tree_view_column_set_fixed_width(col, 150);
    gtk_tree_view_append_column(GTK_TREE_VIEW(releaseList), col);
    col = gtk_tree_view_column_new_with_attributes("Date", rend, "text", 1, NULL);
    gtk_tree_view_column_set_fixed_width(col, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(releaseList), col);
    col = gtk_tree_view_column_new_with_attributes("Assets", rend, "text", 2, NULL);
    gtk_tree_view_column_set_expand(col, true);
    gtk_tree_view_append_column(GTK_TREE_VIEW(releaseList), col);
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(releaseList));
    g_signal_connect(sel, "changed", G_CALLBACK(on_release_select), NULL);
    gtk_container_add(GTK_CONTAINER(scrollR), releaseList);
    gtk_paned_pack1(GTK_PANED(hpaned), leftBox, true, false);

    GtkWidget* rightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget* assetFrame = gtk_frame_new("Assets");
    gtk_box_pack_start(GTK_BOX(rightBox), assetFrame, false, true, 0);
    GtkWidget* assetBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(assetFrame), assetBox);
    GtkWidget* scrollA = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollA), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrollA, -1, 180);
    gtk_box_pack_start(GTK_BOX(assetBox), scrollA, true, true, 0);
    assetStore = gtk_list_store_new(1, G_TYPE_STRING);
    assetList = gtk_tree_view_new_with_model(GTK_TREE_MODEL(assetStore));
    rend = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Files", rend, "text", 0, NULL);
    gtk_tree_view_column_set_expand(col, true);
    gtk_tree_view_append_column(GTK_TREE_VIEW(assetList), col);
    gtk_container_add(GTK_CONTAINER(scrollA), assetList);

    GtkWidget* bodyFrame = gtk_frame_new("Release Notes");
    gtk_box_pack_start(GTK_BOX(rightBox), bodyFrame, true, true, 0);
    GtkWidget* bodyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(bodyFrame), bodyBox);
    GtkWidget* scrollB = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollB), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(bodyBox), scrollB, true, true, 0);
    bodyWebView = webkit_web_view_new();
    g_signal_connect(bodyWebView, "mouse-target-changed",
                     G_CALLBACK(on_mouse_target_changed), NULL);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView),
                              "<p style='font-family: monospace; margin: 15px;'>Select a release</p>", NULL);
    gtk_container_add(GTK_CONTAINER(scrollB), bodyWebView);

    gtk_paned_pack2(GTK_PANED(hpaned), rightBox, true, false);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), releasePage, gtk_label_new("Releases"));

    // ===== Tab 2: Debug =====
    GtkWidget* debugPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(debugPage), 5);
    gtk_box_pack_start(GTK_BOX(debugPage), gtk_label_new("Debug:"), false, false, 0);
    GtkWidget* scrollD = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollD), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(debugPage), scrollD, true, true, 0);
    debugView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(debugView), false);
    gtk_container_add(GTK_CONTAINER(scrollD), debugView);
    debugBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(debugView));
    GtkWidget* clearBtn = gtk_button_new_with_label("Clear");
    g_signal_connect(clearBtn, "clicked", G_CALLBACK(clear_debug), NULL);
    gtk_box_pack_start(GTK_BOX(debugPage), clearBtn, false, false, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), debugPage, gtk_label_new("Debug"));

    statusBar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), statusBar, false, false, 0);
    set_status("Ready. Double-click repo.");

    progressDialog = nullptr;
    progressBar = nullptr;
    progressBar2 = nullptr;
    progressLabel = nullptr;
    progressLabel2 = nullptr;
    speedLabel = nullptr;

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
