#include "ChatPanel.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "McpServer.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

static const char *API_URL      = "https://api.anthropic.com/v1/messages";
static const char *MODEL        = "claude-sonnet-5-5";
static const int   MAX_ROUNDS   = 25; // tool rounds per message, so a runaway loop cannot keep spending
static const char *SYSTEM_PROMPT =
    "You are the assistant inside Bambu Studio, a 3D printing slicer. Use the tools to look at and change the plate, "
    "settings and slices for the user. Check your work with get_scene or get_view when it helps. "
    "The chat shows plain text only: do not use Markdown. Keep replies short.";

static const wxColour TOOL_COLOUR(128, 128, 128);
static const wxColour ERROR_COLOUR(220, 50, 50);

// MCP tool result content -> Anthropic tool_result content (images use a different shape).
static json to_claude_content(const json &mcp_content)
{
    json out = json::array();
    for (const json &c : mcp_content) {
        if (c.value("type", "") == "image")
            out.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", c.value("mimeType", "image/png")}, {"data", c.value("data", "")}}}});
        else
            out.push_back({{"type", "text"}, {"text", c.value("text", "")}});
    }
    return out;
}

ChatPanel::ChatPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY)
{
    m_transcript = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_WORDWRAP);
    m_input      = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, FromDIP(70)), wxTE_MULTILINE | wxTE_WORDWRAP);
    m_input->SetHint(_L("Ask Claude to change the plate... (Enter to send, Tab to hide)"));
    auto *key_btn = new wxButton(this, wxID_ANY, _L("API key..."));
    m_new_chat    = new wxButton(this, wxID_ANY, _L("New chat"));
    m_send        = new wxButton(this, wxID_ANY, _L("Send"));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(key_btn);
    buttons->AddStretchSpacer();
    buttons->Add(m_new_chat, 0, wxRIGHT, FromDIP(4));
    buttons->Add(m_send);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_transcript, 1, wxEXPAND | wxALL, FromDIP(4));
    sizer->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(4));
    SetSizer(sizer);
    wxGetApp().UpdateDarkUIWin(this);

    key_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { ask_api_key(); });
    m_new_chat->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { new_chat(); });
    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send(); });
    // Tab toggles on release, like the 3D canvas, so the release does not reach the canvas after focus moves there.
    // Key-up events go to the focused child only.
    for (wxWindow *child : GetChildren())
        child->Bind(wxEVT_KEY_UP, [this](wxKeyEvent &evt) {
            if (m_tab_down && evt.GetKeyCode() == WXK_TAB) {
                m_tab_down = false;
                wxGetApp().plater()->toggle_chat_pane();
            } else
                evt.Skip();
        });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
        if (evt.GetKeyCode() == WXK_TAB && !evt.HasAnyModifiers())
            m_tab_down = true; // also swallows auto-repeat while held
        else if ((evt.GetKeyCode() == WXK_RETURN || evt.GetKeyCode() == WXK_NUMPAD_ENTER) && !evt.ShiftDown() && FindFocus() == m_input)
            send();
        else
            evt.Skip();
    });
}

ChatPanel::~ChatPanel()
{
    // Stops the HTTP request (progress callback) and any waiting tool, then the turn ends.
    m_running = false;
    if (m_worker.joinable())
        m_worker.join();
}

void ChatPanel::focus_input() { m_input->SetFocus(); }

bool ChatPanel::ask_api_key()
{
    const wxString key = wxGetPasswordFromUser(_L("Anthropic API key (stored in the app config):"), _L("Chat"), "", this);
    if (key.IsEmpty())
        return false;
    wxGetApp().app_config->set("anthropic_api_key", into_u8(key.Strip(wxString::both)));
    return true;
}

void ChatPanel::send()
{
    if (m_running) {
        m_running = false;
        m_send->SetLabel(_L("Stopping..."));
        m_send->Disable();
        return;
    }
    std::string prompt = into_u8(m_input->GetValue());
    boost::trim(prompt);
    if (prompt.empty())
        return;
    if (wxGetApp().app_config->get("anthropic_api_key").empty() && !ask_api_key())
        return;

    m_input->Clear();
    append(_L("You") + ": " + from_u8(prompt) + "\n");
    set_busy(true);
    if (m_worker.joinable())
        m_worker.join(); // the previous turn has finished, set_busy(false) runs at its very end
    m_running = true;
    m_worker  = boost::thread([this, key = wxGetApp().app_config->get("anthropic_api_key"), prompt] { run_turn(key, prompt); });
}

void ChatPanel::new_chat()
{
    m_messages = json::array();
    m_transcript->Clear();
    m_input->SetFocus();
}

// Worker thread. Touches the UI only through CallAfter, which is dropped if the panel is gone.
void ChatPanel::run_turn(const std::string &api_key, const std::string &prompt)
{
    auto ui_append = [this](const wxString &text, const wxColour &colour = wxNullColour) { CallAfter([this, text, colour] { append(text, colour); }); };
    const json saved = m_messages;
    try {
        // After MAX_ROUNDS the history ends with tool results; the next prompt joins that user message.
        const json text = {{"type", "text"}, {"text", prompt}};
        if (!m_messages.empty() && m_messages.back()["role"] == "user")
            m_messages.back()["content"].push_back(text);
        else
            m_messages.push_back({{"role", "user"}, {"content", json::array({text})}});

        for (int round = 0;; ++round) {
            if (round == MAX_ROUNDS) {
                ui_append(wxString::Format(_L("Paused after %d tool rounds. Send a message to continue."), MAX_ROUNDS) + "\n", TOOL_COLOUR);
                break;
            }
            const json reply   = post_messages(api_key);
            const json content = reply.value("content", json::array());
            m_messages.push_back({{"role", "assistant"}, {"content", content}});

            json results = json::array();
            for (const json &block : content) {
                const std::string type = block.value("type", "");
                if (type == "text") {
                    ui_append("Claude: " + from_u8(block.value("text", "")) + "\n");
                } else if (type == "tool_use") {
                    const std::string name = block.value("name", "");
                    ui_append("-> " + from_u8(name) + "\n", TOOL_COLOUR);
                    const json out = mcp_call_tool(name, block.value("input", json::object()), m_running);
                    if (!m_running)
                        throw std::runtime_error(_u8L("Stopped."));
                    results.push_back({{"type", "tool_result"},
                                       {"tool_use_id", block.value("id", "")},
                                       {"content", to_claude_content(out.value("content", json::array()))},
                                       {"is_error", out.value("isError", false)}});
                }
            }
            if (!results.empty())
                m_messages.push_back({{"role", "user"}, {"content", results}});
            if (reply.value("stop_reason", "") != "tool_use")
                break;
        }
    } catch (const std::exception &e) {
        // Drop the whole turn so the history stays valid for the API (no tool_use without its result).
        m_messages = saved;
        ui_append(from_u8(m_running ? e.what() : _u8L("Stopped.")) + "\n", ERROR_COLOUR);
    }
    m_running = false;
    // A new turn may have started before this runs.
    CallAfter([this] { if (!m_running) set_busy(false); });
}

json ChatPanel::post_messages(const std::string &api_key)
{
    json tools = json::array();
    for (const json &t : mcp_tool_list())
        tools.push_back({{"name", t["name"]}, {"description", t["description"]}, {"input_schema", t["inputSchema"]}});
    // Cache the tools and the history so far; both repeat on every request.
    tools.back()["cache_control"] = {{"type", "ephemeral"}};
    json messages = m_messages;
    messages.back()["content"].back()["cache_control"] = {{"type", "ephemeral"}};

    const json body = {{"model", MODEL}, {"max_tokens", 8192}, {"system", SYSTEM_PROMPT}, {"tools", tools}, {"messages", messages}};

    std::string response, error;
    auto http = Http::post(API_URL);
    // The app adds Bambu headers (device id, ...) to every request; they are not for this server.
    for (const auto &header : Http::get_extra_headers())
        http.remove_header(header.first);
    http.header("x-api-key", api_key)
        .header("anthropic-version", "2023-06-01")
        .header("content-type", "application/json")
        .set_post_body(body.dump(-1, ' ', false, json::error_handler_t::replace))
        .timeout_max(600)
        .on_progress([this](Http::Progress, bool &cancel) { cancel = !m_running; })
        .on_complete([&response](std::string b, unsigned) { response = std::move(b); })
        .on_error([&error](std::string b, std::string e, unsigned status) {
            const json j = json::parse(b, nullptr, false);
            error = j.is_object() && j.contains("error") ? j["error"].value("message", b) : (e.empty() ? b : e);
            if (status != 0)
                error = "HTTP " + std::to_string(status) + ": " + error;
        })
        .perform_sync();

    if (!m_running)
        throw std::runtime_error(_u8L("Stopped."));
    if (!error.empty())
        throw std::runtime_error(error);
    const json reply = json::parse(response, nullptr, false);
    if (!reply.is_object())
        throw std::runtime_error("Unexpected reply from the API");
    BOOST_LOG_TRIVIAL(debug) << "chat: stop_reason " << reply.value("stop_reason", "") << ", usage " << reply.value("usage", json::object()).dump();
    return reply;
}

void ChatPanel::append(const wxString &text, const wxColour &colour)
{
    m_transcript->SetDefaultStyle(wxTextAttr(colour.IsOk() ? colour : m_transcript->GetForegroundColour()));
    m_transcript->AppendText(text);
}

void ChatPanel::set_busy(bool busy)
{
    m_send->SetLabel(busy ? _L("Stop") : _L("Send"));
    m_send->Enable();
    m_new_chat->Enable(!busy);
}

} // namespace GUI
} // namespace Slic3r
