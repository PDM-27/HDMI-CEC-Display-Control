// The candidate selection inside AllocateLogicalAddress: the avoid mask and the
// two-poll confirmation. Copied verbatim; only SendFrame and Sleep are stubs.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

constexpr uint8_t kLogicalAddressPlayback1 = 4;
constexpr uint8_t kLogicalAddressPlayback2 = 8;
constexpr uint8_t kLogicalAddressPlayback3 = 11;
constexpr uint8_t kLogicalAddressFreeUse = 14;

struct TxResult { bool accepted = true; bool nacked = false; };

// How each address answers a poll, by call number, so a device that is quiet
// once and present afterwards can be modelled.
struct Answers {
    // nacked == free. Index 0 is the first poll, 1 the second.
    bool nacked[2] = {false, false};
    bool accepted = true;
};
Answers g_bus[16];
int g_polls[16] = {0};
std::vector<uint8_t> g_order;   // the addresses actually polled, in order

void SendFrameStub(uint8_t candidate, TxResult* tx) {
    g_order.push_back(candidate);
    const int n = g_polls[candidate]++;
    tx->accepted = g_bus[candidate].accepted;
    tx->nacked = g_bus[candidate].nacked[n < 2 ? n : 1];
}

uint8_t logicalAddress_ = 4;
uint16_t avoidAddresses_ = 0;

// ------------------------------------------------- verbatim selection
uint8_t Select(uint8_t preferred, size_t* askedOut) {
    uint8_t candidates[] = {kLogicalAddressPlayback1, kLogicalAddressPlayback2,
                            kLogicalAddressPlayback3, kLogicalAddressFreeUse};
    for (size_t i = 1; i < 4; ++i) {
        if (candidates[i] != preferred) continue;
        candidates[i] = candidates[0];
        candidates[0] = preferred;
        break;
    }

    int usable = 0;
    for (const uint8_t candidate : candidates) {
        // Counting only the addresses this device could move *to*. Its own does
        // not count: it is never masked, so counting it would always make the
        // mask look survivable and the fallback below would never fire.
        if (candidate != logicalAddress_ &&
            !(avoidAddresses_ & (1u << candidate))) {
            ++usable;
        }
    }
    const uint16_t avoid = usable ? avoidAddresses_ : 0;

    uint8_t chosen = 0;
    size_t asked = 0;
    for (const uint8_t candidate : candidates) {
        if (candidate != logicalAddress_ && (avoid & (1u << candidate))) {
            continue;
        }
        TxResult tx;
        SendFrameStub(candidate, &tx);
        if (!tx.accepted) continue;
        ++asked;
        if (!tx.nacked) continue;

        TxResult again;
        SendFrameStub(candidate, &again);
        if (again.accepted && !again.nacked) continue;

        chosen = candidate;
        break;
    }
    *askedOut = asked;
    return chosen;
}

// ------------------------------------------------- verbatim seeding
// CecDevice::set_logicalAddress, and the order AcquireDevice calls it in.
void set_logicalAddress(uint8_t address) {
    if (address == kLogicalAddressPlayback1 ||
        address == kLogicalAddressPlayback2 ||
        address == kLogicalAddressPlayback3 ||
        address == kLogicalAddressFreeUse) {
        logicalAddress_ = address;
    }
}
uint8_t Seed(unsigned long stored, uint8_t heldAddress_) {
    logicalAddress_ = kLogicalAddressPlayback1;   // a new device's default
    const uint8_t remembered =
        stored < 16 ? static_cast<uint8_t>(stored) : 0;
    set_logicalAddress(remembered);
    set_logicalAddress(heldAddress_);
    return logicalAddress_;
}

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}
void Reset(uint8_t mine, uint16_t avoid) {
    for (int i = 0; i < 16; ++i) {
        g_bus[i] = Answers();          // everything answers: taken
        g_polls[i] = 0;
    }
    g_order.clear();
    logicalAddress_ = mine;
    avoidAddresses_ = avoid;
}
void Free(uint8_t a) { g_bus[a].nacked[0] = g_bus[a].nacked[1] = true; }
// Quiet once, then present: a device part way through announcing itself.
void QuietOnce(uint8_t a) { g_bus[a].nacked[0] = true; g_bus[a].nacked[1] = false; }

int main() {
    size_t asked = 0;

    // 1. Our own address is free again, so it is kept, and asked about first.
    Reset(4, 0);
    Free(4);
    Expect(Select(4, &asked) == 4, "a free own address is kept");
    Expect(g_order.size() == 2 && g_order[0] == 4, "and it was asked first");

    // 2. The field case: our address is taken, the next is a known neighbour.
    Reset(4, 1u << 8);
    Free(8);   // it would poll free, because it is mid-announce
    Free(11);
    Expect(Select(4, &asked) == 11, "a neighbour's address is not taken");
    for (uint8_t a : g_order) Expect(a != 8, "and is never even polled");

    // 3. A device quiet on the first poll is not mistaken for an absent one.
    Reset(4, 0);
    QuietOnce(8);
    Free(11);
    Expect(Select(4, &asked) == 11, "one silence is not proof an address is free");
    Expect(g_polls[8] == 2, "the second poll is what caught it");

    // 4. Genuinely free: both polls silent.
    Reset(4, 0);
    Free(8);
    Expect(Select(4, &asked) == 8, "two silences are believed");

    // 5. Our own address is never avoided, even when the mask names it.
    Reset(8, 1u << 8);
    Free(8);
    Expect(Select(8, &asked) == 8, "our own address survives the mask");

    // 6. The mask is a preference, not a cage.
    Reset(4, (1u << 4) | (1u << 8) | (1u << 11) | (1u << 14));
    Free(11);
    Expect(Select(4, &asked) == 11,
           "a mask covering everything is ignored rather than deadlocking");

    // 7. Everything taken: no choice, and the answer is conclusive.
    Reset(4, 0);
    Expect(Select(4, &asked) == 0 && asked == 4,
           "a full bus is reported as a full bus");

    // 8. Nothing could be put on the wire: inconclusive, so the caller retries.
    Reset(4, 0);
    for (int i = 0; i < 16; ++i) g_bus[i].accepted = false;
    Expect(Select(4, &asked) == 0 && asked == 0,
           "a bus that cannot be probed is not reported as full");

    // 9. Which address a rebuilt device starts on. A wake never allocates, so
    //    this is the address a woken PC claims and keeps.
    Expect(Seed(4, 8) == 8, "a wake keeps the address it slept on");
    Expect(Seed(8, 0) == 8, "with no device yet, the remembered one is used");
    Expect(Seed(0, 0) == 4, "with neither, the default");
    Expect(Seed(4, 14) == 14, "Free Use is kept through a wake");
    Expect(Seed(8, 9) == 8, "a held value that is not ours is ignored");
    Expect(Seed(0x108, 0) == 4, "a stray registry value cannot wrap to 8");
    Expect(Seed(5, 0) == 4, "the audio system's address is never taken up");

    // 10. The field case behind the seed: a rebuilt device whose own address is
    //     8 and taken, with a sleeping neighbour on 4 that answers nothing.
    Reset(Seed(8, 8), 1u << 4);
    Free(4);
    Free(11);
    Expect(Select(8, &asked) == 11, "a seeded device skips the sleeper's 4");
    for (uint8_t a : g_order) Expect(a != 4, "and never polls it");

    // 11. The same without the seed, which is how it was: the own-address
    //     exemption fell on 4 whoever held it, so the sleeper's 4 was taken.
    Reset(4, 1u << 4);
    Free(4);
    Free(11);
    Expect(Select(8, &asked) == 4,
           "unseeded, the exemption lands on the sleeper (the old fault)");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
