#ifndef slic3r_MiniMeChat_hpp_
#define slic3r_MiniMeChat_hpp_

#include <wx/panel.h>
#include <wx/timer.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxWebView;
class wxWebViewEvent;

namespace Slic3r::GUI {

struct ClaudeRun;

// SliceOfMe: a Claude chat docked at the right of MiniMe's main window. It runs Claude Code with the user's own Claude
// sign-in and gives it MiniMe's tool bridge, so Claude can look at the plate, slice and change things while the user
// talks to it. The page (resources/web/minime_chat) draws the chat; this panel runs Claude Code, signs in, lists and
// reopens earlier Claude Code chats (also those of the Claude app) and passes messages between them.
class ChatPanel : public wxPanel
{
public:
    explicit ChatPanel(wxWindow *parent);
    ~ChatPanel() override;

    // makes the page the first time the panel is shown
    void on_show();
    void focus_input();
    void sys_color_changed();
    // a line Claude Code wrote (from its reader thread, through the GUI thread)
    void on_claude_lines(int run_id, std::vector<std::string> &&lines);
    void on_claude_exit(int run_id);

private:
    void on_script_message(wxWebViewEvent &evt);
    void on_page_loaded(wxWebViewEvent &evt);
    void send_to_page(const std::string &json_text);
    void send_config();
    void check_sign_in(bool quiet);
    void start_claude(const std::string &resume_id, const std::string &cwd, bool fork);
    void stop_claude();
    bool write_line(const std::string &line);
    void list_chats();
    void open_chat(const std::string &path, bool fork);
    void attach_image();

    wxWebView                 *m_browser{nullptr};
    bool                       m_page_ready{false};
    std::vector<std::string>   m_pending;      // messages for the page before it is ready
    std::shared_ptr<ClaudeRun> m_run;
    int                        m_run_id{0};
    std::string                m_session_id;   // the chat going on (Claude Code's session id)
    std::string                m_resume_id;    // reopen this chat when Claude starts next
    std::string                m_resume_cwd;
    bool                       m_resume_fork{false};
    std::string                m_model;
    wxTimer                    m_sign_in_timer;
    int                        m_sign_in_polls{0};
};

// The thin handle at the chat's left edge; dragging it resizes the chat. 'on_drag' gets the mouse x in the parent's
// client coordinates.
class ChatSash : public wxWindow
{
public:
    ChatSash(wxWindow *parent, std::function<void(int)> on_drag);

private:
    std::function<void(int)> m_on_drag;
    bool                     m_hover{false};
};

} // namespace Slic3r::GUI

#endif // slic3r_MiniMeChat_hpp_
