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

    // starts or stops it after the app preference
    static void apply_preference();
    static void start(int port = DEFAULT_PORT);
    static void stop();
    static bool running();
    // what to tell the user: the address, or why it does not run
    static std::string status();
};

} // namespace Slic3r::GUI

#endif // slic3r_MiniMeBridge_hpp_
