#ifndef slic3r_MiniMeBridge_hpp_
#define slic3r_MiniMeBridge_hpp_

#include <string>

namespace Slic3r::GUI {

// SliceOfMe: lets Claude (Claude Code, or any other MCP client on this computer) use MiniMe through tools: look at
// the plate, render the view, slice, read the support results, change settings, move, cut and paint objects,
// paint plate zones, undo. It is an MCP server over HTTP that listens on 127.0.0.1 only and is off unless switched on
// in Preferences. Every change it makes is one undo step; it can never print or send anything to a printer.
class ClaudeBridge
{
public:
    static constexpr int DEFAULT_PORT = 13700;
    static constexpr const char *CONFIG_KEY = "minime_claude_bridge";

    // starts or stops it after the app preference (it also stays on while MiniMe's own Claude chat needs it)
    static void apply_preference();
    // listens on 'port', or with tries > 1 on the first free one of port .. port + tries - 1
    static void start(int port = DEFAULT_PORT, int tries = 1);
    static void stop();
    static bool running();
    // the port it listens on, 0 while it does not (yet)
    static int  port();
    // MiniMe's Claude chat panel: keeps the bridge on while the chat runs, whatever the preference says
    static void set_needed_by_chat(bool needed);
    // what to tell the user: the address, or why it does not run
    static std::string status();
};

} // namespace Slic3r::GUI

#endif // slic3r_MiniMeBridge_hpp_
