#ifndef slic3r_MiniMeWatchdog_hpp_
#define slic3r_MiniMeWatchdog_hpp_

namespace Slic3r::GUI {

// SliceOfMe: when MiniMe's window stops responding (the GUI thread is stuck for several seconds), writes where it is stuck -
// the GUI thread's call stack with function names and source lines - to <data dir>\log\hang_<time>.txt, so a freeze can be
// fixed from the user's machine without a debugger. Windows only; does nothing elsewhere.
class HangWatchdog
{
public:
    // call on the GUI thread once the main window exists
    static void start();
    static void stop();
};

} // namespace Slic3r::GUI

#endif // slic3r_MiniMeWatchdog_hpp_
