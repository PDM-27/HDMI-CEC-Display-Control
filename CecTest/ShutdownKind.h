// ShutdownKind.h - telling a restart apart from a power off.
//
// SERVICE_CONTROL_SHUTDOWN is byte-identical for both and carries no reason
// code, but the distinction matters: a power off should take the TV with it,
// while a restart should not (people reboot to troubleshoot, and a TV that is
// dark through POST makes reaching the BIOS awkward).
//
// User32 writes System event log ID 1074 when shutdown is *initiated*, with
// param5 holding the literal string "restart" or "power off". Measured on the
// development machine, that lands ~6-7 seconds before services are told to
// stop, so the record is already present when the service needs it - provided
// the Event Log service is still answering by then.
//
// That string is the local Windows' own wording. "Other (Unplanned)" sits
// beside it in the same event and is plainly translated, so there is no reason
// to think "restart" is not. Kernel-Power event 109 carries the same fact as a
// number and cannot be translated at all, but it is written about six seconds
// *after* this decision has to be made, and a second after the Event Log
// service has stopped, so it cannot even be asked for by then. Measured across
// two shutdowns on 2026-09-29: 1074 at 23:51:01, the service deciding at
// 23:51:04, the event log gone at 23:51:08, 109 at 23:51:09.
//
// So 109 is read on the way back up instead, where both records sit together,
// and the pair says what the word meant. See LearnShutdownWord.
#pragma once

#include <string>
#include <vector>

namespace cec {

enum class ShutdownKind { Unknown, PowerOff, Restart };

const char* ShutdownKindName(ShutdownKind kind);

// What this machine's Windows puts in event 1074, when that is not the English
// wording. Empty until a shutdown has taught it, and English is matched anyway,
// so an English machine never needs either of these filled in.
//
// A list rather than a word each, because one machine has more than one. The
// same German laptop writes "Ausschalten" for a power off started from the
// Start menu and "Herunterfahren" for one started by shutdown.exe. With a
// single slot the second would overwrite the first and half of that machine's
// shutdowns would go back to being unrecognised.
struct ShutdownWords {
    std::vector<std::wstring> restart;
    std::vector<std::wstring> powerOff;
};

// What event 1074 said, as well as what it meant.
//
// The word is the type field exactly as this Windows wrote it, and `when` is
// the event's own timestamp. Both are empty when no event was found. A caller
// that does not recognise the word keeps the pair and asks
// ConfirmShutdownWord about it after the next boot.
struct ShutdownRecord {
    ShutdownKind kind = ShutdownKind::Unknown;
    // Windows Update restarting the machine by itself, rather than a person
    // doing it - see IsWindowsUpdateShutdown. False when the event cannot say.
    bool byWindowsUpdate = false;
    std::wstring word;
    std::wstring when;
};

// `notBefore` is a FILETIME, as a count of 100ns intervals, that the event has
// to be newer than. Pass the moment this process started: an event older than
// that belongs to a previous shutdown, and believing one is not a hypothetical.
//
// Event 1074 is written a fraction of a second before the service is told to
// stop, and it is not answerable by EvtQuery for a second or two after that. So
// a query at the wrong moment either finds nothing or finds the **previous**
// one, which is still inside any sensible window. A laptop log has both: at
// 01:18:03 a shutdown read the restart from 01:16:49 and reported "restart",
// leaving the television on when it should have switched it off, and at
// 01:21:43 the same query found nothing at all. The first of those needs no
// foreign language to happen - any PC shut down within two minutes of a restart
// would do it.
//
// So this waits, briefly, for an event that is actually this shutdown's.
ShutdownRecord QueryShutdown(unsigned int withinMs, const ShutdownWords& local,
                             unsigned long long notBefore,
                             std::string* detail = nullptr);

// What Kernel-Power 109 says the shutdown that happened after `when` was.
//
// Pairing by time alone does not work, and the log that proved it is worth
// keeping: three "Shut down" clicks on a fast-startup laptop each wrote a 1074
// and then hibernated instead of shutting down, so no 109 ever followed them,
// while two restarts wrote no 1074 at all. Taking the newest of each and
// allowing five minutes between them married an "Ausschalten" to a restart's
// action type from a different cycle two and a half minutes later, and that
// wrong lesson would have been believed for good.
//
// So the caller does not go looking for a word. It passes the one this service
// read while it was itself shutting down, which is the one shutdown it can be
// certain about, and this only has to say what became of it. The 109 must be
// newer than that word, and must be a restart or a power off, since a sleep
// writes one of these too.
bool ConfirmShutdownWord(const std::wstring& when, unsigned int withinMs,
                         ShutdownKind* kind, std::string* detail);

// Looks back over the last `withinMs` milliseconds for the most recent
// shutdown-initiated event. Returns Unknown on any failure; `detail`, if given,
// receives a human-readable reason, which is the only way to tell "no such
// event" apart from "the Event Log service was already gone".
// The kind alone, for a caller with no interest in teaching anything. A
// `notBefore` of zero accepts whatever the log holds, which suits a bench tool
// asking after the fact and suits nothing that has to act.
ShutdownKind QueryShutdownKind(unsigned int withinMs,
                               const ShutdownWords& local,
                               unsigned long long notBefore,
                               std::string* detail = nullptr);

}  // namespace cec
