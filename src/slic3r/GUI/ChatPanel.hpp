#ifndef slic3r_ChatPanel_hpp_
#define slic3r_ChatPanel_hpp_

#include <atomic>

#include <boost/thread.hpp>
#include <wx/panel.h>

#include "nlohmann/json.hpp"

class wxButton;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

// Chat with an LLM (Claude, OpenAI, local models...), which drives the app through the MCP tools (see McpServer.hpp).
// A turn runs on a worker thread because the tools wait for the UI thread.
class ChatPanel : public wxPanel
{
public:
    // Provider, model, key and URL from the app config.
    struct Config;

    ChatPanel(wxWindow *parent);
    ~ChatPanel();

    void focus_input();

private:
    void send();
    void new_chat();
    bool open_settings();
    void update_hint();
    void run_turn(const Config &cfg, const std::string &prompt);
    nlohmann::json post(const Config &cfg, const nlohmann::json &body);
    void append(const wxString &text, const wxColour &colour = wxNullColour);
    void set_busy(bool busy);

    wxTextCtrl *m_transcript;
    wxTextCtrl *m_input;
    wxButton   *m_settings;
    wxButton   *m_send;
    wxButton   *m_new_chat;

    nlohmann::json   m_messages = nlohmann::json::array(); // in the current provider's API format
    std::atomic_bool m_running{false};
    bool             m_tab_down{false};
    boost::thread    m_worker;
};

} // namespace GUI
} // namespace Slic3r

#endif
