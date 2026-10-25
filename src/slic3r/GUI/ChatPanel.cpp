#include "ChatPanel.hpp"

#include <fstream>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "McpServer.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

enum class KeyField { None, Optional, Required };
enum class Api { Anthropic, OpenAI, ClaudeCode };

struct ChatProvider
{
    const char *id;
    const char *name;
    Api         api;
    const char *url;       // empty: set in the settings (Custom's URL, Claude Code's path)
    const char *model;     // default, editable in the settings
    KeyField    key;
    // shortcut: per provider, not per model; a vision model on a local server gets no images. Add a setting if that is needed.
    bool        vision;
};

// Default models change often; they are only a starting point in the settings dialog.
static const std::vector<ChatProvider> PROVIDERS = {
    // Runs the user's Claude Code CLI, so its login (and subscription) is used; the tools go through the MCP server.
    {"claude_code", "Claude (subscription, via Claude Code)", Api::ClaudeCode, "", "", KeyField::None, true},
    {"claude", "Claude (API key)", Api::Anthropic, "https://api.anthropic.com/v1", "claude-sonnet-5-5", KeyField::Required, true},
    {"openai", "OpenAI", Api::OpenAI, "https://api.openai.com/v1", "gpt-5", KeyField::Required, true},
    {"gemini", "Gemini", Api::OpenAI, "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-flash-latest", KeyField::Required, true},
    {"mistral", "Mistral", Api::OpenAI, "https://api.mistral.ai/v1", "mistral-large-latest", KeyField::Required, false},
    {"groq", "Groq", Api::OpenAI, "https://api.groq.com/openai/v1", "openai/gpt-oss-120b", KeyField::Required, false},
    {"openrouter", "OpenRouter", Api::OpenAI, "https://openrouter.ai/api/v1", "openrouter/auto", KeyField::Required, false},
    {"ollama", "Ollama (local)", Api::OpenAI, "http://localhost:11434/v1", "qwen3", KeyField::None, false},
    {"lmstudio", "LM Studio (local)", Api::OpenAI, "http://localhost:1234/v1", "", KeyField::None, false},
    {"custom", "Custom (OpenAI-compatible)", Api::OpenAI, "", "", KeyField::Optional, false},
};

static const int   MAX_ROUNDS = 25; // tool rounds per message, so a runaway loop cannot keep spending
static const char *SYSTEM_PROMPT =
    "You are the assistant inside Bambu Studio, a 3D printing slicer. Use the tools to look at and change the plate, "
    "settings and slices for the user. Check your work with get_scene or get_view when it helps. "
    "The chat shows plain text only: do not use Markdown. Keep replies short.";

static const wxColour TOOL_COLOUR(128, 128, 128);
static const wxColour ERROR_COLOUR(220, 50, 50);

static std::string config_get(const std::string &key, const std::string &fallback = "")
{
    const std::string value = wxGetApp().app_config->get(key);
    return value.empty() ? fallback : value;
}

static std::string provider_key(const ChatProvider &p)
{
    // Before the provider list, the Claude key was stored as anthropic_api_key.
    return config_get(std::string("chat_") + p.id + "_key", p.id == std::string("claude") ? config_get("anthropic_api_key") : "");
}

static const ChatProvider &current_provider()
{
    const std::string id = config_get("chat_provider", PROVIDERS.front().id);
    for (const ChatProvider &p : PROVIDERS)
        if (id == p.id)
            return p;
    return PROVIDERS.front();
}

// The installed Claude Code CLI: on the PATH, else where the native installer puts it
// (an app started from the desktop menu may not have ~/.local/bin on its PATH).
static std::string find_claude_code()
{
    const boost::filesystem::path on_path = boost::process::search_path("claude");
    if (!on_path.empty())
        return on_path.string();
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
    const boost::filesystem::path installed = boost::filesystem::path(home ? home : "") / ".local" / "bin" / "claude.exe";
#else
    const char *home = std::getenv("HOME");
    const boost::filesystem::path installed = boost::filesystem::path(home ? home : "") / ".local" / "bin" / "claude";
#endif
    boost::system::error_code ec;
    return boost::filesystem::exists(installed, ec) ? installed.string() : "";
}

struct ChatPanel::Config
{
    const ChatProvider *provider;
    std::string         url, model, key;

    static Config load()
    {
        const ChatProvider &p = current_provider();
        std::string url = p.url[0] ? std::string(p.url) : config_get(std::string("chat_") + p.id + "_url", p.api == Api::ClaudeCode ? find_claude_code() : "");
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        return {&p, url, config_get(std::string("chat_") + p.id + "_model", p.model), provider_key(p)};
    }
    // Claude Code without a model uses its own default.
    bool complete() const { return !url.empty() && (!model.empty() || provider->api == Api::ClaudeCode) && (provider->key != KeyField::Required || !key.empty()); }
    wxString speaker() const { return provider->api == Api::ClaudeCode ? wxString("Claude") : provider->url[0] ? wxString(provider->name) : _L("Assistant"); }
};

// Returns true when the settings were saved.
static bool edit_settings(wxWindow *parent)
{
    wxDialog dlg(parent, wxID_ANY, _L("Chat settings"));
    auto *provider  = new wxChoice(&dlg, wxID_ANY);
    auto *model     = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxSize(dlg.FromDIP(300), -1));
    auto *key_label = new wxStaticText(&dlg, wxID_ANY, _L("API key"));
    auto *key       = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    auto *url_label = new wxStaticText(&dlg, wxID_ANY, _L("Base URL"));
    auto *url       = new wxTextCtrl(&dlg, wxID_ANY);
    for (const ChatProvider &p : PROVIDERS)
        provider->Append(from_u8(p.name));

    auto *grid = new wxFlexGridSizer(2, dlg.FromDIP(6), dlg.FromDIP(8));
    grid->AddGrowableCol(1);
    for (auto [label, field] : std::initializer_list<std::pair<wxWindow *, wxWindow *>>{
             {new wxStaticText(&dlg, wxID_ANY, _L("Provider")), provider}, {new wxStaticText(&dlg, wxID_ANY, _L("Model")), model}, {key_label, key}, {url_label, url}}) {
        grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(field, 1, wxEXPAND);
    }
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(grid, 1, wxEXPAND | wxALL, dlg.FromDIP(12));
    auto *note = new wxStaticText(&dlg, wxID_ANY, "");
    note->SetForegroundColour(TOOL_COLOUR);
    sizer->Add(note, 0, wxLEFT | wxRIGHT, dlg.FromDIP(12));
    sizer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, dlg.FromDIP(12));
    dlg.SetSizer(sizer);

    auto load = [&](int i) {
        const ChatProvider &p = PROVIDERS[i];
        model->SetValue(from_u8(config_get(std::string("chat_") + p.id + "_model", p.model)));
        const bool claude_code = p.api == Api::ClaudeCode;
        model->SetHint(p.id == std::string("lmstudio") ? _L("Model id shown in LM Studio") : claude_code ? _L("Claude Code's default (or sonnet, opus...)") : wxString());
        key->SetValue(from_u8(provider_key(p)));
        url->SetValue(from_u8(config_get(std::string("chat_") + p.id + "_url")));
        url_label->SetLabel(claude_code ? _L("Claude Code path") : _L("Base URL"));
        const std::string found = claude_code ? find_claude_code() : "";
        url->SetHint(claude_code ? (found.empty() ? _L("Not found: install Claude Code or enter its path") : from_u8(found)) : wxString("http://localhost:8080/v1"));
        note->SetLabel(claude_code ? _L("Uses your Claude Code login: no API key, usage counts against your Claude plan.") :
                                     _L("Keys are stored unencrypted in the app config, one per provider."));
        key_label->Show(p.key != KeyField::None);
        key->Show(p.key != KeyField::None);
        url_label->Show(!p.url[0]);
        url->Show(!p.url[0]);
        dlg.Fit();
    };
    provider->SetSelection(int(&current_provider() - PROVIDERS.data()));
    load(provider->GetSelection());
    provider->Bind(wxEVT_CHOICE, [&](wxCommandEvent &) { load(provider->GetSelection()); });
    wxGetApp().UpdateDlgDarkUI(&dlg);

    if (dlg.ShowModal() != wxID_OK)
        return false;
    const ChatProvider &p   = PROVIDERS[provider->GetSelection()];
    AppConfig          *cfg = wxGetApp().app_config;
    cfg->set("chat_provider", p.id);
    cfg->set(std::string("chat_") + p.id + "_model", into_u8(model->GetValue().Strip(wxString::both)));
    if (p.key != KeyField::None)
        cfg->set(std::string("chat_") + p.id + "_key", into_u8(key->GetValue().Strip(wxString::both)));
    if (!p.url[0])
        cfg->set(std::string("chat_") + p.id + "_url", into_u8(url->GetValue().Strip(wxString::both)));
    return true;
}

// --- The two API formats. The history is kept in the provider's own format. ---

struct ToolCall
{
    std::string id, name;
    json        input; // null when the model sent arguments that are not a JSON object
};

struct Reply
{
    std::vector<std::string> texts;
    std::vector<ToolCall>    calls;
    bool                     wants_results; // the model waits for the tool results
};

static void add_prompt(const ChatPanel::Config &cfg, json &history, const std::string &prompt)
{
    if (cfg.provider->api != Api::Anthropic) {
        history.push_back({{"role", "user"}, {"content", prompt}});
        return;
    }
    // After MAX_ROUNDS the history ends with tool results; Anthropic wants the prompt in that same user message.
    const json text = {{"type", "text"}, {"text", prompt}};
    if (!history.empty() && history.back()["role"] == "user")
        history.back()["content"].push_back(text);
    else
        history.push_back({{"role", "user"}, {"content", json::array({text})}});
}

static json request_body(const ChatPanel::Config &cfg, const json &history)
{
    json tools = json::array();
    if (cfg.provider->api == Api::Anthropic) {
        for (const json &t : mcp_tool_list())
            tools.push_back({{"name", t["name"]}, {"description", t["description"]}, {"input_schema", t["inputSchema"]}});
        // Cache the tools and the history so far; both repeat on every request.
        tools.back()["cache_control"] = {{"type", "ephemeral"}};
        json messages = history;
        messages.back()["content"].back()["cache_control"] = {{"type", "ephemeral"}};
        return {{"model", cfg.model}, {"max_tokens", 8192}, {"system", SYSTEM_PROMPT}, {"tools", tools}, {"messages", messages}};
    }
    for (const json &t : mcp_tool_list())
        tools.push_back({{"type", "function"}, {"function", {{"name", t["name"]}, {"description", t["description"]}, {"parameters", t["inputSchema"]}}}});
    json messages = json::array({{{"role", "system"}, {"content", SYSTEM_PROMPT}}});
    messages.insert(messages.end(), history.begin(), history.end());
    // No max_tokens: newer OpenAI models reject it, and the servers' defaults are fine.
    return {{"model", cfg.model}, {"tools", tools}, {"messages", messages}};
}

// Adds the assistant message to the history.
static Reply read_reply(const ChatPanel::Config &cfg, json &history, const json &reply)
{
    Reply out;
    if (cfg.provider->api == Api::Anthropic) {
        const json content = reply.value("content", json::array());
        history.push_back({{"role", "assistant"}, {"content", content}});
        for (const json &block : content) {
            if (block.value("type", "") == "text")
                out.texts.push_back(block.value("text", ""));
            else if (block.value("type", "") == "tool_use")
                out.calls.push_back({block.value("id", ""), block.value("name", ""), block.value("input", json::object())});
        }
        out.wants_results = reply.value("stop_reason", "") == "tool_use";
        return out;
    }
    if (!reply.contains("choices") || reply["choices"].empty() || !reply["choices"][0].contains("message"))
        throw std::runtime_error("Unexpected reply from the API: " + reply.dump().substr(0, 300));
    const json &msg     = reply["choices"][0]["message"];
    json        message = {{"role", "assistant"}, {"content", msg.contains("content") ? msg["content"] : json()}};
    if (msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty())
        out.texts.push_back(msg["content"].get<std::string>());
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
        message["tool_calls"] = msg["tool_calls"];
        for (const json &call : msg["tool_calls"]) {
            const json &fn   = call.value("function", json::object());
            const json  args = fn.contains("arguments") && fn["arguments"].is_string() ? json::parse(fn["arguments"].get<std::string>(), nullptr, false) :
                                                                                         fn.value("arguments", json::object());
            out.calls.push_back({call.value("id", ""), fn.value("name", ""), args.is_object() ? args : json()});
        }
    }
    history.push_back(message);
    // Local servers do not all set finish_reason to "tool_calls"; the calls themselves are what counts.
    out.wants_results = !out.calls.empty();
    return out;
}

// results[i] is the MCP tool result ({content, isError}) of calls[i].
static void add_results(const ChatPanel::Config &cfg, json &history, const std::vector<ToolCall> &calls, const std::vector<json> &results)
{
    if (cfg.provider->api == Api::Anthropic) {
        json blocks = json::array();
        for (size_t i = 0; i < calls.size(); ++i) {
            json content = json::array();
            for (const json &c : results[i].value("content", json::array())) {
                if (c.value("type", "") == "image")
                    content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", c.value("mimeType", "image/png")}, {"data", c.value("data", "")}}}});
                else
                    content.push_back({{"type", "text"}, {"text", c.value("text", "")}});
            }
            blocks.push_back({{"type", "tool_result"}, {"tool_use_id", calls[i].id}, {"content", content}, {"is_error", results[i].value("isError", false)}});
        }
        history.push_back({{"role", "user"}, {"content", blocks}});
        return;
    }
    // Tool messages are text only; images follow in one user message.
    json images = json::array();
    for (size_t i = 0; i < calls.size(); ++i) {
        std::string text = results[i].value("isError", false) ? "Error: " : "";
        for (const json &c : results[i].value("content", json::array())) {
            if (c.value("type", "") != "image")
                text += c.value("text", "");
            else if (!cfg.provider->vision)
                text += "[image omitted: images are not sent to this provider]";
            else {
                text += "[image attached in the next message]";
                images.push_back({{"type", "text"}, {"text", "Image returned by " + calls[i].name + ":"}});
                images.push_back({{"type", "image_url"}, {"image_url", {{"url", "data:" + c.value("mimeType", "image/png") + ";base64," + c.value("data", "")}}}});
            }
        }
        history.push_back({{"role", "tool"}, {"tool_call_id", calls[i].id}, {"content", text}});
    }
    if (!images.empty())
        history.push_back({{"role", "user"}, {"content", images}});
}

// --- Panel ---

ChatPanel::ChatPanel(wxWindow *parent) : wxPanel(parent, wxID_ANY)
{
    m_transcript = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_WORDWRAP);
    m_input      = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, FromDIP(70)), wxTE_MULTILINE | wxTE_WORDWRAP);
    m_settings   = new wxButton(this, wxID_ANY, _L("Settings..."));
    m_new_chat   = new wxButton(this, wxID_ANY, _L("New chat"));
    m_send       = new wxButton(this, wxID_ANY, _L("Send"));
    update_hint();

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(m_settings);
    buttons->AddStretchSpacer();
    buttons->Add(m_new_chat, 0, wxRIGHT, FromDIP(4));
    buttons->Add(m_send);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_transcript, 1, wxEXPAND | wxALL, FromDIP(4));
    sizer->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(4));
    SetSizer(sizer);
    wxGetApp().UpdateDarkUIWin(this);

    m_settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { open_settings(); });
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
    // Stops the HTTP request (progress callback), Claude Code and any waiting tool, then the turn ends.
    m_running = false;
    kill_claude_code();
    if (m_worker.joinable())
        m_worker.join();
}

void ChatPanel::focus_input() { m_input->SetFocus(); }

void ChatPanel::update_hint()
{
    m_input->SetHint(wxString::Format(_L("Ask %s to change the plate... (Enter to send, Tab to hide)"), Config::load().speaker()));
}

bool ChatPanel::open_settings()
{
    const ChatProvider *before = Config::load().provider;
    if (!edit_settings(this))
        return false;
    // The history is in the old provider's format.
    if (Config::load().provider != before && (!m_messages.empty() || !m_session.empty())) {
        new_chat();
        append(_L("Provider changed: started a new chat.") + "\n", TOOL_COLOUR);
    }
    update_hint();
    return true;
}

void ChatPanel::send()
{
    if (m_running) {
        m_running = false;
        kill_claude_code();
        m_send->SetLabel(_L("Stopping..."));
        m_send->Disable();
        return;
    }
    std::string prompt = into_u8(m_input->GetValue());
    boost::trim(prompt);
    if (prompt.empty())
        return;
    if (!Config::load().complete() && (!open_settings() || !Config::load().complete()))
        return;

    m_input->Clear();
    append(_L("You") + ": " + from_u8(prompt) + "\n");
    set_busy(true);
    if (m_worker.joinable())
        m_worker.join(); // the previous turn has finished, set_busy(false) runs at its very end
    m_running = true;
    m_worker  = boost::thread([this, cfg = Config::load(), prompt] { run_turn(cfg, prompt); });
}

void ChatPanel::new_chat()
{
    m_messages = json::array();
    m_session.clear();
    m_transcript->Clear();
    m_input->SetFocus();
}

// Worker thread. Touches the UI only through CallAfter, which is dropped if the panel is gone.
void ChatPanel::run_turn(const Config &cfg, const std::string &prompt)
{
    auto ui_append = [this](const wxString &text, const wxColour &colour = wxNullColour) { CallAfter([this, text, colour] { append(text, colour); }); };
    const json saved = m_messages;
    try {
        if (cfg.provider->api == Api::ClaudeCode) {
            run_claude_code(cfg, prompt);
            finish_turn();
            return;
        }
        add_prompt(cfg, m_messages, prompt);
        for (int round = 0;; ++round) {
            if (round == MAX_ROUNDS) {
                ui_append(wxString::Format(_L("Paused after %d tool rounds. Send a message to continue."), MAX_ROUNDS) + "\n", TOOL_COLOUR);
                break;
            }
            const Reply reply = read_reply(cfg, m_messages, post(cfg, request_body(cfg, m_messages)));
            for (const std::string &text : reply.texts)
                ui_append(cfg.speaker() + ": " + from_u8(text) + "\n");

            std::vector<json> results;
            for (const ToolCall &call : reply.calls) {
                ui_append("-> " + from_u8(call.name) + "\n", TOOL_COLOUR);
                results.push_back(call.input.is_null() ? json{{"content", json::array({{{"type", "text"}, {"text", "The arguments were not a valid JSON object."}}})}, {"isError", true}} :
                                                         mcp_call_tool(call.name, call.input, m_running));
                if (!m_running)
                    throw std::runtime_error(_u8L("Stopped."));
            }
            if (!results.empty())
                add_results(cfg, m_messages, reply.calls, results);
            if (!reply.wants_results)
                break;
        }
    } catch (const std::exception &e) {
        // Drop the whole turn so the history stays valid for the API (no tool call without its result).
        m_messages = saved;
        ui_append(from_u8(m_running ? e.what() : _u8L("Stopped.")) + "\n", ERROR_COLOUR);
    }
    finish_turn();
}

void ChatPanel::finish_turn()
{
    m_running = false;
    // A new turn may have started before this runs.
    CallAfter([this] { if (!m_running) set_busy(false); });
}

void ChatPanel::kill_claude_code()
{
    std::lock_guard<std::mutex> lock(m_kill_mutex);
    if (m_kill)
        m_kill();
}

// Worker thread. One Claude Code run per message; --resume continues the conversation.
void ChatPanel::run_claude_code(const Config &cfg, const std::string &prompt)
{
    namespace bp = boost::process;
    namespace fs = boost::filesystem;
    // An empty folder, so Claude Code picks up no project CLAUDE.md.
    const fs::path dir = fs::path(data_dir()) / "chat";
    fs::create_directories(dir);
    const fs::path log = dir / "claude_code_stderr.log";

    const json mcp = {{"mcpServers", {{"bambu", {{"type", "http"}, {"url", "http://127.0.0.1:" + std::to_string(MCP_PORT) + "/mcp"}}}}}};
    // Only the slicer tools (no shell, files or web), allowed without prompts; no user settings, hooks or plugins.
    std::vector<std::string> args = {"-p", "--output-format", "stream-json", "--verbose", "--tools", "", "--setting-sources", "",
                                     "--disable-slash-commands", "--strict-mcp-config", "--mcp-config", mcp.dump(), "--allowedTools", "mcp__bambu",
                                     "--append-system-prompt", SYSTEM_PROMPT};
    if (!cfg.model.empty())
        args.insert(args.end(), {"--model", cfg.model});
    if (!m_session.empty())
        args.insert(args.end(), {"--resume", m_session});

    bp::opstream in;
    bp::ipstream out;
    bp::child    child(fs::path(cfg.url), bp::args(args), bp::std_in < in, bp::std_out > out, bp::std_err > log, bp::start_dir(dir));
    {
        std::lock_guard<std::mutex> lock(m_kill_mutex);
        m_kill = [&child] { std::error_code ec; child.terminate(ec); };
        // Stop pressed before m_kill was set.
        if (!m_running)
            m_kill();
    }
    struct ClearKill
    {
        ChatPanel *panel;
        ~ClearKill() { std::lock_guard<std::mutex> lock(panel->m_kill_mutex); panel->m_kill = nullptr; }
    } clear_kill{this};
    // On stdin, so a message starting with '-' is not read as an option.
    in << prompt;
    in.flush();
    in.pipe().close();

    json        result;
    std::string line;
    while (std::getline(out, line)) {
        const json event = json::parse(line, nullptr, false);
        if (!event.is_object())
            continue;
        if (event.contains("session_id") && event["session_id"].is_string())
            m_session = event["session_id"].get<std::string>();
        const std::string type = event.value("type", "");
        if (type == "result")
            result = event;
        else if (type == "assistant" && event.contains("message"))
            for (const json &block : event["message"].value("content", json::array())) {
                if (block.value("type", "") == "text")
                    CallAfter([this, text = cfg.speaker() + ": " + from_u8(block.value("text", "")) + "\n"] { append(text); });
                else if (block.value("type", "") == "tool_use") {
                    std::string name = block.value("name", "");
                    if (boost::starts_with(name, "mcp__bambu__"))
                        name = name.substr(12);
                    CallAfter([this, text = "-> " + from_u8(name) + "\n"] { append(text, TOOL_COLOUR); });
                }
            }
    }
    std::error_code ec;
    child.wait(ec);

    if (!m_running)
        throw std::runtime_error(_u8L("Stopped."));
    if (result.is_null()) {
        std::ifstream     f(log.string());
        std::stringstream err;
        err << f.rdbuf();
        throw std::runtime_error("Claude Code stopped without an answer (exit code " + std::to_string(child.exit_code()) + "): " + err.str().substr(0, 500));
    }
    if (result.value("is_error", false))
        throw std::runtime_error("Claude Code: " + (result.contains("result") && result["result"].is_string() ? result["result"].get<std::string>() : result.value("subtype", "error")));
}

json ChatPanel::post(const Config &cfg, const json &body)
{
    std::string response, error;
    auto http = Http::post(cfg.url + (cfg.provider->api == Api::Anthropic ? "/messages" : "/chat/completions"));
    // The app adds Bambu headers (device id, ...) to every request; they are not for this server.
    for (const auto &header : Http::get_extra_headers())
        http.remove_header(header.first);
    if (cfg.provider->api == Api::Anthropic)
        http.header("x-api-key", cfg.key).header("anthropic-version", "2023-06-01");
    else if (!cfg.key.empty())
        http.header("Authorization", "Bearer " + cfg.key);
    http.header("content-type", "application/json")
        .set_post_body(body.dump(-1, ' ', false, json::error_handler_t::replace))
        .timeout_max(600)
        .on_progress([this](Http::Progress, bool &cancel) { cancel = !m_running; })
        .on_complete([&response](std::string b, unsigned) { response = std::move(b); })
        .on_error([&error](std::string b, std::string e, unsigned status) {
            const json j = json::parse(b, nullptr, false);
            error = j.is_object() && j.contains("error") && j["error"].is_object() ? j["error"].value("message", b) : (b.empty() ? e : b);
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
    BOOST_LOG_TRIVIAL(debug) << "chat: " << cfg.provider->id << " usage " << reply.value("usage", json::object()).dump();
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
    m_settings->Enable(!busy); // a turn holds the history in the current provider's format
}

} // namespace GUI
} // namespace Slic3r
