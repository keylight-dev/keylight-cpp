// demo/notes/main.cpp — Keylight Notes console demo (C++ SDK)
//
// Mirrors keylight-rust demo-app and keylight-js demo/notes.ts exactly:
//   - Free tier: up to 3 notes
//   - Pro tier:  unlimited notes + export feature (gated by "pro" entitlement)
//
// Build (opt-in — requires OpenSSL):
//   cmake -B build-demo -DKEYLIGHT_BUILD_DEMO=ON \
//         -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
//   cmake --build build-demo
//   ./build-demo/demo/notes/notes <subcommand>
//
// Subcommands:
//   notes add "text"        — add a note (free tier: max 3)
//   notes list              — list all notes
//   notes activate <KEY>    — activate a Pro license key
//   notes export <path>     — export notes to file (Pro only)
//   notes deactivate        — release the seat (cleanup)

#include "keylight/keylight.hpp"
#include "keylight/keyset.hpp"
#include "keylight/transport/httplib.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Demo tenant constants — match JS/Rust demos verbatim
// ---------------------------------------------------------------------------
static constexpr const char* BASE    = "https://api.keylight.dev";
static constexpr const char* TENANT  = "keylight-notes-demo";
static constexpr const char* PRODUCT = "notes";

// ---------------------------------------------------------------------------
// Notes persistence — stored alongside the lease in ~/.keylight/
// ---------------------------------------------------------------------------
static std::filesystem::path notes_path() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home
        ? std::filesystem::path(home) / ".keylight"
        : std::filesystem::temp_directory_path() / ".keylight";
    return base / "keylight-notes.txt";
}

static std::vector<std::string> load_notes() {
    std::ifstream f(notes_path());
    if (!f) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

static void save_notes(const std::vector<std::string>& notes) {
    namespace fs = std::filesystem;
    fs::create_directories(notes_path().parent_path());
    std::ofstream f(notes_path(), std::ios::trunc);
    for (const auto& n : notes) {
        f << n << "\n";
    }
}

// ---------------------------------------------------------------------------
// state_name — human-readable state label
// ---------------------------------------------------------------------------
static const char* state_name(keylight::State s) {
    switch (s) {
        case keylight::State::Licensed: return "Licensed";
        case keylight::State::Trial:    return "Trial";
        case keylight::State::Expired:  return "Expired";
        case keylight::State::Invalid:  return "Invalid";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Keylight Notes — C++ SDK demo\n\n"
                  << "Usage:\n"
                  << "  notes add \"<text>\"    Add a note (free tier: max 3)\n"
                  << "  notes list            List all notes\n"
                  << "  notes activate <KEY>  Activate a Pro license key\n"
                  << "  notes export <path>   Export notes to file (Pro only)\n"
                  << "  notes deactivate      Release the Pro seat\n";
        return 1;
    }

    std::string cmd = argv[1];

    // ── Build the SDK client ──────────────────────────────────────────────

    keylight::HttplibTransport transport;

    // Fetch trusted keys so offline lease verification works after activation.
    keylight::Config cfg;
    cfg.tenantId   = TENANT;
    cfg.productId  = PRODUCT;
    cfg.apiBaseUrl = BASE;
    cfg.keyPrefix  = "NOTES";
    // No sdkKey — demo tenant is public.

    auto ks = keylight::fetchKeyset(transport, BASE, TENANT);
    if (ks.is_ok()) {
        cfg.trustedKeys = ks.value();
    } else {
        // Non-fatal: offline use still works if a lease is already cached.
        std::cerr << "[warn] fetchKeyset: " << ks.error().message << "\n";
    }

    // Lease persisted to ~/.keylight/keylight-notes-demo-notes.lease
    std::string store_path = keylight::default_store_path(cfg);
    keylight::FileStore store(store_path);
    keylight::Client    client(cfg, transport, store);

    // ── Dispatch ──────────────────────────────────────────────────────────

    if (cmd == "activate") {
        if (argc < 3) {
            std::cerr << "Usage: notes activate <LICENSE_KEY>\n";
            return 1;
        }
        std::string key = argv[2];
        auto r = client.activate(key);
        if (!r.is_ok()) {
            std::cerr << "Activation error: " << r.error().message << "\n";
            return 1;
        }
        bool pro = client.hasEntitlement("pro");
        if (r.value() == keylight::State::Licensed) {
            std::cout << "Pro unlocked!\n";
            std::cout << "State:          " << state_name(r.value()) << "\n";
            std::cout << "Note limit:     unlimited\n";
            std::cout << "Export enabled: " << (pro ? "yes" : "no") << "\n";
        } else {
            std::cout << "Activation failed (state: " << state_name(r.value()) << ")\n";
            return 1;
        }

    } else if (cmd == "deactivate") {
        auto r = client.deactivate();
        if (!r.is_ok()) {
            std::cerr << "Deactivation error: " << r.error().message << "\n";
            return 1;
        }
        std::cout << "Seat released. State: Invalid\n";

    } else if (cmd == "list") {
        auto notes = load_notes();
        bool pro   = client.hasEntitlement("pro");
        std::cout << "State:      " << state_name(client.state()) << "\n";
        std::cout << "Tier:       " << (pro ? "Pro" : "Free") << "\n";
        std::cout << "Note limit: " << (pro ? "unlimited" : "3") << "\n";
        if (notes.empty()) {
            std::cout << "(no notes)\n";
        } else {
            for (size_t i = 0; i < notes.size(); ++i) {
                std::cout << (i + 1) << ". " << notes[i] << "\n";
            }
        }

    } else if (cmd == "add") {
        if (argc < 3) {
            std::cerr << "Usage: notes add \"<text>\"\n";
            return 1;
        }
        std::string text = argv[2];
        auto notes       = load_notes();
        bool pro         = client.hasEntitlement("pro");

        if (!pro && notes.size() >= 3) {
            std::cerr << "Free tier is limited to 3 notes. "
                         "Activate a Pro key to add more.\n";
            return 1;
        }

        notes.push_back(text);
        save_notes(notes);
        std::cout << "Added (" << notes.size() << " note"
                  << (notes.size() == 1 ? "" : "s") << ").\n";

    } else if (cmd == "export") {
        if (argc < 3) {
            std::cerr << "Usage: notes export <path>\n";
            return 1;
        }
        if (!client.hasEntitlement("pro")) {
            std::cerr << "Export is a Pro feature. Activate a Pro key.\n";
            return 1;
        }
        std::string path = argv[2];
        auto notes       = load_notes();
        std::ofstream f(path, std::ios::trunc);
        if (!f) {
            std::cerr << "Cannot open output file: " << path << "\n";
            return 1;
        }
        for (const auto& n : notes) {
            f << n << "\n";
        }
        std::cout << "Exported " << notes.size() << " note"
                  << (notes.size() == 1 ? "" : "s") << " to " << path << "\n";

    } else {
        std::cerr << "Unknown command: " << cmd << "\n"
                  << "Run without arguments to see usage.\n";
        return 1;
    }

    return 0;
}
