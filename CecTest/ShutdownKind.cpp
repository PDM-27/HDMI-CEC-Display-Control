#include "ShutdownKind.h"

#include <windows.h>
#include <winevt.h>

#include <cstdio>
#include <algorithm>
#include <cwchar>
#include <vector>

namespace cec {
namespace {

std::string Detail(const char* what) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s (error %lu)", what, GetLastError());
    return buf;
}

// The most recent event matching a query, rendered as XML, or empty.
//
// Note the raw "<=" the callers write: despite appearing in an XML-ish query,
// the Event Log XPath parser rejects the escaped "&lt;=" form.
std::wstring NewestEventXml(const wchar_t* query, std::string* detail) {
    EVT_HANDLE results = EvtQuery(nullptr, L"System", query,
                                  EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) {
        if (detail) *detail = Detail("EvtQuery failed");
        return {};
    }

    EVT_HANDLE event = nullptr;
    DWORD returned = 0;
    if (!EvtNext(results, 1, &event, 2000, 0, &returned) || !returned) {
        if (detail) *detail = Detail("no matching event");
        EvtClose(results);
        return {};
    }

    std::wstring xml(8192, L'\0');
    DWORD used = 0;
    DWORD properties = 0;
    const BOOL rendered =
        EvtRender(nullptr, event, EvtRenderEventXml,
                  static_cast<DWORD>(xml.size() * sizeof(wchar_t)), xml.data(),
                  &used, &properties);
    EvtClose(event);
    EvtClose(results);
    if (!rendered) {
        if (detail) *detail = Detail("EvtRender failed");
        return {};
    }
    // Trimmed to the text, so that searching it cannot wander into the tail of
    // the buffer that nothing wrote.
    xml.resize(std::wcslen(xml.c_str()));
    return xml;
}

// Every <Data> value, in the order they appear.
//
// EvtRender emits bare <Data> elements for a legacy provider like User32 - the
// Name="paramN" attributes visible in PowerShell are added by its own
// formatting, not present here - so for that event position is the only thing
// that names a field.
std::vector<std::wstring> DataValues(const std::wstring& xml) {
    std::vector<std::wstring> values;
    size_t at = xml.find(L"<Data");
    while (at != std::wstring::npos) {
        const size_t open = xml.find(L'>', at);
        if (open == std::wstring::npos) break;
        const size_t close = xml.find(L'<', open + 1);
        if (close == std::wstring::npos) break;
        values.push_back(xml.substr(open + 1, close - open - 1));
        at = xml.find(L"<Data", close);
    }
    return values;
}

// The value of a <Data> the provider's manifest has named, which a modern
// provider like Kernel-Power has and User32 has not.
std::wstring NamedData(const std::wstring& xml, const wchar_t* name) {
    std::wstring key = L"Name='";
    key += name;
    key += L"'>";
    const size_t at = xml.find(key);
    if (at == std::wstring::npos) return {};
    const size_t start = at + key.size();
    const size_t end = xml.find(L'<', start);
    if (end == std::wstring::npos) return {};
    return xml.substr(start, end - start);
}

// The text of an attribute, such as the SystemTime the event carries.
std::wstring Attribute(const std::wstring& xml, const wchar_t* name) {
    const size_t at = xml.find(name);
    if (at == std::wstring::npos) return {};
    const size_t start = at + std::wcslen(name);
    const size_t end = xml.find(L'\'', start);
    if (end == std::wstring::npos) return {};
    return xml.substr(start, end - start);
}

// Whether a shutdown was Windows Update acting on its own: begun by the
// SYSTEM account, for a reason whose major class is the operating system.
//
// Both halves are needed, and both are language-neutral, which the readable
// fields of this event are not. Measured on the desktop across 150 events: the
// servicing restarts are TrustedInstaller as SYSTEM with 0x80020003; a person
// clicking "Restart now" in the update prompt is MoNotificationUx under their
// own account with 0x80020010, which is a person and so still gets the TV;
// and the power button on the sign-in screen is winlogon as SYSTEM with
// 0x500ff, a person again, which the reason class keeps out. The class is the
// SHTDN_REASON_MAJOR_ bits, 0x00FF0000, and 0x00020000 is the operating system.
bool IsWindowsUpdateShutdown(const std::wstring& sid, const std::wstring& code) {
    if (sid != L"S-1-5-18" || code.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long reason = std::wcstoul(code.c_str(), &end, 16);
    if (!end || *end != L'\0') return false;
    return (reason & 0x00FF0000UL) == 0x00020000UL;
}

// An event timestamp as the count of 100ns intervals the rest of Windows
// measures time in, so that two of them can be compared.
bool ParseEventTime(const std::wstring& text, ULONGLONG& out) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (swscanf_s(text.c_str(), L"%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour,
                  &minute, &second) != 6) {
        return false;
    }
    SYSTEMTIME when = {};
    when.wYear = static_cast<WORD>(year);
    when.wMonth = static_cast<WORD>(month);
    when.wDay = static_cast<WORD>(day);
    when.wHour = static_cast<WORD>(hour);
    when.wMinute = static_cast<WORD>(minute);
    when.wSecond = static_cast<WORD>(second);
    FILETIME stamp = {};
    if (!SystemTimeToFileTime(&when, &stamp)) return false;
    out = (static_cast<ULONGLONG>(stamp.dwHighDateTime) << 32) |
          stamp.dwLowDateTime;
    return true;
}

// How long to keep asking for an event that has been written but is not yet
// answerable, and how long to wait between asks.
constexpr int kFreshTries = 16;
constexpr DWORD kFreshWaitMs = 250;

void FormatQuery(wchar_t* out, size_t size, const wchar_t* provider,
                 unsigned int eventId, unsigned int withinMs) {
    std::swprintf(out, size,
                  L"*[System[Provider[@Name='%s'] and (EventID=%u) and "
                  L"TimeCreated[timediff(@SystemTime) <= %u]]]",
                  provider, eventId, withinMs);
}

bool Holds(const std::vector<std::wstring>& words, const std::wstring& word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

// A field as UTF-8, for a log line.
//
// This kept printable ASCII and dropped the rest, which on a Russian machine
// turned the one field worth reading into a pair of spaces: the line said
// type=unknown and then showed nothing where the word should have been. The log
// is written as raw bytes, so UTF-8 passes through, and any reader that assumes
// it - which Notepad now does - shows the word as Windows wrote it.
std::string Printable(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()), nullptr,
                                          0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out;
    out.resize(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), bytes, nullptr, nullptr);
    return out;
}

}  // namespace

const char* ShutdownKindName(ShutdownKind kind) {
    switch (kind) {
        case ShutdownKind::PowerOff: return "power off";
        case ShutdownKind::Restart:  return "restart";
        default:                     return "unknown";
    }
}

ShutdownRecord QueryShutdown(unsigned int withinMs, const ShutdownWords& local,
                             unsigned long long notBefore, std::string* detail) {
    ShutdownRecord found;

    wchar_t query[256];
    FormatQuery(query, 256, L"User32", 1074, withinMs);

    // Waited for, rather than asked once. The event exists before the service is
    // told to stop but is not answerable for a moment after that, so a single
    // query lands in the gap and comes back either empty or holding the
    // previous shutdown's. Four seconds is far longer than the fraction of a
    // second measured, and it is spent only when the answer is not already
    // there; a shutdown has minutes of budget at this point and nothing else is
    // waiting on this.
    std::wstring xml;
    for (int attempt = 0; attempt < kFreshTries; ++attempt) {
        if (attempt) Sleep(kFreshWaitMs);
        xml = NewestEventXml(query, detail);
        if (xml.empty()) continue;
        if (!notBefore) break;
        ULONGLONG wrote = 0;
        if (ParseEventTime(Attribute(xml, L"SystemTime='"), wrote) &&
            wrote >= notBefore) {
            break;
        }
        // Somebody else's shutdown. Say so, in case this is all there is.
        if (detail) *detail = "the only event found is older than this service";
        xml.clear();
    }
    if (xml.empty()) return found;

    // Matched on the value rather than on a position that the rendering could
    // change: only the shutdown-type field is ever exactly one of these.
    //
    // English is tested first and always, so a machine that has never taught
    // this anything behaves as it always did. The learned words are whatever
    // this Windows said the last time one of these was proved by Kernel-Power
    // 109, and an empty one matches nothing rather than everything.
    std::string values;
    const std::vector<std::wstring> data = DataValues(xml);
    for (const std::wstring& value : data) {
        if (!values.empty()) values += " | ";
        values += Printable(value);
        if (value == L"restart" || Holds(local.restart, value)) {
            found.kind = ShutdownKind::Restart;
        } else if (value == L"power off" || Holds(local.powerOff, value)) {
            found.kind = ShutdownKind::PowerOff;
        }
    }

    // The word itself comes out by position, because this provider has no
    // manifest and its fields have no names to ask for - see DataValues. A
    // count that is not seven means the event is not the shape this was written
    // against, and nothing is taken from it.
    if (data.size() == 7) {
        found.word = data[4];
        found.when = Attribute(xml, L"SystemTime='");
        found.byWindowsUpdate =
            IsWindowsUpdateShutdown(Attribute(xml, L"UserID='"), data[3]);
    }

    if (detail) {
        *detail = values.empty() ? "no <Data> elements in rendered event"
                                 : ("fields: " + values);
    }
    return found;
}

ShutdownKind QueryShutdownKind(unsigned int withinMs, const ShutdownWords& local,
                               unsigned long long notBefore,
                               std::string* detail) {
    return QueryShutdown(withinMs, local, notBefore, detail).kind;
}

bool ConfirmShutdownWord(const std::wstring& when, unsigned int withinMs,
                         ShutdownKind* kind, std::string* detail) {
    if (!kind || when.empty()) return false;
    ULONGLONG wordAt = 0;
    if (!ParseEventTime(when, wordAt)) {
        if (detail) *detail = "the word kept from the last shutdown has no time";
        return false;
    }

    wchar_t query[256];
    FormatQuery(query, 256, L"Microsoft-Windows-Kernel-Power", 109, withinMs);
    const std::wstring power = NewestEventXml(query, detail);
    if (power.empty()) return false;

    // 5 is a restart and 6 is a power off, measured both ways. Anything else is
    // a transition this is not trying to name, since a sleep writes one of
    // these events as well, and guessing at it would teach the wrong word.
    const std::wstring action = NamedData(power, L"ShutdownActionType");
    if (action != L"5" && action != L"6") {
        // Named, not just refused. Anything other than these two is a
        // transition whose meaning is not known here, and the value is the only
        // thing that will ever say which - a log that reports the refusal
        // without the number sends whoever reads it back to the event viewer.
        if (detail) {
            *detail = "the last transition was ShutdownActionType " +
                      (action.empty() ? std::string("(absent)") : Printable(action)) +
                      ", which is neither a restart nor a power off";
        }
        return false;
    }

    ULONGLONG powerAt = 0;
    if (!ParseEventTime(Attribute(power, L"SystemTime='"), powerAt)) {
        if (detail) *detail = "the power event carried no readable time";
        return false;
    }
    // It has to have happened after the word, or it belongs to some earlier
    // shutdown and says nothing about this one.
    if (powerAt <= wordAt) {
        if (detail) *detail = "the power event is older than the word";
        return false;
    }

    *kind = (action == L"5") ? ShutdownKind::Restart : ShutdownKind::PowerOff;
    if (detail) *detail = "confirmed by Kernel-Power 109";
    return true;
}

}  // namespace cec
