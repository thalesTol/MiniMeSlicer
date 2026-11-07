#include "MiniMeChat.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MiniMeBridge.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/base64.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
// some GUI header defines pid_t as a macro, which breaks Boost.Process (MediaPlayCtrl.cpp does the same)
#undef pid_t
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace Slic3r::GUI {

namespace bp = boost::process;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

ChatPanel *s_panel = nullptr; // the chat panel, for what comes back from worker threads

// The prompt Claude gets on top of Claude Code's own.
const char *CHAT_PROMPT =
    "You are chatting with the user inside MiniMe Slicer, a slicer for FDM 3D printing (a fork of Bambu Studio). This chat "
    "is a panel at the right of MiniMe's window: the user sees MiniMe's 3D view and this chat side by side, and talks "
    "about the plate in front of them.\n"
    "- The minime tools work on the open MiniMe. Each change is one undo step; the user can also undo it with Ctrl+Z.\n"
    "- Work from MiniMe's data, not from pictures. get_scene lists every object and part with its filament, height range on "
    "the plate, painted filaments (with their heights) and mesh errors. After slice, the layers tool tells which filaments each "
    "layer prints and how many filament changes there are. Base every statement about the model on these numbers; if they do "
    "not tell, say you do not know rather than guess. Use render_view to show the user something, not to find things out.\n"
    "- After every change, read the result back (get_scene, or slice + layers) and check it before you say it worked. Never say "
    "something is done unless a tool confirmed it; if a step failed or you cannot do it, say so plainly.\n"
    "- Make shapes with MiniMe's tools (add_part: box, rounded box, cylinder, a slab with an object's outline; cut_object with "
    "result=parts_in_place; assemble_objects; move_part; set_filament; delete) instead of writing scripts or files.\n"
    "- Colors only on the outer layers (fewer filament changes): set the object's interior_filament and interior_skin_layers "
    "with set_settings (object). Everything deeper inside then prints with that filament, over paint and parts.\n"
    "- If MiniMe lacks something the user needs, say exactly what is missing so it can be added.\n"
    "- Change only what the user asked for, or what you proposed and they accepted. You cannot print or send anything to a "
    "printer.\n"
    "- Answer in the user's language, short and practical, in plain words; millimetres and grams; give print times like "
    "1 h 12 min. Use short lists and bold for the important number; no long reports.\n";

std::string utf8(const fs::path &p) { return boost::nowide::narrow(p.wstring()); }
fs::path    path_of(const std::string &s) { return fs::path(boost::nowide::widen(s)); }

std::string env_var(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

fs::path home_dir() { return path_of(wxGetHomeDir().ToUTF8().data()); }

// Claude Code keeps its settings, sign-in and chats here
fs::path claude_config_dir()
{
    const std::string own = env_var("CLAUDE_CONFIG_DIR");
    return own.empty() ? home_dir() / ".claude" : path_of(own);
}

// "2.1.317" > "2.1.309"
bool newer_version(const std::string &a, const std::string &b)
{
    auto parts = [](const std::string &s) {
        std::vector<int> v;
        size_t           i = 0;
        while (i < s.size()) {
            size_t j = s.find('.', i);
            if (j == std::string::npos)
                j = s.size();
            v.push_back(std::atoi(s.substr(i, j - i).c_str()));
            i = j + 1;
        }
        return v;
    };
    return parts(a) > parts(b);
}

// Claude Code's program: the user's choice, then PATH, then the copy that comes with the Claude desktop app.
fs::path find_claude()
{
    boost::system::error_code ec;
    if (AppConfig *config = wxGetApp().app_config) {
        const std::string own = config->get("minime_claude_path");
        if (!own.empty() && fs::is_regular_file(path_of(own), ec))
            return path_of(own);
    }
#ifdef _WIN32
    const char *exe_name = "claude.exe";
#else
    const char *exe_name = "claude";
#endif
    {
        fs::path p = bp::search_path(exe_name);
        if (!p.empty())
            return p;
    }
    // the Claude desktop app: <app data>/Claude/claude-code/<version>/<build>/claude(.exe)
    std::vector<fs::path> roots;
#ifdef _WIN32
    if (!env_var("APPDATA").empty())
        roots.push_back(path_of(env_var("APPDATA")) / "Claude" / "claude-code");
#elif __APPLE__
    roots.push_back(home_dir() / "Library" / "Application Support" / "Claude" / "claude-code");
#else
    roots.push_back(home_dir() / ".config" / "Claude" / "claude-code");
#endif
    fs::path    best;
    std::string best_version;
    for (const fs::path &root : roots) {
        if (!fs::is_directory(root, ec))
            continue;
        for (fs::directory_iterator v(root, ec), end; !ec && v != end; v.increment(ec)) {
            if (!fs::is_directory(v->path(), ec))
                continue;
            const std::string version = v->path().filename().string();
            for (fs::directory_iterator b(v->path(), ec), bend; !ec && b != bend; b.increment(ec)) {
                const fs::path exe = b->path() / exe_name;
                if (fs::is_regular_file(exe, ec) && (best.empty() || newer_version(version, best_version))) {
                    best         = exe;
                    best_version = version;
                }
            }
            ec.clear();
        }
    }
    if (!best.empty())
        return best;
    for (const fs::path &p : {home_dir() / ".local" / "bin" / exe_name, home_dir() / ".claude" / "local" / exe_name})
        if (fs::is_regular_file(p, ec))
            return p;
    return {};
}

// where the chat runs: MiniMe's own folder for Claude, with the tool bridge's address and the chat prompt
fs::path chat_dir()
{
    fs::path dir = path_of(data_dir()) / "claude-chat";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

bool write_text(const fs::path &file, const std::string &text)
{
    boost::nowide::ofstream out(utf8(file).c_str(), std::ios::binary | std::ios::trunc);
    out << text;
    return bool(out);
}

// Claude Code's folder for the chats of one working folder: every character but letters and digits becomes '-'
std::string project_key(const std::string &cwd)
{
    std::string k = cwd;
    for (char &c : k)
        if (!std::isalnum((unsigned char) c))
            c = '-';
    return k;
}

std::string base64_of(const std::string &bytes) { return wxBase64Encode(bytes.data(), bytes.size()).ToStdString(); }

// what a chat says first or the title it was given
struct ChatInfo
{
    std::string path, id, cwd, title, first_prompt;
    std::time_t when{0};
};

std::string prompt_text(const json &message)
{
    const json &content = message.contains("content") ? message["content"] : json();
    std::string text;
    if (content.is_string())
        text = content.get<std::string>();
    else if (content.is_array())
        for (const json &b : content)
            if (b.value("type", "") == "text") {
                text = b.value("text", "");
                break;
            }
    // commands, hook notes and the like are not what the user said
    if (text.rfind("<", 0) == 0 || text.rfind("Caveat:", 0) == 0)
        return {};
    return text;
}

void scan_lines(const std::string &text, const std::function<bool(const json &)> &take)
{
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos)
            j = text.size();
        if (j > i + 2) {
            json e = json::parse(text.begin() + i, text.begin() + j, nullptr, false);
            if (!e.is_discarded() && !take(e))
                return;
        }
        i = j + 1;
    }
}

ChatInfo read_chat_info(const fs::path &file)
{
    ChatInfo info;
    info.path = utf8(file);
    info.id   = file.stem().string();
    boost::system::error_code ec;
    info.when = fs::last_write_time(file, ec);
    const uintmax_t size = fs::file_size(file, ec);
    boost::nowide::ifstream in(info.path.c_str(), std::ios::binary);
    if (!in)
        return info;
    // the start: the folder and the first thing the user said
    std::string head(size_t(std::min<uintmax_t>(size, 256 * 1024)), '\0');
    in.read(head.data(), std::streamsize(head.size()));
    head.resize(size_t(in.gcount()));
    scan_lines(head, [&](const json &e) {
        const std::string type = e.value("type", "");
        if (type == "custom-title")
            info.title = e.value("customTitle", "");
        if (info.cwd.empty() && e.contains("cwd") && e["cwd"].is_string())
            info.cwd = e["cwd"].get<std::string>();
        if (info.first_prompt.empty() && type == "user" && !e.value("isSidechain", false) && !e.value("isMeta", false) &&
            e.contains("message"))
            info.first_prompt = prompt_text(e["message"]);
        return info.cwd.empty() || info.first_prompt.empty();
    });
    // the end: the latest title
    if (size > head.size()) {
        const uintmax_t tail_size = std::min<uintmax_t>(size - head.size(), 128 * 1024);
        in.clear();
        in.seekg(std::streamoff(size - tail_size));
        std::string tail(size_t(tail_size), '\0');
        in.read(tail.data(), std::streamsize(tail.size()));
        tail.resize(size_t(in.gcount()));
        scan_lines(tail, [&](const json &e) {
            if (e.value("type", "") == "custom-title")
                info.title = e.value("customTitle", "");
            return true;
        });
    }
    return info;
}

// Big pictures and long tool output are left out when an earlier chat is shown again.
void slim_content(json &content)
{
    if (!content.is_array())
        return;
    for (json &b : content) {
        const std::string type = b.value("type", "");
        if (type == "image" && b.contains("source") && b["source"].contains("data") &&
            b["source"]["data"].is_string() && b["source"]["data"].get_ref<const std::string &>().size() > 160 * 1024) {
            b["source"]["data"] = "";
            b["omitted"]        = true;
        } else if (type == "tool_result") {
            if (b.contains("content") && b["content"].is_string() && b["content"].get_ref<const std::string &>().size() > 4000)
                b["content"] = b["content"].get<std::string>().substr(0, 4000) + " …";
            else if (b.contains("content"))
                slim_content(b["content"]);
        } else if (type == "text" && b.contains("text") && b["text"].is_string() &&
                   b["text"].get_ref<const std::string &>().size() > 20000) {
            b["text"] = b["text"].get<std::string>().substr(0, 20000) + " …";
        } else if (type == "thinking") {
            b["thinking"] = "";
            b.erase("signature");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// lines from Claude Code, collected on its reader thread and handed to the GUI thread in batches

std::mutex                                     s_lines_mutex;
std::vector<std::pair<int, std::string>>       s_lines;
bool                                           s_flush_posted = false;

void post_line(int run_id, std::string &&line)
{
    std::lock_guard<std::mutex> lock(s_lines_mutex);
    s_lines.emplace_back(run_id, std::move(line));
    if (s_flush_posted)
        return;
    s_flush_posted = true;
    wxGetApp().CallAfter([] {
        std::vector<std::pair<int, std::string>> lines;
        {
            std::lock_guard<std::mutex> lock(s_lines_mutex);
            lines.swap(s_lines);
            s_flush_posted = false;
        }
        if (s_panel == nullptr || lines.empty())
            return;
        // in runs of the same process
        size_t i = 0;
        while (i < lines.size()) {
            const int                run = lines[i].first;
            std::vector<std::string> batch;
            for (; i < lines.size() && lines[i].first == run; ++i)
                batch.push_back(std::move(lines[i].second));
            s_panel->on_claude_lines(run, std::move(batch));
        }
    });
}

} // namespace

// A running Claude Code. Its reader threads keep it alive until its output ends.
struct ClaudeRun
{
    int          id{0};
    bp::opstream in;
    bp::ipstream out, err;
    bp::child    child;
    std::mutex   write_mutex;
    std::mutex   err_mutex;
    std::deque<std::string> err_tail;

    std::string stderr_tail()
    {
        std::lock_guard<std::mutex> lock(err_mutex);
        std::string s;
        for (const std::string &l : err_tail)
            s += l + "\n";
        return s;
    }
};

// ---------------------------------------------------------------------------------------------------------------

ChatPanel::ChatPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    s_panel = this;
    SetSizer(new wxBoxSizer(wxVERTICAL));
    SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(18, 22, 31) : wxColour(247, 248, 251));
    if (AppConfig *config = wxGetApp().app_config)
        m_model = config->get("minime_chat_model");

    m_sign_in_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (++m_sign_in_polls > 100) // about 5 minutes
            m_sign_in_timer.Stop();
        check_sign_in(true);
    });
}

ChatPanel::~ChatPanel()
{
    m_sign_in_timer.Stop();
    stop_claude();
    if (s_panel == this)
        s_panel = nullptr;
    ClaudeBridge::set_needed_by_chat(false);
}

void ChatPanel::on_show()
{
    if (m_browser != nullptr) {
        focus_input();
        return;
    }
    wxString url = wxString::Format("file://%s/web/minime_chat/index.html", from_u8(resources_dir()));
    url.Replace("\\", "/");
    m_browser = WebView::CreateWebView(this, url, "minime_chat");
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "MiniMe chat: cannot create the web view";
        return;
    }
    GetSizer()->Add(m_browser, 1, wxEXPAND);
    Layout();
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ChatPanel::on_script_message, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &ChatPanel::on_page_loaded, this, m_browser->GetId());
    // links open in the user's browser, never inside the chat
    Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent &evt) {
        const wxString target = evt.GetURL();
        if (target.StartsWith("file:") || target.StartsWith("about:") || target.StartsWith("data:"))
            return;
        evt.Veto();
        if (target.StartsWith("http://") || target.StartsWith("https://"))
            wxLaunchDefaultBrowser(target);
    }, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent &evt) {
        const wxString target = evt.GetURL();
        if (target.StartsWith("http://") || target.StartsWith("https://"))
            wxLaunchDefaultBrowser(target);
    }, m_browser->GetId());
}

void ChatPanel::on_page_loaded(wxWebViewEvent &) {}

void ChatPanel::focus_input()
{
    if (m_browser != nullptr) {
        m_browser->SetFocus();
        send_to_page(R"({"kind":"focus"})");
    }
}

void ChatPanel::sys_color_changed()
{
    SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(18, 22, 31) : wxColour(247, 248, 251));
    if (m_page_ready)
        send_config();
}

void ChatPanel::send_to_page(const std::string &json_text)
{
    if (m_browser == nullptr)
        return;
    if (!m_page_ready) {
        m_pending.push_back(json_text);
        return;
    }
    WebView::RunScript(m_browser, wxString::FromUTF8("window.mm && window.mm.recv(" + json_text + ");"));
}

void ChatPanel::send_config()
{
    const fs::path claude = find_claude();
    json c = {{"kind", "config"},
              {"dark", wxGetApp().dark_mode()},
              {"model", m_model},
              {"claude", claude.empty() ? json() : json(utf8(claude))},
              {"chat_dir", utf8(chat_dir())},
              {"session_id", m_session_id}};
    send_to_page(c.dump());
}

// ---------------------------------------------------------------------------------------------------------------
// sign-in

void ChatPanel::check_sign_in(bool quiet)
{
    const fs::path claude = find_claude();
    if (claude.empty()) {
        send_to_page(R"({"kind":"auth","state":"no_claude"})");
        return;
    }
    if (!quiet)
        send_to_page(R"({"kind":"auth","state":"checking"})");
    std::thread([claude] {
        json answer = {{"kind", "auth"}, {"state", "unknown"}};
        try {
            bp::ipstream out;
            bp::child    c(claude, std::vector<std::wstring>{L"auth", L"status"}, bp::std_out > out, bp::std_err > bp::null,
                           bp::std_in < bp::null, bp::limit_handles
#ifdef _WIN32
                           , bp::windows::create_no_window
#endif
            );
            std::string text, line;
            while (std::getline(out, line))
                text += line + "\n";
            c.wait();
            json status = json::parse(text, nullptr, false);
            if (status.is_object()) {
                const bool in = status.value("loggedIn", false);
                answer["state"] = in ? "signed_in" : "signed_out";
                for (const char *key : {"email", "subscriptionType", "authMethod", "orgName"})
                    if (status.contains(key) && status[key].is_string())
                        answer[key] = status[key];
            }
        } catch (const std::exception &e) {
            answer["detail"] = e.what();
        }
        wxGetApp().CallAfter([answer] {
            if (s_panel == nullptr)
                return;
            if (answer["state"] == "signed_in")
                s_panel->m_sign_in_timer.Stop();
            s_panel->send_to_page(answer.dump());
        });
    }).detach();
}

// ---------------------------------------------------------------------------------------------------------------
// the Claude Code process

void ChatPanel::start_claude(const std::string &resume_id, const std::string &cwd_in, bool fork)
{
    stop_claude();
    const fs::path claude = find_claude();
    if (claude.empty()) {
        send_to_page(R"({"kind":"auth","state":"no_claude"})");
        return;
    }
    // MiniMe's tools for this chat
    ClaudeBridge::set_needed_by_chat(true);
    for (int i = 0; i < 60 && ClaudeBridge::port() == 0 && ClaudeBridge::running(); ++i)
        wxMilliSleep(50);
    const int port = ClaudeBridge::port();
    if (port == 0) {
        json e = {{"kind", "error"}, {"text", "MiniMe's tool bridge did not start: " + ClaudeBridge::status()}};
        send_to_page(e.dump());
    }
    const fs::path dir = chat_dir();
    const fs::path mcp = dir / "minime-mcp.json";
    json mcp_config = {{"mcpServers", {{"minime", {{"type", "http"}, {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"}}}}}};
    write_text(mcp, mcp_config.dump(2));

    fs::path cwd = cwd_in.empty() ? dir : path_of(cwd_in);
    boost::system::error_code ec;
    if (!fs::is_directory(cwd, ec))
        cwd = dir;

    std::vector<std::wstring> args = {L"-p",
                                      L"--input-format", L"stream-json",
                                      L"--output-format", L"stream-json",
                                      L"--verbose",
                                      L"--include-partial-messages",
                                      // Claude Code asks MiniMe (control_request can_use_tool) before tools that need approval
                                      L"--permission-prompt-tool", L"stdio",
                                      L"--mcp-config", mcp.wstring(),
                                      L"--strict-mcp-config",
                                      L"--append-system-prompt", boost::nowide::widen(CHAT_PROMPT),
                                      L"--allowedTools", L"mcp__minime", L"Read", L"Glob", L"Grep", L"WebSearch", L"TodoWrite"};
    if (!m_model.empty()) {
        args.push_back(L"--model");
        args.push_back(boost::nowide::widen(m_model));
    }
    if (!resume_id.empty()) {
        args.push_back(L"--resume");
        args.push_back(boost::nowide::widen(resume_id));
        if (fork)
            args.push_back(L"--fork-session");
    } else {
        args.push_back(L"--name");
        args.push_back(L"MiniMe chat");
    }

    auto run = std::make_shared<ClaudeRun>();
    run->id  = ++m_run_id;
    try {
        // started from inside another Claude Code (while developing), it must not think it is nested in it
        bp::wenvironment env = boost::this_process::wenvironment();
        env.erase(L"CLAUDECODE");
        run->child = bp::child(claude, args, bp::start_dir(cwd), bp::std_out > run->out, bp::std_err > run->err,
                               bp::std_in < run->in, env, bp::limit_handles
#ifdef _WIN32
                               , bp::windows::create_no_window
#endif
        );
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MiniMe chat: cannot start Claude Code " << utf8(claude) << ": " << e.what();
        json err = {{"kind", "exit"}, {"code", -1}, {"stderr", std::string("Claude Code could not be started: ") + e.what()}};
        send_to_page(err.dump());
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "MiniMe chat: Claude Code started (" << utf8(claude) << ", folder " << utf8(cwd)
                            << (resume_id.empty() ? std::string() : ", chat " + resume_id) << ")";
    m_run = run;
    const int id = run->id;
    std::thread([run, id] {
        std::string line;
        while (std::getline(run->out, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                post_line(id, std::move(line));
        }
        wxGetApp().CallAfter([id] {
            if (s_panel)
                s_panel->on_claude_exit(id);
        });
    }).detach();
    std::thread([run] {
        std::string line;
        while (std::getline(run->err, line)) {
            BOOST_LOG_TRIVIAL(info) << "MiniMe chat (Claude Code): " << line;
            std::lock_guard<std::mutex> lock(run->err_mutex);
            run->err_tail.push_back(line);
            if (run->err_tail.size() > 12)
                run->err_tail.pop_front();
        }
    }).detach();

    // the SDK handshake (it also tells the models)
    write_line(R"({"type":"control_request","request_id":"minime-init","request":{"subtype":"initialize"}})");
    send_to_page(R"({"kind":"started"})");
}

void ChatPanel::stop_claude()
{
    if (!m_run)
        return;
    std::shared_ptr<ClaudeRun> run = std::move(m_run);
    m_run.reset();
    {
        std::lock_guard<std::mutex> lock(run->write_mutex);
        try {
            run->in.pipe().close();
        } catch (...) {
        }
    }
    std::error_code ec;
    if (run->child.running(ec))
        run->child.terminate(ec);
}

bool ChatPanel::write_line(const std::string &line)
{
    if (!m_run)
        return false;
    std::lock_guard<std::mutex> lock(m_run->write_mutex);
    try {
        m_run->in << line << '\n';
        m_run->in.flush();
        return bool(m_run->in);
    } catch (...) {
        return false;
    }
}

void ChatPanel::on_claude_lines(int run_id, std::vector<std::string> &&lines)
{
    if (!m_run || run_id != m_run->id)
        return;
    std::string out = R"({"kind":"claude","lines":[)";
    bool        first = true;
    for (const std::string &line : lines) {
        json msg = json::parse(line, nullptr, false);
        if (msg.is_discarded()) {
            BOOST_LOG_TRIVIAL(info) << "MiniMe chat (Claude Code, not JSON): " << line.substr(0, 300);
            continue;
        }
        const std::string type = msg.value("type", "");
        if (type == "system" && msg.value("subtype", "") == "init")
            m_session_id = msg.value("session_id", "");
        if (type == "result" && msg.contains("session_id") && msg["session_id"].is_string())
            m_session_id = msg["session_id"].get<std::string>();
        if (!first)
            out += ',';
        first = false;
        out += msg.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    out += "]}";
    if (!first)
        send_to_page(out);
}

void ChatPanel::on_claude_exit(int run_id)
{
    if (!m_run || run_id != m_run->id)
        return;
    std::shared_ptr<ClaudeRun> run = std::move(m_run);
    m_run.reset();
    int             code = -1;
    std::error_code ec;
    if (run->child.running(ec))
        run->child.terminate(ec);
    else
        code = run->child.exit_code();
    // the error lines may still be on their way
    wxMilliSleep(50);
    json e = {{"kind", "exit"}, {"code", code}, {"stderr", run->stderr_tail()}};
    BOOST_LOG_TRIVIAL(info) << "MiniMe chat: Claude Code ended with " << code;
    send_to_page(e.dump());
}

// ---------------------------------------------------------------------------------------------------------------
// earlier chats

void ChatPanel::list_chats()
{
    const fs::path projects = claude_config_dir() / "projects";
    const std::string mine   = project_key(utf8(chat_dir()));
    std::thread([projects, mine] {
        boost::system::error_code ec;
        std::vector<std::pair<std::time_t, fs::path>> files;
        if (fs::is_directory(projects, ec))
            for (fs::directory_iterator d(projects, ec), end; !ec && d != end; d.increment(ec)) {
                if (!fs::is_directory(d->path(), ec))
                    continue;
                for (fs::directory_iterator f(d->path(), ec), fend; !ec && f != fend; f.increment(ec))
                    if (f->path().extension() == ".jsonl" && f->path().filename().string().rfind("agent-", 0) != 0)
                        files.emplace_back(fs::last_write_time(f->path(), ec), f->path());
                ec.clear();
            }
        std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        if (files.size() > 60)
            files.resize(60);
        json chats = json::array();
        for (const auto &[when, file] : files) {
            ChatInfo info = read_chat_info(file);
            if (info.first_prompt.empty() && info.title.empty())
                continue;
            const bool own = file.parent_path().filename().string() == mine;
            chats.push_back({{"path", info.path},
                             {"id", info.id},
                             {"cwd", info.cwd},
                             {"title", info.title.empty() ? info.first_prompt.substr(0, 200) : info.title},
                             {"when", (long long) info.when},
                             {"mine", own}});
        }
        json answer = {{"kind", "chats"}, {"chats", chats}};
        wxGetApp().CallAfter([answer] {
            if (s_panel)
                s_panel->send_to_page(answer.dump(-1, ' ', false, json::error_handler_t::replace));
        });
    }).detach();
}

void ChatPanel::open_chat(const std::string &path, bool fork)
{
    const fs::path file = path_of(path);
    // only Claude Code's own chat files
    boost::system::error_code ec;
    const fs::path projects = fs::canonical(claude_config_dir() / "projects", ec);
    const fs::path real     = fs::canonical(file, ec);
    if (ec || file.extension() != ".jsonl" || real.parent_path().parent_path() != projects) {
        send_to_page(R"({"kind":"error","text":"That is not a Claude Code chat."})");
        return;
    }
    stop_claude();
    m_session_id.clear();
    send_to_page(R"({"kind":"loading_chat"})");
    std::thread([file, fork] {
        ChatInfo           info = read_chat_info(file);
        std::deque<json>   entries;
        boost::nowide::ifstream in(utf8(file).c_str(), std::ios::binary);
        std::string        line;
        while (std::getline(in, line)) {
            // a quick look before parsing: most lines of a long chat are not messages
            if (line.find("\"type\":\"user\"") == std::string::npos && line.find("\"type\":\"assistant\"") == std::string::npos)
                continue;
            json e = json::parse(line, nullptr, false);
            if (e.is_discarded() || e.value("isSidechain", false) || e.value("isMeta", false) || !e.contains("message"))
                continue;
            const std::string type = e.value("type", "");
            if (type != "user" && type != "assistant")
                continue;
            json m = e["message"];
            if (m.contains("content"))
                slim_content(m["content"]);
            json slim = {{"role", type}, {"content", m.contains("content") ? m["content"] : json()}};
            if (m.contains("id"))
                slim["id"] = m["id"];
            entries.push_back(std::move(slim));
            if (entries.size() > 300)
                entries.pop_front();
        }
        json answer = {{"kind", "transcript"},
                       {"id", info.id},
                       {"cwd", info.cwd},
                       {"fork", fork},
                       {"title", info.title.empty() ? info.first_prompt.substr(0, 200) : info.title},
                       {"entries", json(entries)}};
        wxGetApp().CallAfter([answer, id = info.id, cwd = info.cwd, fork] {
            if (s_panel == nullptr)
                return;
            s_panel->m_resume_id   = id;
            s_panel->m_resume_cwd  = cwd;
            s_panel->m_resume_fork = fork;
            s_panel->send_to_page(answer.dump(-1, ' ', false, json::error_handler_t::replace));
        });
    }).detach();
}

void ChatPanel::attach_image()
{
    wxFileDialog dlg(this, _L("Attach a picture"), wxEmptyString, wxEmptyString,
                     "Pictures (*.png;*.jpg;*.jpeg;*.gif;*.webp)|*.png;*.jpg;*.jpeg;*.gif;*.webp", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = dlg.GetPath().ToUTF8().data();
    boost::nowide::ifstream in(path.c_str(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 5 * 1024 * 1024) {
        send_to_page(R"({"kind":"error","text":"Pictures can be up to 5 MB."})");
        return;
    }
    std::string ext = path_of(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    const std::string media = ext == ".png" ? "image/png" : ext == ".gif" ? "image/gif" : ext == ".webp" ? "image/webp" : "image/jpeg";
    json a = {{"kind", "attachment"}, {"name", utf8(path_of(path).filename())}, {"media_type", media}, {"data", base64_of(bytes)}};
    send_to_page(a.dump());
}

// ---------------------------------------------------------------------------------------------------------------
// messages from the page

void ChatPanel::on_script_message(wxWebViewEvent &evt)
{
    json msg = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
    if (!msg.is_object())
        return;
    const std::string cmd = msg.value("cmd", "");
    if (cmd == "ready") {
        m_page_ready = true;
        std::vector<std::string> pending;
        pending.swap(m_pending);
        send_config();
        for (const std::string &p : pending)
            send_to_page(p);
        check_sign_in(false);
    } else if (cmd == "send") {
        if (!msg.contains("content") || !msg["content"].is_array())
            return;
        if (!m_run) {
            const std::string resume = m_resume_id.empty() ? m_session_id : m_resume_id;
            start_claude(resume, m_resume_cwd, m_resume_fork);
            m_resume_id.clear();
            m_resume_fork = false;
        }
        json user = {{"type", "user"},
                     {"message", {{"role", "user"}, {"content", msg["content"]}}},
                     {"parent_tool_use_id", nullptr},
                     {"session_id", m_session_id}};
        if (!write_line(user.dump(-1, ' ', false, json::error_handler_t::replace)))
            send_to_page(R"({"kind":"error","text":"Claude Code is not running."})");
    } else if (cmd == "interrupt") {
        static int n = 0;
        write_line(json({{"type", "control_request"}, {"request_id", "minime-stop-" + std::to_string(++n)}, {"request", {{"subtype", "interrupt"}}}}).dump());
    } else if (cmd == "permission") {
        json response = msg.value("allow", false) ?
                            json({{"behavior", "allow"}, {"updatedInput", msg.contains("input") ? msg["input"] : json::object()}}) :
                            json({{"behavior", "deny"}, {"message", msg.value("message", std::string("The user said no."))}});
        write_line(json({{"type", "control_response"},
                         {"response", {{"subtype", "success"}, {"request_id", msg.value("request_id", "")}, {"response", response}}}})
                       .dump(-1, ' ', false, json::error_handler_t::replace));
    } else if (cmd == "control_error") {
        write_line(json({{"type", "control_response"},
                         {"response", {{"subtype", "error"}, {"request_id", msg.value("request_id", "")}, {"error", "MiniMe does not support this"}}}})
                       .dump());
    } else if (cmd == "new_chat") {
        stop_claude();
        m_session_id.clear();
        m_resume_id.clear();
        m_resume_cwd.clear();
        m_resume_fork = false;
    } else if (cmd == "list_chats") {
        list_chats();
    } else if (cmd == "open_chat") {
        open_chat(msg.value("path", ""), msg.value("fork", false));
    } else if (cmd == "sign_in") {
        const fs::path claude = find_claude();
        if (claude.empty())
            return;
        try {
            // in a window of its own: it opens the browser to sign in, then waits
            bp::spawn(claude, std::vector<std::wstring>{L"auth", L"login"}, bp::start_dir(chat_dir()));
        } catch (const std::exception &e) {
            json err = {{"kind", "error"}, {"text", std::string("Could not start the sign-in: ") + e.what()}};
            send_to_page(err.dump());
            return;
        }
        m_sign_in_polls = 0;
        m_sign_in_timer.Start(3000);
    } else if (cmd == "check_sign_in") {
        check_sign_in(msg.value("quiet", false));
    } else if (cmd == "set_model") {
        m_model = msg.value("model", "");
        if (AppConfig *config = wxGetApp().app_config)
            config->set("minime_chat_model", m_model);
        if (m_run) {
            json request = {{"subtype", "set_model"}};
            if (!m_model.empty())
                request["model"] = m_model;
            write_line(json({{"type", "control_request"}, {"request_id", "minime-model"}, {"request", request}}).dump());
        }
    } else if (cmd == "open_link") {
        const std::string url = msg.value("url", "");
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
            wxLaunchDefaultBrowser(wxString::FromUTF8(url));
    } else if (cmd == "close") {
        if (MainFrame *frame = wxGetApp().mainframe)
            frame->show_chat(false);
    } else if (cmd == "attach_image") {
        attach_image();
    } else if (cmd == "stop") {
        stop_claude();
    }
}

// ---------------------------------------------------------------------------------------------------------------

ChatSash::ChatSash(wxWindow *parent, std::function<void(int)> on_drag)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(5), -1), wxBORDER_NONE), m_on_drag(std::move(on_drag))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_SIZEWE));
    SetMinSize(wxSize(FromDIP(5), -1));
    Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        const bool dark = wxGetApp().dark_mode();
        const wxColour back = dark ? wxColour(18, 22, 31) : wxColour(247, 248, 251);
        const wxColour line = m_hover || HasCapture() ? wxColour(30, 110, 240) : (dark ? wxColour(44, 52, 68) : wxColour(220, 224, 232));
        dc.SetBackground(wxBrush(back));
        dc.Clear();
        const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(line));
        const int w = m_hover || HasCapture() ? FromDIP(2) : 1;
        dc.DrawRectangle(0, 0, w, sz.y);
    });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &) { m_hover = true; Refresh(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { m_hover = false; Refresh(); });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
        if (!HasCapture())
            CaptureMouse();
        Refresh();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (HasCapture())
            ReleaseMouse();
        Refresh();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { Refresh(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) {
        if (HasCapture() && evt.LeftIsDown() && m_on_drag)
            m_on_drag(GetParent()->ScreenToClient(ClientToScreen(evt.GetPosition())).x);
    });
}

} // namespace Slic3r::GUI
