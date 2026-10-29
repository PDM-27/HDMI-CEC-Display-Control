// PipeProtocol.h - the contract between the service and the tray app.
//
// A deliberately small, line-based text protocol: one request line in, one
// response line out, UTF-8, newline terminated. Text rather than a packed
// struct because it is trivially inspectable, survives version drift between a
// service and a tray app that were built at different times, and carries no
// pointers or lengths for a malformed message to abuse.
//
// SECURITY. The service runs as SYSTEM and the tray app runs as a normal user,
// so this pipe is a privilege boundary. Two rules follow, and both are load
// bearing:
//
//   1. The tray app never asks the service to send arbitrary CEC frames, and
//      never writes the settings itself. It asks for *named* operations, and
//      the service decides what those mean. A "send these bytes" command would
//      hand every local user a way to drive the display hardware directly.
//
//   2. The pipe DACL must not grant FILE_CREATE_PIPE_INSTANCE to ordinary
//      users. See kPipeSddl below - that bit is the whole ballgame.
#pragma once

namespace cec {

// Local only. The server additionally sets PIPE_REJECT_REMOTE_CLIENTS.
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\CecService";

// A second pipe that answers questions and never does anything.
//
// The pipe above is served by one thread, one request at a time, and a command
// such as switching input runs *on that thread* for up to three seconds while
// it waits on the television. Everything else waits behind it, including the
// menu asking which input is on screen - so the menu drew a stale answer during
// exactly the seconds after a switch, when somebody has just watched the
// picture change and opened it to look. The answer was ready the whole time,
// sitting in a snapshot that needs no CEC lock; nobody was free to hand it over.
//
// So questions get a thread of their own. This one serves STATUS and DEVICES
// and nothing else: both are a copy out of that snapshot under a mutex held for
// the length of a copy, neither touches the hardware, the settings, or the CEC
// lock, and neither changes anything at all. That is what makes a second
// concurrent server safe to add - it cannot race the command thread because it
// shares no mutable state with it.
//
// Rule 1 in the header above still governs: this carries named questions, not
// arbitrary ones, and the handler is a two-verb whitelist rather than a filter.
inline constexpr wchar_t kReadPipeName[] = L"\\\\.\\pipe\\CecService-ask";

// D:P                    protected - no inherited ACEs
// (A;;FA;;;SY)           LocalSystem: full control (the service itself)
// (A;;FA;;;BA)           Administrators: full control
// (A;;0x12019b;;;AU)     Authenticated Users: connect, read, write
//
// 0x12019b is FILE_GENERIC_READ | FILE_GENERIC_WRITE with 0x4 masked off.
// For a *file* 0x4 is FILE_APPEND_DATA, but for a *pipe* the same bit is
// FILE_CREATE_PIPE_INSTANCE - so granting plain FILE_GENERIC_WRITE would let
// any logged-in user create their own instance of this pipe name, sit in front
// of the real service and impersonate it to other clients. Masking that one bit
// is what stops the classic named-pipe squatting attack.
inline constexpr wchar_t kPipeSddl[] =
    L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x12019b;;;AU)";

// How the service tells the tray app that something has arrived, rather than
// being asked over and over whether anything has.
//
// A remote key has to travel from Session 0 - where the service can see the CEC
// frame but has no desktop to inject into - to the tray app, which has a
// desktop but no CEC. The obvious shape is a blocking read on the pipe, and it
// is wrong: the pipe server answers one client at a time, so a tray parked on a
// long poll would wedge every volume press behind it.
//
// So the pipe stays strictly request/response and a named event does the
// waking. The tray blocks on this, costs nothing while it waits, and asks for
// the keys through the ordinary pipe once signalled. Auto-reset, because each
// signal means "there is something to collect" and collecting is idempotent.
//
// Global\ so that a service in Session 0 and an app in the user's session name
// the same object.
inline constexpr wchar_t kRemoteKeyEventName[] =
    L"Global\\CecServiceRemoteKey";

// Authenticated users may wait on it and nothing more. The service is the only
// thing that ever signals it, which matters: a user who could set this event
// could only cause an empty queue to be read, but there is no reason to grant
// even that.
//
// 0x00100000 is SYNCHRONIZE - the right to wait, without EVENT_MODIFY_STATE.
inline constexpr wchar_t kRemoteKeyEventSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100000;;;AU)";

// Signalled when a volume answer is ready to collect with VOLUME. The same
// rights as the remote key event, for the same reasons.
inline constexpr wchar_t kVolumeEventName[] = L"Global\\CecServiceVolume";

// Generous for the protocol, small enough that a malformed client cannot make
// the service allocate anything meaningful.
inline constexpr unsigned kPipeMaxMessage = 1024;

// ---------------------------------------------------------------- requests
//
//   STATUS                    -> OK attached=1 la=4 pa=1.0.0.0 name=HTPC
//   GET <setting>             -> OK <0|1>
//   SET <setting> <0|1>       -> OK
//   CMD <command> [argument]  -> OK
//   DEVICES                   -> OK <tab>2.0.0.0|awake|onscreen||Chromecast
//   KEYS                      -> OK 0DA6 0126R 2000
//   VOLUME ask                -> OK   (question sent; the answer follows)
//   VOLUME                    -> OK vol=35 mute=0 | OK refused | OK
//
// VOLUME ask puts <Give Audio Status> on the wire and returns straight away.
// The answer, or silence once the question has waited long enough, is kept by
// the service and kVolumeEventName is signalled; VOLUME then returns it without
// touching the bus. A report the bus volunteers is passed on the same way,
// unasked. "OK refused" means the set said it will not report (Feature Abort,
// or the 0x7F "decline to say"), which it will keep saying; bare "OK" is
// silence, which may just have been slow. The tray treats the two differently -
// see g_volumeSupport in CecTray.cpp.
//
// DEVICES lists what is plugged into the television, one entry per HDMI port,
// separated by tabs. The fields are the port, awake or asleep, onscreen when
// the TV is showing it, thispc when it is this machine, and last the name -
// last because a name may contain spaces. A device that never says its name is
// named by what it is, "Playback device". Asleep means it stopped answering a
// poll: it is still offered, because switching to a sleeping device is how it
// gets woken, and greying it would say the opposite.
//
// STATUS also carries one flag per setting (log= shutdown= sleep= blank=
// keys= pcvol=), s0=1 on a Modern Standby PC - where sleeping arrives
// as the screen turning off and nothing else, so the blank setting is what
// covers sleep and the menu says so - then tv=<identity> once a television has been
// recognised from its EDID, and tvvol=1 when that set has answered a volume
// question before. pcvol is the value in force for the set on the other end,
// which is that set's own - see TvStore in CecService.cpp.
//
// KEYS drains the queue of remote-control buttons the TV has forwarded, oldest
// first, and returns bare OK when there are none. Each is four hex digits: the
// CEC UI command, then the Windows virtual key it sends by default, 00 for a
// button with none. A trailing R marks the TV repeating a held button. The tray
// decides what is actually sent, since a person may choose a different key for
// any button. Draining rather than peeking, because a key delivered twice is
// worse than one delivered late.
//
// Anything unrecognised, malformed, or out of range gets ERR <reason>. The
// service validates every field; nothing from the pipe reaches the CEC layer
// without being matched against a known name first.
//
// Settings:  LoggingEnabled, StandbyOnShutdown, StandbyOnSleep,
//            StandbyOnDisplayOff, ForwardRemoteKeys, VolumeControlsPc.
//            VolumeControlsPc is read and written for the set on the other
//            end once a set is known.
//
// Commands:
//   prepare        warm-up on tray hover: refresh a stale registration, and
//                  re-check for a soundbar. Not counted as traffic.
//   press-volume-up / press-volume-down / press-mute, then release
//                  a held key, re-asserted by the tray every 300ms
//   audio-status   log what <Give Audio Status> returns (diagnostic)
//   tv-on          <Image View On> + <Active Source>
//   tv-standby     <Standby>, unless the TV reports it is already off: on the
//                  TCL <Standby> is a toggle, and would turn it back on
//   active-source  <Active Source> only - switch the TV to this PC
//   volume-up      <User Control Pressed/Released> 0x41   (TV support unverified)
//   volume-down    ... 0x42                               (TV support unverified)
//   mute           ... 0x43                               (TV support unverified)
//   input <1-4>    HDMI n, for the hotkey. Exactly switch-to below with the
//                  port named as a number instead of an address: the two asked
//                  for the same thing down two paths once, with two sets of
//                  retries and two ideas of what counted as success, and now
//                  there is one
//   set-volume <n> <Set Audio Volume Level> 0-100        (CEC 2.0; many ignore)
//   keep-awake     renew the keep-the-TV-awake lease; the tray sends it every
//                  two minutes while its toggle is on, and it lapses after five
//   keep-awake-off end the lease at once
//   switch-to <a>  put that HDMI port on screen, "2.0.0.0" style. The service
//                  picks the route: this PC claims the screen itself, and for
//                  anything else <Set Stream Path> goes out, which a TV passes
//                  to the bus for the device named to answer. Verified by
//                  asking who ended up on screen, tried twice, and for another
//                  PC running this app there is a direct ask as a last resort.
//                  ERR names why, which the tray shows
//   screen-on      the screen has come on, and a tray is here to say whether a
//                  person did it. Without one the service turns the TV back on
//                  at once, as it always did
//   user-active    the tray saw real keyboard or mouse input. This, not the
//                  screen coming on, is what turns the TV back on after the
//                  service switched it off: a TV changing state drops its HDMI
//                  signal, which wakes the screen with nobody there
//   prepare-blank  the tray expects the screen to go off in about ten seconds;
//                  announce now, if the display-off setting is on and nothing
//                  announced lately, so the <Standby> can leave at once

}  // namespace cec
