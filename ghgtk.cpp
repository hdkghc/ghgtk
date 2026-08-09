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
string current_readme_base_url;
atomic<bool> isWorking{false};
atomic<bool> shouldStop{false};
atomic<int64_t> downloaded_bytes{0};
atomic<int> downloaded_files{0};
atomic<pid_t> gh_pid{0};
atomic<double> current_speed{0.0};
atomic<double> current_eta{0.0};
mutex releasesMutex;
mutex speed_mutex;
static bool is_dark_theme = false;
atomic<bool> is_searching{false};

GtkWidget *window;
GtkWidget *searchEntry;
GtkWidget *repoList;
GtkWidget *releaseList;
GtkWidget *assetList;
GtkWidget *statusBar;
GtkWidget *bodyWebView;
GtkWidget *readmeWebView;
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


static void detect_theme() {
    GtkSettings* settings = gtk_settings_get_default();
    gchar* theme_name = NULL;
    g_object_get(settings, "gtk-theme-name", &theme_name, NULL);
    if (theme_name) {
        string theme = string(theme_name);
        for (auto& c : theme) c = tolower(c);
        is_dark_theme = (theme.find("dark") != string::npos);
        g_free(theme_name);
    }
}

static string get_initial_html(const string& text, bool dark) {
    string color = dark ? "#8b949e" : "#666";
    string bg = dark ? "#0d1117" : "#ffffff";
    return "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><style>"
           "body { background: " + bg + "; color: " + color + "; font-family: monospace; margin: 15px; }"
           "</style></head><body><p>" + text + "</p></body></html>";
}

static string get_markdown_css(bool dark) {
    if (dark) {
        return
            "body { font-family: 'Ubuntu Sans Mono', 'Cascadia Mono', 'Consolas', monospace; font-size: 14px; line-height: 1.6; margin: 15px; padding: 0; color: #e6edf3; background: #0d1117; overflow-wrap: break-word; }\n"
            "h1 { font-size: 2em; border-bottom: 2px solid #21262d; padding-bottom: 0.3em; }\n"
            "h2 { font-size: 1.5em; border-bottom: 1px solid #21262d; padding-bottom: 0.3em; }\n"
            "h3 { font-size: 1.25em; }\n"
            "h4 { font-size: 1.1em; }\n"
            "h5 { font-size: 1.0em; }\n"
            "h6 { font-size: 0.9em; }\n"
            "p { margin: 0.5em 0; }\n"
            "pre { background: #161b22; padding: 12px; border-radius: 6px; overflow: auto; border: 1px solid #30363d; font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', monospace; font-size: 13px; white-space: pre-wrap; word-break: break-all; }\n"
            "code { background: #161b22; padding: 2px 6px; border-radius: 3px; font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', monospace; font-size: 0.9em; color: #e6edf3; }\n"
            "pre code { background: transparent; padding: 0; border-radius: 0; font-size: inherit; }\n"
            "ul, ol { padding-left: 25px; margin: 0.3em 0; }\n"
            "li { margin: 2px 0; }\n"
            "li > ul, li > ol { margin: 0; padding-left: 20px; }\n"
            "blockquote { border-left: 4px solid #30363d; padding-left: 16px; color: #8b949e; margin: 10px 0; background: #161b22; padding: 8px 16px; }\n"
            "a { color: #58a6ff; text-decoration: underline; cursor: pointer; }\n"
            "a:hover { color: #79c0ff; }\n"
            "img { max-width: 100%; }\n"
            "table { border-collapse: collapse; width: 100%; margin: 10px 0; }\n"
            "th, td { border: 1px solid #30363d; padding: 6px 13px; }\n"
            "th { background: #161b22; }\n"
            "hr { border: 0; border-top: 1px solid #21262d; margin: 15px 0; }\n";
    } else {
        return
            "body { font-family: 'Ubuntu Sans Mono', 'Cascadia Mono', 'Consolas', monospace; font-size: 14px; line-height: 1.6; margin: 15px; padding: 0; color: #24292e; background: #ffffff; overflow-wrap: break-word; }\n"
            "h1 { font-size: 2em; border-bottom: 2px solid #eaecef; padding-bottom: 0.3em; }\n"
            "h2 { font-size: 1.5em; border-bottom: 1px solid #eaecef; padding-bottom: 0.3em; }\n"
            "h3 { font-size: 1.25em; }\n"
            "h4 { font-size: 1.1em; }\n"
            "h5 { font-size: 1.0em; }\n"
            "h6 { font-size: 0.9em; }\n"
            "p { margin: 0.5em 0; }\n"
            "pre { background: #f6f8fa; padding: 12px; border-radius: 6px; overflow: auto; border: 1px solid #e1e4e8; font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', monospace; font-size: 13px; white-space: pre-wrap; word-break: break-all; }\n"
            "code { background: #f6f8fa; padding: 2px 6px; border-radius: 3px; font-family: 'Cascadia Mono', 'Ubuntu Sans Mono', monospace; font-size: 0.9em; }\n"
            "pre code { background: transparent; padding: 0; border-radius: 0; font-size: inherit; }\n"
            "ul, ol { padding-left: 25px; margin: 0.3em 0; }\n"
            "li { margin: 2px 0; }\n"
            "li > ul, li > ol { margin: 0; padding-left: 20px; }\n"
            "blockquote { border-left: 4px solid #dfe2e5; padding-left: 16px; color: #6a737d; margin: 10px 0; background: #f8f9fa; padding: 8px 16px; }\n"
            "a { color: #0366d6; text-decoration: underline; cursor: pointer; }\n"
            "a:hover { color: #0056b3; }\n"
            "img { max-width: 100%; }\n"
            "table { border-collapse: collapse; width: 100%; margin: 10px 0; }\n"
            "th, td { border: 1px solid #dfe2e5; padding: 6px 13px; }\n"
            "th { background: #f6f8fa; }\n"
            "hr { border: 0; border-top: 1px solid #eaecef; margin: 15px 0; }\n";
    }
}

// ============ Safe String ============

string safe(const string& s) {
    string r;
    r.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        if (c >= 32 && c <= 126) { r.push_back((char)c); i++; }
        else if (c == '\n' || c == '\r') { r.push_back('\n'); i++; }
        else if (c == '\t') { r.push_back('\t'); i++; }
        else if (c >= 128) {
            int len = 0;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            else { i++; continue; }
            if (i + len > s.size()) { i++; continue; }
            bool valid = true;
            for (int j = 1; j < len; j++) {
                if ((s[i+j] & 0xC0) != 0x80) { valid = false; break; }
            }
            if (valid) { r.append(s.substr(i, len)); }
            i += len;
        } else { r.push_back(' '); i++; }
    }
    return r;
}

// ============ Utility ============

string exec_cmd(const string& cmd) {
    // 如果正在搜索，且命令不是 "gh search"，则禁止执行
    if (is_searching && cmd.find("gh search") == string::npos) {
        return "";
    }
    
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
        char buf[16]; snprintf(buf, sizeof(buf), "%.1f KB", kb);
        return string(buf);
    }
    if (bytes < 1024 * 1024 * 1024) {
        double mb = bytes / (1024.0 * 1024.0);
        char buf[16]; snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return string(buf);
    }
    double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    char buf[16]; snprintf(buf, sizeof(buf), "%.2f GB", gb);
    return string(buf);
}

string format_speed(double kb_s) {
    if (kb_s < 0) return "N/A";
    if (kb_s < 1.0) {
        double b_s = kb_s * 1024;
        return to_string((int)b_s) + " B/s";
    }
    if (kb_s < 1024) { return to_string((int)kb_s) + " KB/s"; }
    double mb_s = kb_s / 1024.0;
    if (mb_s < 1024) {
        char buf[16]; snprintf(buf, sizeof(buf), "%.1f MB/s", mb_s);
        return string(buf);
    }
    double gb_s = mb_s / 1024.0;
    char buf[16]; snprintf(buf, sizeof(buf), "%.2f GB/s", gb_s);
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
        gtk_statusbar_get_context_id(GTK_STATUSBAR(statusBar), "status"), s.c_str());
}

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

// ===== Progress =====

void update_single_progress(const string& label, double frac) {
    if (!progressLabel || !progressBar) return;
    gtk_label_set_text(GTK_LABEL(progressLabel), safe(label).c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar), frac);
    if (progressLabel2) gtk_widget_hide(progressLabel2);
    if (progressBar2) gtk_widget_hide(progressBar2);
    if (speedLabel) gtk_widget_hide(speedLabel);
}

void show_single_progress(const string& label, double frac) {
    g_idle_add([](gpointer data) -> gboolean {
        auto* args = static_cast<tuple<string, double>*>(data);
        update_single_progress(get<0>(*args), get<1>(*args));
        delete args;
        return G_SOURCE_REMOVE;
    }, new tuple<string, double>(label, frac));
}

void update_double_progress(const string& label1, double frac1,
                            const string& label2, double frac2) {
    if (!progressLabel || !progressBar || !progressLabel2 || !progressBar2) return;
    gtk_label_set_text(GTK_LABEL(progressLabel), safe(label1).c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar), frac1);
    gtk_widget_show(progressLabel2);
    gtk_widget_show(progressBar2);
    gtk_label_set_text(GTK_LABEL(progressLabel2), safe(label2).c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar2), frac2);
    if (speedLabel) {
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

void show_progress_dialog(const string& title, bool single = true) {
    if (progressDialog) {
        gtk_widget_destroy(progressDialog);
        progressDialog = nullptr;
        progressBar = nullptr;
        progressBar2 = nullptr;
        progressLabel = nullptr;
        progressLabel2 = nullptr;
        speedLabel = nullptr;
    }
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
    if (!single) {
        progressLabel2 = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(content), progressLabel2, false, false, 2);
        progressBar2 = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progressBar2), 0.0);
        gtk_widget_set_size_request(progressBar2, 400, 20);
        gtk_box_pack_start(GTK_BOX(content), progressBar2, false, false, 2);
    } else {
        progressLabel2 = nullptr;
        progressBar2 = nullptr;
    }
    if (!single) {
        speedLabel = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(content), speedLabel, false, false, 2);
    } else {
        speedLabel = nullptr;
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
    if (single) show_single_progress("Starting...", 0.0);
    else {
        show_double_progress("Files: 0/0", 0.0, "Size: 0 B / 0 B", 0.0);
        gtk_label_set_text(GTK_LABEL(speedLabel), "Calculating...");
    }
    gtk_widget_show(progressDialog);
    while (gtk_events_pending()) gtk_main_iteration();
}

void hide_progress_dialog() {
    if (progressDialog) {
        gtk_widget_destroy(progressDialog);
        progressDialog = nullptr;
        progressBar = nullptr;
        progressBar2 = nullptr;
        progressLabel = nullptr;
        progressLabel2 = nullptr;
        speedLabel = nullptr;
    }
    shouldStop = false;
    downloaded_bytes = 0;
    downloaded_files = 0;
    gh_pid = 0;
    current_speed = 0.0;
    current_eta = 0.0;
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

// ============ Markdown ============


// 辅助函数：构建表格 HTML
string build_table_html(const vector<vector<string>>& rows, int header_idx) {
    if (rows.size() < 2) return "";
    
    string table = "<table>\n";
    int h = (header_idx >= 0 && header_idx < (int)rows.size()) ? header_idx : 0;
    
    // 表头
    table += "  <thead>\n    <tr>\n";
    for (auto& cell : rows[h]) {
        char* cell_html = cmark_markdown_to_html(cell.c_str(), cell.size(), CMARK_OPT_DEFAULT);
        string cell_content = cell_html ? string(cell_html) : cell;
        if (cell_html) free(cell_html);
        table += "      <th>" + cell_content + "</th>\n";
    }
    table += "    </tr>\n  </thead>\n";
    
    // 表体
    table += "  <tbody>\n";
    for (size_t j = 0; j < rows.size(); j++) {
        if (j == h) continue;
        table += "    <tr>\n";
        for (auto& cell : rows[j]) {
            char* cell_html = cmark_markdown_to_html(cell.c_str(), cell.size(), CMARK_OPT_DEFAULT);
            string cell_content = cell_html ? string(cell_html) : cell;
            if (cell_html) free(cell_html);
            table += "      <td>" + cell_content + "</td>\n";
        }
        table += "    </tr>\n";
    }
    table += "  </tbody>\n";
    table += "</table>\n";
    
    return table;
}

string render_markdown_html(const string& body) {
    string safe_body = safe(body);
    if (safe_body.empty()) return "<p><em>No content</em></p>";
    
    // 1. 先用 cmark 渲染整个文档
    char* html = cmark_markdown_to_html(safe_body.c_str(), safe_body.size(), CMARK_OPT_DEFAULT);
    string result = html ? string(html) : "";
    if (html) free(html);
    
    // 2. 检测是否有表格
    vector<string> lines;
    stringstream ss(safe_body);
    string line;
    while (getline(ss, line)) lines.push_back(line);
    
    bool has_table = false;
    bool in_code_block = false;
    for (auto& l : lines) {
        if (l.find("```") == 0) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) continue;
        size_t s = l.find_first_not_of(" \t");
        if (s == string::npos) continue;  // 跳过空行
        size_t e = l.find_last_not_of(" \t");
        string trimmed = l.substr(s, e - s + 1);
        if (trimmed.front() == '|' && trimmed.back() == '|') {
            if (trimmed.find("---") != string::npos) {
                has_table = true;
                break;
            }
        }
    }
    if (!has_table) return result;
    
    // 3. 解析表格，同时收集非表格内容
    string non_table_content;
    vector<string> table_htmls;
    vector<vector<string>> rows;
    bool in_table = false;
    int header_idx = -1;
    int table_counter = 0;
    bool last_line_empty = false;  // 追踪连续空行
    
    for (auto& l : lines) {
        // 检查是否为完全空行
        bool is_empty = true;
        for (char c : l) {
            if (c != ' ' && c != '\t' && c != '\r') {
                is_empty = false;
                break;
            }
        }
        
        if (is_empty) {
            // 空行：如果正在解析表格，结束表格
            if (in_table && rows.size() > 1) {
                string table = build_table_html(rows, header_idx);
                table_htmls.push_back(table);
                non_table_content += "<!--TABLE " + to_string(table_counter++) + "-->\n";
            }
            in_table = false;
            rows.clear();
            header_idx = -1;
            // 保留一个空行在非表格内容中
            if (!last_line_empty) {
                non_table_content += "\n";
                last_line_empty = true;
            }
            continue;
        }
        last_line_empty = false;
        
        // 检查是否为表格行
        size_t s = l.find_first_not_of(" \t");
        size_t e = l.find_last_not_of(" \t");
        string trimmed = l.substr(s, e - s + 1);
        bool is_table_line = (trimmed.front() == '|' && trimmed.back() == '|');
        
        if (!is_table_line) {
            // 非表格行：如果之前正在解析表格，结束当前表格
            if (in_table && rows.size() > 1) {
                string table = build_table_html(rows, header_idx);
                table_htmls.push_back(table);
                non_table_content += "<!--TABLE " + to_string(table_counter++) + "-->\n";
            }
            in_table = false;
            rows.clear();
            header_idx = -1;
            non_table_content += l + "\n";
            continue;
        }
        
        // 解析表格行
        string inner = trimmed.substr(1, trimmed.length() - 2);
        vector<string> cells;
        string cell;
        for (char c : inner) {
            if (c == '|') { cells.push_back(cell); cell = ""; }
            else cell += c;
        }
        if (!cell.empty() || !cells.empty()) {
            if (!cell.empty()) cells.push_back(cell);
            else if (cells.empty()) cells.push_back("");
        }
        for (auto& c : cells) {
            size_t cs = c.find_first_not_of(" \t");
            if (cs == string::npos) { c = ""; continue; }
            size_t ce = c.find_last_not_of(" \t");
            c = c.substr(cs, ce - cs + 1);
        }
        
        // 检查是否为分隔行 (|---|---|)
        bool is_separator = true;
        if (cells.empty()) {
            is_separator = false;
        } else {
            for (auto& c : cells) {
                string tc = c;
                tc.erase(remove(tc.begin(), tc.end(), ' '), tc.end());
                tc.erase(remove(tc.begin(), tc.end(), ':'), tc.end());
                if (tc.find_first_not_of('-') != string::npos) {
                    is_separator = false;
                    break;
                }
            }
        }
        
        if (is_separator) { 
            header_idx = rows.size(); 
            in_table = true; 
            continue; 
        }
        
        if (!in_table) { 
            in_table = true; 
            rows.clear(); 
            header_idx = -1; 
        }
        rows.push_back(cells);
    }
    
    // 处理文件末尾可能未结束的表格
    if (in_table && rows.size() > 1) {
        string table = build_table_html(rows, header_idx);
        table_htmls.push_back(table);
        non_table_content += "<!--TABLE " + to_string(table_counter++) + "-->\n";
    }
    
    // 4. 合并：用 cmark 渲染非表格内容，然后替换占位符
    if (!table_htmls.empty()) {
        char* non_table_html = cmark_markdown_to_html(non_table_content.c_str(), non_table_content.size(), CMARK_OPT_UNSAFE);
        string final_result = non_table_html ? string(non_table_html) : "";
        if (non_table_html) free(non_table_html);
        
        // 替换所有 HTML 注释为对应的表格 HTML
        for (int i = 0; i < (int)table_htmls.size(); i++) {
            string placeholder = "<!--TABLE " + to_string(i) + "-->";
            size_t pos = final_result.find(placeholder);
            if (pos != string::npos) {
                final_result.replace(pos, placeholder.length(), table_htmls[i]);
            } else {
                // 找不到占位符，追加在最后
                final_result += "\n" + table_htmls[i];
            }
        }
        return final_result;
    }
    
    return result;
}

void render_markdown_to_webview(GtkWidget* webView, const string& body, const string& default_msg) {
    if (!webView) return;
    string html_content = render_markdown_html(body);
    string full_html = "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"><style>\n" +
                       get_markdown_css(is_dark_theme) + "</style></head>\n<body>" + html_content + "</body>\n</html>";
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(webView), full_html.c_str(), NULL);
}

static void on_mouse_target_changed(WebKitWebView *web_view,
                                    WebKitHitTestResult *hit_test_result,
                                    guint modifiers, gpointer user_data) {
    if (webkit_hit_test_result_context_is_link(hit_test_result)) {
        const char* uri = webkit_hit_test_result_get_link_uri(hit_test_result);
        if (uri) set_status("URL: " + string(uri));
    } else set_status("Ready");
}

// ============ Fetch README ============

string fetch_readme(const string& owner, const string& repo) {
    string cmd = "gh api repos/" + owner + "/" + repo + "/readme 2>&1";
    string out = exec_cmd(cmd);
    if (out.empty()) return "";
    if (out.find("Not Found") != string::npos || out.find("404") != string::npos) return "";
    json_object* root = json_tokener_parse(out.c_str());
    if (!root) return "";
    string content;
    string html_url = "";
    json_object* contentObj;
    if (json_object_object_get_ex(root, "content", &contentObj)) {
        const char* v = json_object_get_string(contentObj);
        if (v) {
            string encoded = v;
            encoded.erase(remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
            encoded.erase(remove(encoded.begin(), encoded.end(), '\r'), encoded.end());
            string cmd2 = "echo '" + encoded + "' | base64 -d 2>/dev/null";
            content = exec_cmd(cmd2);
            if (content.empty()) {
                string cmd3 = "printf '%s' '" + encoded + "' | base64 -d 2>/dev/null";
                content = exec_cmd(cmd3);
            }
        }
    }
    json_object* htmlUrlObj;
    if (json_object_object_get_ex(root, "html_url", &htmlUrlObj)) {
        const char* v = json_object_get_string(htmlUrlObj);
        if (v) {
            html_url = safe(v);
            size_t last_slash = html_url.find_last_of('/');
            if (last_slash != string::npos) {
                current_readme_base_url = html_url.substr(0, last_slash + 1);
            } else {
                current_readme_base_url = html_url;
            }
        }
    }
    json_object_put(root);
    return content;
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
        if (!r.name.empty() && !r.owner.empty()) result.push_back(r);
        progress(0.7 + 0.3 * (i / (double)len));
    }
    json_object_put(root);
    progress(1.0);
    return result;
}

// ============ getReleases: 使用 gh api 一次获取全部 ============

vector<Release> getReleases(const string& owner, const string& repo, function<void(double)> progress) {
    vector<Release> result;
    if (shouldStop) { progress(1.0); return result; }
    progress(0.1);
    
    string cmd = "gh api repos/" + owner + "/" + repo + "/releases --paginate 2>&1";
    string out = exec_cmd(cmd);
    
    if (out.empty() || shouldStop) {
        progress(1.0);
        return result;
    }
    
    json_object* root = json_tokener_parse(out.c_str());
    if (!root) {
        progress(1.0);
        return result;
    }
    
    if (json_object_get_type(root) != json_type_array) {
        json_object_put(root);
        progress(1.0);
        return result;
    }
    
    int len = json_object_array_length(root);
    progress(0.3);
    
    for (int i = 0; i < len && !shouldStop; i++) {
        json_object* item = json_object_array_get_idx(root, i);
        Release r;
        r.total_size_bytes = 0;
        
        json_object* obj;
        if (json_object_object_get_ex(item, "tag_name", &obj)) {
            const char* v = json_object_get_string(obj);
            r.tag = v ? safe(v) : "";
        }
        if (json_object_object_get_ex(item, "published_at", &obj)) {
            const char* v = json_object_get_string(obj);
            r.published = v ? safe(v) : "";
        }
        if (json_object_object_get_ex(item, "body", &obj)) {
            const char* v = json_object_get_string(obj);
            r.body = v ? safe(v) : "";
        }
        
        if (json_object_object_get_ex(item, "assets", &obj)) {
            int alen = json_object_array_length(obj);
            for (int j = 0; j < alen && !shouldStop; j++) {
                json_object* asset = json_object_array_get_idx(obj, j);
                json_object* nameObj, *sizeObj;
                Asset asset_item;
                if (json_object_object_get_ex(asset, "name", &nameObj)) {
                    const char* v = json_object_get_string(nameObj);
                    asset_item.name = v ? safe(v) : "";
                }
                if (json_object_object_get_ex(asset, "size", &sizeObj)) {
                    int64_t size_bytes = json_object_get_int64(sizeObj);
                    asset_item.size_bytes = size_bytes;
                    asset_item.size = format_size(size_bytes);
                    r.total_size_bytes += size_bytes;
                }
                if (!asset_item.name.empty()) r.assets.push_back(asset_item);
            }
        }
        
        if (!r.tag.empty()) result.push_back(r);
        progress(0.3 + 0.7 * ((i + 1) / (double)len));
    }
    
    json_object_put(root);
    progress(1.0);
    return result;
}

// ============ Download ============

bool file_exists(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

int64_t file_size(const string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return st.st_size;
    return -1;
}

void do_download_async(const string& owner, const string& repo, const string& tag,
                       const vector<Asset>& assets, bool create_folder, const string& base_path) {
    if (isWorking) return;
    downloaded_bytes = 0; downloaded_files = 0; shouldStop = false; gh_pid = 0;
    current_speed = 0.0; current_eta = 0.0;
    set_working(true);
    show_progress_dialog("Downloading...", false);
    int total_files = assets.size();
    int64_t total_bytes = 0;
    for (auto& a : assets) total_bytes += a.size_bytes;
    thread([owner, repo, tag, assets, total_files, total_bytes, create_folder, base_path]() {
        (void)chdir(base_path.c_str());
        string target_dir = "";
        if (create_folder) { target_dir = repo + "-" + tag; mkdir(target_dir.c_str(), 0755); }
        chrono::steady_clock::time_point last_sample_time = chrono::steady_clock::now();
        int64_t last_sample_bytes = 0, accumulated_bytes = 0;
        for (int i = 0; i < total_files && !shouldStop; i++) {
            string filename = assets[i].name;
            string filepath = target_dir.empty() ? filename : target_dir + "/" + filename;
            int64_t total_size = assets[i].size_bytes;
            if (file_exists(filepath) && file_size(filepath) >= total_size) {
                downloaded_files++; downloaded_bytes += total_size; accumulated_bytes += total_size;
                double fp = downloaded_files / (double)total_files;
                double bp = (total_bytes > 0) ? (accumulated_bytes / (double)total_bytes) : fp;
                show_double_progress("Files: " + to_string(downloaded_files) + "/" + to_string(total_files), fp,
                                     "Skipped: " + filename, 1.0);
                continue;
            }
            string cmd = "gh release download -R " + owner + "/" + repo + " " + tag +
                        " --pattern '" + filename + "' --clobber 2>/dev/null";
            if (!target_dir.empty()) cmd = "cd '" + target_dir + "' && " + cmd;
            pid_t pid = fork();
            if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd.c_str(), NULL); exit(1); }
            else if (pid > 0) {
                gh_pid = pid;
                bool file_created = false;
                int64_t last_size = 0;
                int64_t initial_size = file_exists(filepath) ? file_size(filepath) : 0;
                if (initial_size > 0) last_sample_bytes = initial_size;
                while (!shouldStop) {
                    if (file_exists(filepath)) {
                        int64_t current_size = file_size(filepath);
                        if (current_size >= 0) {
                            file_created = true;
                            double single_progress = (total_size > 0) ? min(current_size / (double)total_size, 0.999) : 0.0;
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
                                    } else if (remaining_bytes <= 0) current_eta = 0.0;
                                }
                                last_sample_time = now;
                                last_sample_bytes = current_size;
                            }
                            string label2 = filename + ": " + format_size(current_size) + " / " + format_size(total_size);
                            show_double_progress(
                                "Files: " + to_string(downloaded_files) + "/" + to_string(total_files),
                                downloaded_files / (double)total_files, label2, single_progress);
                            last_size = current_size;
                            if (current_size >= total_size) break;
                        }
                    } else {
                        if (file_created) {
                            show_double_progress(
                                "Files: " + to_string(downloaded_files) + "/" + to_string(total_files),
                                downloaded_files / (double)total_files, "Waiting: " + filename, 0.0);
                        }
                    }
                    int status;
                    pid_t result = waitpid(pid, &status, WNOHANG);
                    if (result == pid) {
                        if (file_exists(filepath) && file_size(filepath) >= total_size) break;
                        if (file_created) break;
                        break;
                    }
                    this_thread::sleep_for(chrono::milliseconds(200));
                }
                waitpid(pid, NULL, 0);
                if (gh_pid == pid) gh_pid = 0;
            } else continue;
            downloaded_files++; downloaded_bytes += total_size; accumulated_bytes += total_size;
            double fp = downloaded_files / (double)total_files;
            double bp = (total_bytes > 0) ? (accumulated_bytes / (double)total_bytes) : fp;
            show_double_progress("Files: " + to_string(downloaded_files) + "/" + to_string(total_files), fp,
                                 "Done: " + filename, 1.0);
        }
        show_double_progress("Files: " + to_string(total_files) + "/" + to_string(total_files), 1.0,
                             "Size: " + format_size(total_bytes) + " / " + format_size(total_bytes), 1.0);
        if (speedLabel) gtk_label_set_text(GTK_LABEL(speedLabel), "Complete!");
        g_idle_add([](gpointer) -> gboolean {
            if (shouldStop) set_status("Download cancelled");
            else set_status("Download complete!");
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
    
    // ===== 开启搜索开关，禁止 gh api 等命令 =====
    is_searching = true;
    
    set_working(true);
    show_progress_dialog("Searching...", true);
    gtk_list_store_clear(repoStore);
    gtk_list_store_clear(releaseStore);
    gtk_list_store_clear(assetStore);
    if (bodyWebView) webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), "", NULL);
    
    thread([query]() {
        auto progress = [](double f) { show_single_progress("Searching...", f); };
        repos = searchRepos(query, progress);
        
        // ===== 搜索完成，关闭开关 =====
        is_searching = false;
        
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
    if (bodyWebView) {
        string html = get_initial_html("Loading releases...", is_dark_theme);
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), html.c_str(), NULL);
    }
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
            GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(releaseList));
            gtk_tree_selection_unselect_all(sel);
            g_signal_emit_by_name(sel, "changed", 0);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }).detach();
}

// ============ Callbacks ============

void on_search(GtkWidget*, gpointer) {
    const char* q = gtk_entry_get_text(GTK_ENTRY(searchEntry));
    if (strlen(q) == 0) { set_status("Enter search term"); return; }
    string query = string(q);
    if (query.find("url:") == 0 && query.length() > 4) {
        string url = query.substr(4);
        if (url.find("http") != 0) url = "https://" + url;
        set_status("Loading: " + url);
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(readmeWebView), url.c_str());
        return;
    }
    do_search_async(query);
}

static gboolean on_readme_load(gpointer data) {
    string* readme = static_cast<string*>(data);
    if (!readme->empty()) {
        render_markdown_to_webview(readmeWebView, *readme, "");
    } else {
        string msg = get_initial_html("No README found", is_dark_theme);
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(readmeWebView), msg.c_str(), NULL);
    }
    delete readme;
    return G_SOURCE_REMOVE;
}

void on_repo_selected_in_search(GtkTreeSelection* selection, gpointer data) {
    GtkTreeIter iter;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
    char* display;
    gtk_tree_model_get(model, &iter, 0, &display, -1);
    if (!display) return;
    string full = display;
    g_free(display);
    size_t slash = full.find('/');
    if (slash == string::npos) return;
    string owner = full.substr(0, slash);
    string repo = full.substr(slash + 1);
    current_readme_base_url = "https://github.com/" + owner + "/" + repo + "/blob/main/";
    thread([owner, repo]() {
        string readme = fetch_readme(owner, repo);
        string* readme_ptr = new string(readme);
        g_idle_add((GSourceFunc)on_readme_load, readme_ptr);
    }).detach();
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

void render_body_async(const string& owner, const string& repo, const string& tag, int idx) {
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView),
        get_initial_html("Loading release notes...", is_dark_theme).c_str(), NULL);
    thread([owner, repo, tag, idx]() {
        if (idx < (int)releases.size() && !releases[idx].body.empty()) {
            g_idle_add([](gpointer data) -> gboolean {
                int* pidx = static_cast<int*>(data);
                if (*pidx < (int)releases.size()) {
                    render_markdown_to_webview(bodyWebView, releases[*pidx].body, "");
                }
                delete pidx;
                return G_SOURCE_REMOVE;
            }, new int(idx));
            return;
        }
        string cmd = "gh release view " + tag + " -R " + owner + "/" + repo + " --json body 2>&1";
        string out = exec_cmd(cmd);
        string body;
        if (!out.empty()) {
            json_object* root = json_tokener_parse(out.c_str());
            if (root) {
                json_object* bodyObj;
                if (json_object_object_get_ex(root, "body", &bodyObj)) {
                    const char* v = json_object_get_string(bodyObj);
                    if (v) body = safe(v);
                }
                json_object_put(root);
            }
        }
        g_idle_add([](gpointer data) -> gboolean {
            auto* params = static_cast<pair<int, string>*>(data);
            int idx2 = params->first;
            string body2 = params->second;
            if (idx2 < (int)releases.size()) {
                releases[idx2].body = body2;
                if (!body2.empty()) {
                    render_markdown_to_webview(bodyWebView, body2, "");
                } else {
                    webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView),
                        get_initial_html("No release notes available", is_dark_theme).c_str(), NULL);
                }
            }
            delete params;
            return G_SOURCE_REMOVE;
        }, new pair<int, string>(idx, body));
    }).detach();
}

void on_release_select(GtkTreeSelection* sel, gpointer) {
    GtkTreeIter it;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        string html = get_initial_html("Select a release to see notes", is_dark_theme);
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), html.c_str(), NULL);
        return;
    }
    char* tag;
    gtk_tree_model_get(model, &it, 0, &tag, -1);
    if (!tag) return;
    string tagStr = tag;
    g_free(tag);
    int idx = -1;
    for (int i = 0; i < (int)releases.size(); i++) {
        if (releases[i].tag == tagStr) { idx = i; break; }
    }
    if (idx < 0) {
        string html = get_initial_html("Select a release to see notes", is_dark_theme);
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView), html.c_str(), NULL);
        return;
    }
    gtk_list_store_clear(assetStore);
    for (auto& a : releases[idx].assets) {
        GtkTreeIter it2;
        gtk_list_store_append(assetStore, &it2);
        string display = a.name + "  (" + a.size + ")";
        gtk_list_store_set(assetStore, &it2, 0, display.c_str(), -1);
    }
    if (!releases[idx].body.empty()) {
        render_markdown_to_webview(bodyWebView, releases[idx].body, "");
    } else {
        render_body_async(currentOwner, currentRepo, tagStr, idx);
    }
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
                    if (a.name == name) { selected_assets.push_back(a); break; }
                }
            }
            create_folder = false;
        }
    } else {
        int idx = -1;
        for (int i = 0; i < (int)releases.size(); i++) {
            if (releases[i].tag == tagStr) { idx = i; break; }
        }
        if (idx >= 0) selected_assets = releases[idx].assets;
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
        if (path) { (void)chdir(path); g_free(path); }
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

static gboolean on_readme_navigation_policy_decision(WebKitWebView *web_view,
                                                      WebKitNavigationPolicyDecision *decision,
                                                      gpointer user_data) {
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(decision);
    WebKitURIRequest *request = webkit_navigation_action_get_request(action);
    const char* uri = webkit_uri_request_get_uri(request);
    string uri_str = uri ? string(uri) : "";
    WebKitPolicyDecision *policy_decision = WEBKIT_POLICY_DECISION(decision);
    if (uri_str.empty() || uri_str.find("about:") == 0 || uri_str.find("data:") == 0) {
        webkit_policy_decision_use(policy_decision);
        return TRUE;
    }
    if (uri_str.find("://") == string::npos && uri_str.find("about:") != 0) {
        if (!current_readme_base_url.empty()) {
            string base = current_readme_base_url;
            string owner, repo;
            size_t start = base.find("github.com/");
            if (start != string::npos) {
                start += 11;
                size_t end = base.find("/", start);
                if (end != string::npos) {
                    owner = base.substr(start, end - start);
                    size_t start2 = end + 1;
                    size_t end2 = base.find("/", start2);
                    if (end2 != string::npos) repo = base.substr(start2, end2 - start2);
                }
            }
            if (!owner.empty() && !repo.empty()) {
                string rel_path = uri_str;
                if (rel_path.find("./") == 0) rel_path = rel_path.substr(2);
                while (rel_path.find("../") == 0) rel_path = rel_path.substr(3);
                bool is_md = (rel_path.length() >= 3 && rel_path.substr(rel_path.length() - 3) == ".md");
                bool is_dir = (!rel_path.empty() && rel_path.back() == '/');
                if (is_md) {
                    string cmd = "gh api repos/" + owner + "/" + repo + "/contents/" + rel_path + " 2>&1";
                    string out = exec_cmd(cmd);
                    json_object* root = json_tokener_parse(out.c_str());
                    if (root) {
                        json_object* contentObj;
                        string content;
                        if (json_object_object_get_ex(root, "content", &contentObj)) {
                            const char* v = json_object_get_string(contentObj);
                            if (v) {
                                string encoded = v;
                                encoded.erase(remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
                                encoded.erase(remove(encoded.begin(), encoded.end(), '\r'), encoded.end());
                                string cmd2 = "echo '" + encoded + "' | base64 -d 2>/dev/null";
                                content = exec_cmd(cmd2);
                            }
                        }
                        json_object_put(root);
                        if (!content.empty()) {
                            set_status("→ Loading: " + rel_path);
                            render_markdown_to_webview(GTK_WIDGET(web_view), content, "");
                            webkit_policy_decision_ignore(policy_decision);
                            return TRUE;
                        }
                    }
                } else if (is_dir) {
                    string cmd = "gh api repos/" + owner + "/" + repo + "/contents/" + rel_path + " 2>&1";
                    string out = exec_cmd(cmd);
                    json_object* root = json_tokener_parse(out.c_str());
                    if (root && json_object_get_type(root) == json_type_array) {
                        string html = "<h2>Directory: " + rel_path + "</h2><ul>";
                        int len = json_object_array_length(root);
                        for (int i = 0; i < len && i < 50; i++) {
                            json_object* item = json_object_array_get_idx(root, i);
                            json_object* nameObj, *typeObj;
                            string name, type;
                            if (json_object_object_get_ex(item, "name", &nameObj)) {
                                const char* n = json_object_get_string(nameObj);
                                if (n) name = n;
                            }
                            if (json_object_object_get_ex(item, "type", &typeObj)) {
                                const char* t = json_object_get_string(typeObj);
                                if (t) type = t;
                            }
                            if (!name.empty()) {
                                string icon = (type == "dir") ? "📁 " : "📄 ";
                                html += "<li>" + icon + "<a href=\"" + name + "\">" + name + "</a></li>";
                            }
                        }
                        html += "</ul>";
                        if (len >= 50) html += "<p>... and more</p>";
                        set_status("→ Directory: " + rel_path);
                        string full_html = 
                            "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><style>"
                            "body { font-family: monospace; margin: 15px; }"
                            "a { color: #0366d6; text-decoration: none; }"
                            "a:hover { text-decoration: underline; }"
                            "li { margin: 4px 0; }"
                            "</style></head><body>" + html + "</body></html>";
                        webkit_web_view_load_html(web_view, full_html.c_str(), NULL);
                        webkit_policy_decision_ignore(policy_decision);
                        json_object_put(root);
                        return TRUE;
                    }
                    if (root) json_object_put(root);
                } else {
                    string raw_url = "https://raw.githubusercontent.com/" + owner + "/" + repo + "/main/" + rel_path;
                    set_status("→ Loading raw: " + rel_path);
                    webkit_web_view_load_uri(web_view, raw_url.c_str());
                    webkit_policy_decision_ignore(policy_decision);
                    return TRUE;
                }
            }
        }
    }
    set_status("→ " + uri_str);
    webkit_policy_decision_use(policy_decision);
    return TRUE;
}

// ============ Main ============

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    string check = exec_cmd("gh --version 2>/dev/null");
    if (check.empty()) {
        GtkWidget* dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "gh not found. Install: sudo apt install gh");
        gtk_dialog_run(GTK_DIALOG(dlg)); gtk_widget_destroy(dlg);
        return 1;
    }
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GitHub GTK");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    detect_theme();
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

    GtkWidget* searchPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(searchPage), searchPaned, true, true, 0);
    GtkWidget* searchLeftBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(searchLeftBox), scroll, true, true, 0);
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
    GtkTreeSelection* repoSelection = gtk_tree_view_get_selection(GTK_TREE_VIEW(repoList));
    g_signal_connect(repoSelection, "changed", G_CALLBACK(on_repo_selected_in_search), NULL);
    g_signal_connect(repoList, "row-activated", G_CALLBACK(on_repo_activate), NULL);
    gtk_container_add(GTK_CONTAINER(scroll), repoList);
    gtk_paned_pack1(GTK_PANED(searchPaned), searchLeftBox, true, false);

    GtkWidget* searchRightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* readmeFrame = gtk_frame_new("README Preview");
    gtk_box_pack_start(GTK_BOX(searchRightBox), readmeFrame, true, true, 0);
    GtkWidget* readmeBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(readmeFrame), readmeBox);
    GtkWidget* readmeScroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(readmeScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(readmeBox), readmeScroll, true, true, 0);
    readmeWebView = webkit_web_view_new();
    g_signal_connect(readmeWebView, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
    g_signal_connect(readmeWebView, "decide-policy",
                     G_CALLBACK(on_readme_navigation_policy_decision), NULL);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(readmeWebView),
        get_initial_html("Select a repository to preview README", is_dark_theme).c_str(), NULL);
    gtk_container_add(GTK_CONTAINER(readmeScroll), readmeWebView);
    gtk_paned_pack2(GTK_PANED(searchPaned), searchRightBox, true, false);
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
    GtkWidget* releaseLeftBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* scrollR = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollR), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(releaseLeftBox), scrollR, true, true, 0);
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
    gtk_paned_pack1(GTK_PANED(hpaned), releaseLeftBox, true, false);

    GtkWidget* releaseRightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* assetFrame = gtk_frame_new("Assets");
    gtk_box_pack_start(GTK_BOX(releaseRightBox), assetFrame, false, true, 0);
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
    gtk_box_pack_start(GTK_BOX(releaseRightBox), bodyFrame, true, true, 0);
    GtkWidget* bodyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(bodyFrame), bodyBox);
    GtkWidget* scrollB = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollB), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(bodyBox), scrollB, true, true, 0);
    bodyWebView = webkit_web_view_new();
    g_signal_connect(bodyWebView, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(bodyWebView),
        get_initial_html("Select a release to see notes", is_dark_theme).c_str(), NULL);
    gtk_container_add(GTK_CONTAINER(scrollB), bodyWebView);
    gtk_paned_pack2(GTK_PANED(hpaned), releaseRightBox, true, false);
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
    set_status("Ready. Click a repo to preview README, double-click to see releases.");

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
