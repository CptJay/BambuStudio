#ifndef slic3r_McpServer_hpp_
#define slic3r_McpServer_hpp_

#include <atomic>

#include <boost/asio.hpp>
#include <boost/thread.hpp>

#define MCP_PORT 13619

namespace Slic3r {
namespace GUI {

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
