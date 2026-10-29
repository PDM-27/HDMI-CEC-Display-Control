// Every path through MoveIfAddressTaken, against a fake device that can be made
// to fail in each of the ways the real one can. The decision logic is copied
// verbatim; only the device and Log are stubs.
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

typedef unsigned long DWORD;
typedef long LONG;

static DWORD g_now = 100000;
DWORD GetTickCount() { return g_now; }

std::vector<std::string> g_log;
void Log(const char* fmt, ...) {
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    g_log.push_back(buf);
}

constexpr DWORD kAddressRetryGapMs = 3000;
constexpr int kAddressRetryLimit = 5;
constexpr uint8_t kLogicalAddressFreeUse = 14;

const char* FreeUseNote(uint8_t address) {
    return address == kLogicalAddressFreeUse
               ? ", the Free Use address, because the playback ones were taken"
               : "";
}

// ----------------------------------------------------------- the fake device
struct FakeDevice {
    bool clash = false;
    uint8_t logical = 4;
    bool probed = false;
    std::string error;

    // Scripted outcome of the next AllocateLogicalAddress.
    enum Outcome { kKeep, kMove, kBusy, kFull, kRestoreFailed } outcome = kKeep;
    uint8_t moveTo = 8;
    int allocCalls = 0;

    bool addressClash() const { return clash; }
    void clear_addressClash() { clash = false; }
    void set_addressClash() { clash = true; }
    bool probedTheBus() const { return probed; }
    uint8_t logicalAddress() const { return logical; }
    const std::string& lastError() const { return error; }
    uint16_t physicalAddress() const { return 0x4000; }
    void Announce(uint16_t) {}

    bool AllocateLogicalAddress(uint8_t preferred) {
        ++allocCalls;
        lastPreferred = preferred;
        switch (outcome) {
            case kKeep:   probed = true;  error.clear(); return true;
            case kMove:   probed = true;  error.clear(); logical = moveTo; return true;
            case kBusy:   probed = false;
                          error = "could not probe the CEC bus for a free address";
                          return false;
            case kFull:   probed = true;
                          error = "every logical address this app can use is taken";
                          return false;
            // Picked one, then could not put the mask back: the address changed
            // and the call still failed.
            case kRestoreFailed:
                          probed = false;
                          error = "could not restore the logical address after probing";
                          logical = moveTo;
                          return false;
        }
        return false;
    }
    uint8_t lastPreferred = 0xFF;
};

FakeDevice dev;
FakeDevice* device_ = &dev;
DWORD addressRetryTick_ = 0;
int addressRetries_ = 0;
bool mapWanted_ = false;
DWORD lastAnnounceTick_ = 0, lastTrafficTick_ = 0;
int rememberCalls = 0;
void RememberAddress() { ++rememberCalls; }

// ----------------------------------------------------------- verbatim
void MoveIfAddressTaken() {
    if (!device_ || !device_->addressClash()) return;
    if (addressRetryTick_ &&
        static_cast<LONG>(addressRetryTick_ - GetTickCount()) > 0) {
        return;
    }
    device_->clear_addressClash();

    const uint8_t before = device_->logicalAddress();
    const bool allocated = device_->AllocateLogicalAddress(before);
    const uint8_t after = device_->logicalAddress();

    if (!allocated) {
        if (!addressRetries_) {
            Log("CEC address %u is shared with another device: %s; commands "
                "may be ignored", before, device_->lastError().c_str());
        }
        if (!device_->probedTheBus() &&
            ++addressRetries_ <= kAddressRetryLimit) {
            device_->set_addressClash();
            addressRetryTick_ = (GetTickCount() + kAddressRetryGapMs) | 1;
            return;
        }
        if (addressRetries_ > kAddressRetryLimit) {
            Log("gave up moving off CEC address %u after %d tries; waiting "
                "for it to be noticed again", before, kAddressRetryLimit);
        }
        addressRetries_ = 0;
        addressRetryTick_ = 0;
        return;
    }

    addressRetries_ = 0;
    addressRetryTick_ = 0;
    if (after == before) {
        Log("CEC address %u is free again; keeping it", before);
        return;
    }
    Log("CEC address %u was taken by another device; moved to %u%s", before,
        after, FreeUseNote(after));
    RememberAddress();
    mapWanted_ = true;
    device_->Announce(device_->physicalAddress());
    lastAnnounceTick_ = lastTrafficTick_ = GetTickCount();
}

// ----------------------------------------------------------- checks
int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}
bool LogHas(const char* text) {
    for (const std::string& l : g_log)
        if (l.find(text) != std::string::npos) return true;
    return false;
}
void Reset(FakeDevice::Outcome outcome, uint8_t logical = 4) {
    dev = FakeDevice();
    dev.outcome = outcome;
    dev.logical = logical;
    dev.clash = true;
    addressRetryTick_ = 0;
    addressRetries_ = 0;
    rememberCalls = 0;
    mapWanted_ = false;
    g_log.clear();
}

int main() {
    // 1. The address turned out to be free again.
    Reset(FakeDevice::kKeep);
    MoveIfAddressTaken();
    Expect(LogHas("is free again; keeping it"), "keeping a freed address is a success");
    Expect(!LogHas("shared with another device"), "and is not called a failure");
    Expect(rememberCalls == 0 && !mapWanted_, "nothing is re-announced for it");
    Expect(dev.lastPreferred == 4, "our own address was offered first");

    // 2. A real move.
    Reset(FakeDevice::kMove);
    MoveIfAddressTaken();
    Expect(LogHas("moved to 8"), "a move is reported");
    Expect(rememberCalls == 1 && mapWanted_, "and is remembered and re-announced");

    // 2b. A move onto Free Use says so.
    Reset(FakeDevice::kMove);
    dev.moveTo = 14;
    MoveIfAddressTaken();
    Expect(LogHas("moved to 14, the Free Use address"), "Free Use is called out");

    // 3. The dangerous one: picked an address, could not restore the mask. The
    //    address changed, so a check on the address alone would call it a move.
    Reset(FakeDevice::kRestoreFailed);
    MoveIfAddressTaken();
    Expect(!LogHas("was taken by another device; moved to"),
           "a failed restore is never reported as a successful move");
    Expect(LogHas("could not restore"), "the real reason is reported");
    Expect(rememberCalls == 0, "and a broken address is not remembered");
    Expect(dev.clash, "it is queued to try again, since that must be fixed");

    // 4. Busy bus: retries on a leash, then stops.
    Reset(FakeDevice::kBusy);
    MoveIfAddressTaken();
    Expect(dev.clash && addressRetryTick_, "a busy bus schedules a retry");
    Expect(dev.allocCalls == 1, "one attempt so far");
    MoveIfAddressTaken();
    Expect(dev.allocCalls == 1, "and the gate blocks an immediate second one");
    int guard = 0;
    while (dev.clash && guard++ < 50) {
        g_now += kAddressRetryGapMs + 1;
        MoveIfAddressTaken();
    }
    Expect(dev.allocCalls == kAddressRetryLimit + 1,
           "it stops after the limit, not before and not forever");
    Expect(LogHas("gave up moving off"), "and says so once");
    int shared = 0;
    for (const std::string& l : g_log)
        if (l.find("is shared with another device") != std::string::npos) ++shared;
    Expect(shared == 1, "the failure itself is logged once, not once per try");
    Expect(addressRetries_ == 0 && addressRetryTick_ == 0,
           "and the leash is reset for the next clash");

    // 5. A genuinely full bus is not retried at all.
    Reset(FakeDevice::kFull);
    MoveIfAddressTaken();
    Expect(!dev.clash && !addressRetryTick_, "a full bus is not retried");
    Expect(LogHas("every logical address this app can use is taken"),
           "and says what it found");

    // 6. A fresh clash after a quiet period still works.
    Reset(FakeDevice::kBusy);
    MoveIfAddressTaken();
    // +1 because the deadline is OR'd with 1 so that 0 can mean "no gate".
    g_now += kAddressRetryGapMs + 1;
    dev.outcome = FakeDevice::kMove;
    MoveIfAddressTaken();
    Expect(LogHas("moved to 8"), "a retry that succeeds still moves");
    Expect(addressRetries_ == 0, "and clears the leash");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
