#ifndef slic3r_McpServer_hpp_
#define slic3r_McpServer_hpp_

#include <atomic>

#include <boost/asio.hpp>
#include <boost/thread.hpp>

#include "nlohmann/json.hpp"

#define MCP_PORT 13619

namespace Slic3r {
namespace GUI {

// The MCP tools, also used in-process by the chat pane. mcp_call_tool must not run on the UI thread:
// tools wait for the UI thread to run their work. Returns an MCP tool result ({content, isError}).
nlohmann::json mcp_tool_list();
nlohmann::json mcp_call_tool(const std::string &name, const nlohmann::json &args, const std::atomic_bool &running);

// Model Context Protocol server (Streamable HTTP, JSON responses only) on http://127.0.0.1:MCP_PORT/mcp.
// Requests are handled on a worker thread; tools run on the UI thread via CallAfter.
class McpServer
{
public:
    ~McpServer() { stop(); }

    void start();
    void stop();

private:
    void do_accept();

    std::atomic_bool               m_running{false};
    boost::asio::io_context        m_io{1};
    boost::asio::ip::tcp::acceptor m_acceptor{m_io};
    boost::thread                  m_worker;
};

} // namespace GUI
} // namespace Slic3r

#endif
