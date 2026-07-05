// symprof — symbolize AFFINEUI_SAMPLER profile dumps.
//
// The in-process sampling profiler (src/diag/sampler.cpp, enabled with
// AFFINEUI_SAMPLER=1) appends aggregated stacks to affineui_profile.txt
// as exe-relative RVAs. This tool resolves them against the exe's PDB
// via dbghelp and prints two views:
//
//   1. the top aggregated stacks, symbolized frame by frame
//   2. a flat ranking by LEAF (self) samples — "which function is hot"
//
// Usage:  symprof <app.exe> [affineui_profile.txt] [top_stacks]
//
// The profile's dumps are cumulative (the sampler never resets its
// counters), so only the section after the LAST "==== dump" header is
// read. Works on any machine with the exe+pdb pair — collect the
// profile on one box, symbolize on another.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr DWORD64 kLoadBase = 0x140000000ULL;

struct StackLine {
    std::uint64_t              count{0};
    std::vector<std::uint32_t> rvas;
};

// SizeOfImage from the PE optional header. dbghelp needs a non-zero
// module size to bound SymFromAddr lookups when loading from a path.
DWORD pe_image_size(const std::string& exe) {
    std::ifstream f(exe, std::ios::binary);
    if (!f) return 0;
    IMAGE_DOS_HEADER dos{};
    f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!f || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    f.seekg(dos.e_lfanew, std::ios::beg);
    IMAGE_NT_HEADERS64 nt{};
    f.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    if (!f || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt.OptionalHeader.SizeOfImage;
}

std::string resolve(HANDLE proc, std::uint32_t rva, bool with_line) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    std::memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 512;
    DWORD64 disp = 0;
    if (!SymFromAddr(proc, kLoadBase + rva, &disp, sym)) {
        char raw[32];
        std::snprintf(raw, sizeof(raw), "0x%X", rva);
        return raw;
    }
    std::string out = sym->Name;
    if (with_line) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        if (SymGetLineFromAddr64(proc, kLoadBase + rva, &ldisp, &line) &&
            line.FileName != nullptr) {
            std::string file = line.FileName;
            // Trim to a repo-relative-ish tail for readability.
            for (const char* marker : {"affineui\\", "affineui/"}) {
                if (const auto pos = file.rfind(marker);
                    pos != std::string::npos) {
                    file = file.substr(pos + std::strlen(marker));
                }
            }
            out += " (" + file + ":" + std::to_string(line.LineNumber) + ")";
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: symprof <app.exe> [affineui_profile.txt] "
                     "[top_stacks]\n");
        return 2;
    }
    const std::string exe = argv[1];
    const std::string profile =
        argc > 2 ? argv[2] : "affineui_profile.txt";
    const int top_stacks = argc > 3 ? std::atoi(argv[3]) : 15;

    std::ifstream in(profile);
    if (!in) {
        std::fprintf(stderr, "symprof: cannot open %s\n", profile.c_str());
        return 2;
    }

    // Keep only the stacks after the LAST dump header (cumulative dumps).
    std::vector<StackLine> stacks;
    std::string header;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("====", 0) == 0) {
            if (line.find("dump:") != std::string::npos) {
                header = line;
                stacks.clear();
            }
            continue;
        }
        const auto x = line.find(" x ");
        if (x == std::string::npos) continue;
        StackLine s;
        s.count = std::strtoull(line.c_str(), nullptr, 10);
        std::istringstream frames(line.substr(x + 3));
        std::string tok;
        while (frames >> tok) {
            s.rvas.push_back(
                static_cast<std::uint32_t>(std::strtoul(tok.c_str(),
                                                        nullptr, 16)));
        }
        if (s.count > 0 && !s.rvas.empty()) stacks.push_back(std::move(s));
    }
    if (stacks.empty()) {
        std::fprintf(stderr, "symprof: no stacks found in %s\n",
                     profile.c_str());
        return 1;
    }

    // Point the search path at the exe's own directory so the paired PDB
    // is found even when the cwd is elsewhere. SYMOPT_LOAD_LINES for
    // file:line; NO SYMOPT_DEFERRED_LOADS — we force a full load below and
    // want the failure reported synchronously, not on first lookup.
    std::string search_dir = exe;
    if (const auto slash = search_dir.find_last_of("\\/");
        slash != std::string::npos) {
        search_dir.resize(slash);
    } else {
        search_dir = ".";
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEBUG);
    HANDLE proc = GetCurrentProcess();
    if (!SymInitialize(proc, search_dir.c_str(), FALSE)) {
        std::fprintf(stderr, "symprof: SymInitialize failed (%lu)\n",
                     GetLastError());
        return 1;
    }
    // Pass the real image size (from the PE header) rather than 0 — a zero
    // size with a null file handle leaves dbghelp unable to bound the
    // module, so SymFromAddr silently misses and every frame prints raw.
    const DWORD image_size = pe_image_size(exe);
    const DWORD64 loaded =
        SymLoadModuleEx(proc, nullptr, exe.c_str(), nullptr, kLoadBase,
                        image_size, nullptr, 0);
    if (loaded == 0 && GetLastError() != ERROR_SUCCESS) {
        std::fprintf(stderr, "symprof: SymLoadModuleEx failed (%lu) — is "
                             "the .pdb next to the exe?\n",
                     GetLastError());
        return 1;
    }
    // Verify symbols actually resolved (deferred/partial loads look like
    // success but return a stripped module — the raw-RVA failure mode).
    IMAGEHLP_MODULE64 modinfo{};
    modinfo.SizeOfStruct = sizeof(modinfo);
    if (SymGetModuleInfo64(proc, kLoadBase, &modinfo)) {
        if (modinfo.SymType != SymPdb && modinfo.SymType != SymDia) {
            std::fprintf(stderr,
                         "symprof: WARNING — module loaded but symbols are "
                         "%d (not PDB); output will be raw RVAs. Check that "
                         "%s\\%s.pdb exists and matches.\n",
                         static_cast<int>(modinfo.SymType),
                         search_dir.c_str(), "decius_dender");
        }
    }

    std::sort(stacks.begin(), stacks.end(),
              [](const StackLine& a, const StackLine& b) {
                  return a.count > b.count;
              });
    std::uint64_t total = 0;
    for (const auto& s : stacks) total += s.count;

    std::printf("%s\n", header.c_str());
    std::printf("%llu samples across %zu unique stacks\n\n",
                static_cast<unsigned long long>(total), stacks.size());

    // View 1: top stacks, leaf-first.
    const std::size_t show =
        std::min<std::size_t>(stacks.size(),
                              static_cast<std::size_t>(
                                  top_stacks > 0 ? top_stacks : 15));
    for (std::size_t i = 0; i < show; ++i) {
        const auto& s = stacks[i];
        std::printf("---- %llu samples (%.1f%%) ----\n",
                    static_cast<unsigned long long>(s.count),
                    total > 0 ? 100.0 * static_cast<double>(s.count) /
                                    static_cast<double>(total)
                              : 0.0);
        for (std::size_t j = 0; j < s.rvas.size(); ++j) {
            std::printf("  %s\n",
                        resolve(proc, s.rvas[j], true).c_str());
        }
        std::printf("\n");
    }

    // View 2: flat self-sample ranking (leaf frame of every stack).
    std::map<std::uint32_t, std::uint64_t> self;
    for (const auto& s : stacks) self[s.rvas.front()] += s.count;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
    flat.reserve(self.size());
    for (const auto& [rva, c] : self) flat.push_back({c, rva});
    std::sort(flat.begin(), flat.end(), std::greater<>());

    std::printf("==== self samples by function ====\n");
    std::map<std::string, std::uint64_t> by_name;
    for (const auto& [c, rva] : flat) {
        by_name[resolve(proc, rva, false)] += c;
    }
    std::vector<std::pair<std::uint64_t, std::string>> named;
    named.reserve(by_name.size());
    for (const auto& [n, c] : by_name) named.push_back({c, n});
    std::sort(named.begin(), named.end(), std::greater<>());
    for (std::size_t i = 0; i < named.size() && i < 25; ++i) {
        std::printf("%6llu  %4.1f%%  %s\n",
                    static_cast<unsigned long long>(named[i].first),
                    total > 0 ? 100.0 *
                                    static_cast<double>(named[i].first) /
                                    static_cast<double>(total)
                              : 0.0,
                    named[i].second.c_str());
    }
    return 0;
}

#else

#include <cstdio>

int main() {
    std::fprintf(stderr, "symprof is win32-only (dbghelp)\n");
    return 2;
}

#endif
