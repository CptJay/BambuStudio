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

// Chat with Claude, which drives the app through the MCP tools (see McpServer.hpp).
// A turn runs on a worker thread because the tools wait for the UI thread.
class ChatPanel : public wxPanel
{
public:
    ChatPanel(wxWindow *parent);
    ~ChatPanel();

    void focus_input();

private:
    void send();
    void new_chat();
    bool ask_api_key();
    void run_turn(const std::string &api_key, const std::string &prompt);
    nlohmann::json post_messages(const std::string &api_key);
    void append(const wxString &text, const wxColour &colour = wxNullColour);
    void set_busy(bool busy);

    wxTextCtrl *m_transcript;
    wxTextCtrl *m_input;
    wxButton   *m_send;
    wxButton   *m_new_chat;

    nlohmann::json   m_messages = nlohmann::json::array();
    std::atomic_bool m_running{false};
    bool             m_tab_down{false};
    boost::thread    m_worker;
};

} // namespace GUI
} // namespace Slic3r

#endif
