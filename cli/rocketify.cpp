// rocketify — the command line tool for mod developers (Windows and Linux).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "rocketify.h"
#include "embedded.inc"  // kFiles: the files mods are built with (made by build.sh)

namespace fs = std::filesystem;
using Args = std::vector<std::string>;

static const char* const kVersion = "0.0.2-alpha";
static const char* const kCliUrl = "https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/dist/cli/";
#ifdef _WIN32
static const bool kWindows = true;
#else
static const bool kWindows = false;
#endif

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- system

static std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

static fs::path Home() {
    const std::string h = Env(kWindows ? "USERPROFILE" : "HOME");
    if (h.empty()) throw Error("no home folder found");
    return h;
}

static fs::path DataDir() { return Home() / ".rocketify"; }

// Full path of a program in PATH, "" if it is not there.
static std::string Which(const std::string& program) {
    const std::string path = Env("PATH");
    const char sep = kWindows ? ';' : ':';
    for (size_t i = 0; i <= path.size();) {
        size_t end = path.find(sep, i);
        if (end == std::string::npos) end = path.size();
        if (end > i) {
            const fs::path p = fs::path(path.substr(i, end - i)) / (program + (kWindows ? ".exe" : ""));
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p.string();
        }
        i = end + 1;
    }
    return "";
}

#ifdef _WIN32
static std::string Quote(const std::string& a) {
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') { ++slashes; continue; }
        out.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        out += c;
    }
    return out.append(slashes * 2, '\\') + '"';
}
#endif

// Runs a program and waits for it; returns its exit code.
static int Run(const Args& args) {
    std::cout.flush();
#ifdef _WIN32
    std::string cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? "" : " ") + Quote(a);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        throw Error("could not start " + args[0]);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    const pid_t pid = fork();
    if (pid < 0) throw Error("could not start " + args[0]);
    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

// ---------------------------------------------------------------- config (~/.rocketify/config)

static std::map<std::string, std::string> LoadConfig() {
    std::map<std::string, std::string> c;
    std::ifstream in(DataDir() / "config");
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) c[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return c;
}

static std::string Config(const std::string& key) { return LoadConfig()[key]; }

static const std::map<std::string, std::string> kKeys = {
    {"mods-dir", "the game's rocketify/mods folder"},
    {"box", "distrobox to build in if this system has no compiler (Linux, default: rocketify)"},
};

static int SetConfig(const std::string& key, std::string value);

// ---------------------------------------------------------------- built-in files

static std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

static void WriteFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
    if (!out.flush()) throw Error("could not write " + p.string());
}

// The headers and build files, unpacked from this program. Unchanged files are left alone, so
// that mods are not rebuilt for nothing.
static fs::path BuildFiles() {
    const fs::path dir = DataDir() / "files";
    for (const auto& f : kFiles) {
        const std::string path = f.path;
        if (path.rfind("template/", 0) == 0) continue;
        if (!fs::exists(dir / path) || ReadFile(dir / path) != f.text) WriteFile(dir / path, f.text);
    }
    return dir;
}

// ---------------------------------------------------------------- game

static std::vector<fs::path> FindModsDirs() {
    std::vector<fs::path> roots, found;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        roots.push_back(fs::path(buf) / "steamapps" / "common" / "rocketleague");
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)"})
        if (!Env(var).empty()) {
            roots.push_back(fs::path(Env(var)) / "Epic Games" / "rocketleague");
            roots.push_back(fs::path(Env(var)) / "Steam" / "steamapps" / "common" / "rocketleague");
        }
#else
    for (const char* p : {"Games/Heroic/rocketleague", ".local/share/Steam/steamapps/common/rocketleague",
                          ".steam/steam/steamapps/common/rocketleague",
                          ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/rocketleague"})
        roots.push_back(Home() / p);
#endif
    for (const auto& r : roots) {
        std::error_code ec;
        const fs::path mods = r / "Binaries" / "Win64" / "rocketify" / "mods";
        if (!fs::is_directory(mods, ec)) continue;
        bool known = false;
        for (const auto& f : found) known = known || fs::equivalent(f, mods, ec);
        if (!known) found.push_back(mods);
    }
    return found;
}

static const std::string g_envModsDir = Env("ROCKETIFY_MODS_DIR");  // builds clear the variable

static fs::path NeedModsDir() {
    for (const std::string& s : {g_envModsDir, Config("mods-dir")}) {
        if (s.empty()) continue;
        if (!fs::is_directory(s)) throw Error("the mods folder does not exist: " + s);
        return s;
    }
    const auto dirs = FindModsDirs();
    if (dirs.empty())
        throw Error("no Rocket League with Rocketify found. Install Rocketify, or run: rocketify config mods-dir PATH");
    if (dirs.size() > 1) {
        std::string list;
        for (const auto& d : dirs) list += "\n  " + d.string();
        throw Error("several games found, choose one with: rocketify config mods-dir PATH" + list);
    }
    return dirs[0];
}

// The number after a marker text in a file, 0 if it is not there.
static int NumberAfter(const fs::path& file, const std::string& marker) {
    std::ifstream in(file, std::ios::binary);
    const std::string s{std::istreambuf_iterator<char>(in), {}};
    for (size_t i = 0; (i = s.find(marker, i)) != std::string::npos; ++i) {
        const size_t d = i + marker.size();
        if (d < s.size() && std::isdigit(static_cast<unsigned char>(s[d]))) return std::atoi(s.c_str() + d);
    }
    return 0;
}

// "" if the game's loader can run mods made with this version, otherwise what is wrong.
static std::string LoaderProblem(const fs::path& modsDir) {
    const fs::path dll = modsDir.parent_path() / "rocketify.dll";
    std::error_code ec;
    if (!fs::is_regular_file(dll, ec)) return "there is no rocketify.dll next to the mods folder";
    const int need = ROCKETIFY_API_VERSION;
    const int have = NumberAfter(dll, "API version ");
    if (have >= need) return "";
    return "the game's Rocketify is too old (API " + (have ? std::to_string(have) : std::string("unknown")) + ", mods need " +
           std::to_string(need) + "). Install the newest Rocketify";
}

// ---------------------------------------------------------------- new

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t i = 0; (i = s.find(from, i)) != std::string::npos; i += to.size()) s.replace(i, from.size(), to);
}

static int CmdNew(const Args& a) {
    std::string name;
    if (!a.empty()) name = a[0];
    else {
        std::cout << "Mod name: " << std::flush;
        std::getline(std::cin, name);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    while (!name.empty() && name[0] == ' ') name.erase(0, 1);
    if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name[0] == '-')
        throw Error("this is not a valid mod name");
    // The folder is called like the mod; the file name of the mod only has simple characters.
    std::string id;
    for (unsigned char c : name) {
        if (c < 0x80 && std::isalnum(c)) id += static_cast<char>(std::tolower(c));
        else if (!id.empty() && id.back() != '_') id += '_';
    }
    while (!id.empty() && id.back() == '_') id.pop_back();
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0]))) id = id.empty() ? "mod" : "mod_" + id;
    // CMake keeps these names for itself.
    for (const char* reserved : {"all", "clean", "help", "install", "package", "test", "edit_cache", "rebuild_cache"})
        if (id == reserved) { id += "_mod"; break; }
    const fs::path dir = fs::current_path() / name;
    if (fs::exists(dir)) throw Error("there already is a folder " + dir.string());
    for (const auto& f : kFiles) {
        std::string path = f.path, text = f.text;
        if (path.rfind("template/", 0) != 0) continue;
        ReplaceAll(text, "project(my_mod", "project(" + id);
        WriteFile(dir / path.substr(9), text);
    }
    BuildFiles();  // so that editors find <rocketify.hpp> before the first build
    std::cout << "Created " << dir.string() << "\n";
    return 0;
}

// ---------------------------------------------------------------- build / install

// The mod project around the current folder.
static fs::path NeedProject() {
    for (fs::path p = fs::current_path();; p = p.parent_path()) {
        if (fs::is_regular_file(p / "CMakeLists.txt") &&
            ReadFile(p / "CMakeLists.txt").find("setup_rocketify_mod(") != std::string::npos &&
            !fs::exists(p / "include" / "rocketify.hpp"))
            return p;
        if (p == p.parent_path()) throw Error("no Rocketify mod here. Make one with: rocketify new NAME");
    }
}

static fs::path BuiltMod(const fs::path& project) {
    fs::path best;
    for (const char* sub : {"", "Release", "Debug"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(project / "build" / sub, ec))
            if (e.path().extension() == ".rlmod" &&
                (best.empty() || fs::last_write_time(e.path()) > fs::last_write_time(best)))
                best = e.path();
    }
    if (best.empty()) throw Error("nothing built yet. Run: rocketify build");
    return best;
}

static int CmdInstall() {
    const fs::path mod = BuiltMod(NeedProject()), to = NeedModsDir() / mod.filename();
    std::error_code ec;
    // Never write into a file a running game has loaded: put a new one in its place.
    const fs::path tmp = to.string() + ".new";
    fs::copy_file(mod, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(tmp, to, ec);
    std::error_code ignored;
    if (ec) fs::remove(tmp, ignored);
    if (ec) throw Error("could not write " + to.string() + " (" + ec.message() + "). Is the game running?");
    std::cout << "Installed: " << to.string() << "\n";
    const std::string problem = LoaderProblem(to.parent_path());
    if (!problem.empty()) std::cout << "Warning: " << problem << ".\n";
    std::cout << "Mods are loaded when the game starts: restart it if it is running.\n";
    return 0;
}

[[maybe_unused]] static std::string Box() {
    const std::string b = Config("box");
    return b.empty() ? "rocketify" : b;
}

[[maybe_unused]] static bool HasMingw() { return !Which("x86_64-w64-mingw32-g++").empty() && !Which("cmake").empty(); }

[[maybe_unused]] static bool HasVisualStudio() {
    const std::string pf = Env("ProgramFiles(x86)");
    return !pf.empty() && fs::exists(fs::path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe");
}

#ifndef _WIN32
// True if the distrobox to build in exists.
static bool HasBox() {
    if (Which("distrobox").empty()) return false;
    return Run({"sh", "-c", "distrobox list --no-color 2>/dev/null | awk -F'|' -v b=\"$1\" '{gsub(/ /,\"\",$2)} $2==b{f=1} END{exit !f}'", "sh", Box()}) == 0;
}
#endif

// Installs everything needed to build mods.
static int CmdDependencies() {
#ifdef _WIN32
    if (Which("winget").empty()) throw Error("winget is needed (\"App Installer\" in the Microsoft Store)");
    const struct { const char *program, *package; } tools[] = {
        {"cmake", "Kitware.CMake"}, {"ninja", "Ninja-build.Ninja"}, {"g++", "BrechtSanders.WinLibs.POSIX.UCRT"}};
    int failed = 0;
    for (const auto& t : tools) {
        if (!Which(t.program).empty()) {
            std::cout << t.program << " is already installed\n";
            continue;
        }
        std::cout << "Installing " << t.package << "\n";
        if (Run({"winget", "install", "--id", t.package, "-e", "--source", "winget"}) != 0) ++failed;
    }
    if (failed) throw Error("some tools could not be installed");
    std::cout << "Done. Open a new terminal to build mods.\n";
    return 0;
#else
    // Also run inside a distrobox, so this is a plain shell script.
    static const char* const script =
        "have() { command -v \"$1\" >/dev/null 2>&1; }\n"
        "if have cmake && have ninja && have x86_64-w64-mingw32-g++; then echo 'Everything is already installed.'; exit 0; fi\n"
        "S=; [ \"$(id -u)\" = 0 ] || S=sudo\n"
        "if have apt-get; then $S apt-get update && $S apt-get install -y cmake ninja-build g++-mingw-w64-x86-64\n"
        "elif have dnf; then $S dnf install -y cmake ninja-build mingw64-gcc-c++ mingw64-winpthreads-static\n"
        "elif have pacman; then $S pacman -S --needed --noconfirm cmake ninja mingw-w64-gcc\n"
        "elif have zypper; then $S zypper install -y cmake ninja mingw64-cross-gcc-c++ mingw64-winpthreads-devel\n"
        "else echo 'No known package manager. Install cmake, ninja and MinGW-w64 yourself.' >&2; exit 1; fi\n"
        "have cmake && have ninja && have x86_64-w64-mingw32-g++ || { echo 'Some tools are still missing.' >&2; exit 1; }\n"
        "echo 'Done.'\n";
    if (!fs::exists("/run/ostree-booted") || HasMingw()) return Run({"sh", "-c", script});
    // The system itself is read-only (Bazzite, Silverblue, ...): use a distrobox.
    if (Which("distrobox").empty()) throw Error("this system is read-only: install distrobox first");
    if (!HasBox()) {
        std::cout << "Creating the '" << Box() << "' distrobox\n";
        if (int rc = Run({"distrobox", "create", "--yes", "--name", Box(), "--image", "registry.fedoraproject.org/fedora-toolbox:latest"})) return rc;
    }
    return Run({"distrobox", "enter", Box(), "--", "sh", "-c", script});
#endif
}

static int CmdBuild(const Args& a) {
    bool install = false, clean = false;
    std::string type = "Release";
    for (const auto& x : a) {
        if (x == "--install" || x == "-i") install = true;  // fail if it cannot be installed
        else if (x == "--no-install") continue;
        else if (x == "--clean") clean = true;
        else if (x == "--debug") type = "Debug";
        else throw Error("unknown option " + x + " (see rocketify help)");
    }
    const fs::path project = fs::canonical(NeedProject()), files = BuildFiles();
    if (install) NeedModsDir();  // complain before building
    const fs::path out = project / "build";
    if (clean) fs::remove_all(out);

    Args prefix;
    Args configure = {"cmake", "-S", project.string(), "-B", out.string(), "-DCMAKE_BUILD_TYPE=" + type, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DROCKETIFY_DIR=" + files.generic_string()};
    const bool fresh = !fs::exists(out / "CMakeCache.txt");
    const char* const missing = "the build tools are missing. Run: rocketify install dependencies";
#ifdef _WIN32
    SetEnvironmentVariableA("ROCKETIFY_SDK", files.string().c_str());  // mods made before 1.3
    SetEnvironmentVariableA("ROCKETIFY_MODS_DIR", nullptr);            // installing is done here
    if (Which("cmake").empty()) throw Error(missing);
    if (fresh) {
        if (!Which("g++").empty()) {
            if (Which("ninja").empty()) configure.insert(configure.end(), {"-G", "MinGW Makefiles"});
            else configure.insert(configure.end(), {"-G", "Ninja"});
            configure.push_back("-DCMAKE_CXX_COMPILER=g++");
        } else if (HasVisualStudio()) {
            configure.insert(configure.end(), {"-A", "x64"});
        } else {
            throw Error(missing);
        }
    }
#else
    bool ninja = !Which("ninja").empty();
    if (!HasMingw()) {
        if (!HasBox()) throw Error(missing);
        std::cout << "Building inside the '" << Box() << "' distrobox\n";
        prefix = {"distrobox", "enter", Box(), "--", "env", "-u", "ROCKETIFY_MODS_DIR", "ROCKETIFY_SDK=" + files.string()};
        if (fresh) ninja = Run({"distrobox", "enter", Box(), "--", "sh", "-c", "command -v ninja >/dev/null"}) == 0;
    }
    setenv("ROCKETIFY_SDK", files.c_str(), 1);  // mods made before 1.3
    unsetenv("ROCKETIFY_MODS_DIR");             // installing is done here
    if (fresh) {
        configure.push_back("-DCMAKE_TOOLCHAIN_FILE=" + (files / "cmake" / "mingw.cmake").string());
        if (ninja) configure.insert(configure.end(), {"-G", "Ninja"});
    }
#endif
    Args build = {"cmake", "--build", out.string(), "--config", type};
    for (Args* step : {&configure, &build}) {
        step->insert(step->begin(), prefix.begin(), prefix.end());
        if (int rc = Run(*step)) {
            std::cerr << "Build failed.\n";
            return rc;
        }
    }
    std::cout << "Built: " << BuiltMod(project).string() << "\n";
    return 0;
}

// ---------------------------------------------------------------- config / doctor

static int CmdConfig(const Args& a) {
    auto c = LoadConfig();
    if (a.empty()) {
        for (const auto& k : kKeys) std::cout << k.first << " = " << (c[k.first].empty() ? "(not set)" : c[k.first]) << "    " << k.second << "\n";
        return 0;
    }
    if (!kKeys.count(a[0])) throw Error("unknown setting " + a[0] + " (see rocketify config)");
    if (a.size() == 1) {
        std::cout << c[a[0]] << "\n";
        return 0;
    }
    std::string value = a[1];
    if (a[0] != "box" && !value.empty()) {
        if (!fs::is_directory(value)) throw Error("no such folder: " + value);
    }
    return SetConfig(a[0], value);
}

static int SetConfig(const std::string& key, std::string value) {
    auto c = LoadConfig();
    if (key != "box" && !value.empty()) value = fs::canonical(value).string();
    c[key] = value;
    fs::create_directories(DataDir());
    std::ofstream out(DataDir() / "config");
    for (const auto& kv : c)
        if (!kv.second.empty()) out << kv.first << "=" << kv.second << "\n";
    if (!out.flush()) throw Error("could not write the settings");
    return 0;
}

static int CmdList() {
    const fs::path dir = NeedModsDir();
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".rlmod" || e.path().extension() == ".dll") names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    std::cout << dir.string() << "\n";
    for (const auto& n : names) std::cout << "  " << n << "\n";
    if (names.empty()) std::cout << "  (no mods)\n";
    return 0;
}

static int CmdRemove(const Args& a) {
    if (a.size() != 1) throw Error("use: rocketify remove NAME (see rocketify list)");
    const fs::path dir = NeedModsDir();
    if (fs::path(a[0]).filename().string() != a[0] || a[0] == "." || a[0] == "..") throw Error("give the mod's name, not a path");
    for (const char* ext : {"", ".rlmod", ".dll"}) {
        const fs::path p = dir / (a[0] + ext);
        if (!fs::is_regular_file(p) || (p.extension() != ".rlmod" && p.extension() != ".dll")) continue;
        fs::remove(p);
        std::cout << "Removed: " << p.string() << "\nRestart the game if it is running.\n";
        return 0;
    }
    throw Error("no mod named " + a[0] + " (see rocketify list)");
}

// The path of this program.
static fs::path Self() {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) throw Error("could not find this program");
    return std::string(buf, n);
#else
    return fs::canonical("/proc/self/exe");
#endif
}

// Replaces this program with the newest one.
static int CmdUpdate() {
    if (Which("curl").empty()) throw Error("curl is needed");
    const fs::path self = Self();
    const fs::path tmp = self.string() + ".new", old = self.string() + ".old";
    std::error_code ec;
    if (Run({"curl", "-fsSL", std::string(kCliUrl) + (kWindows ? "rocketify.exe" : "rocketify"), "-o", tmp.string()}) != 0) {
        fs::remove(tmp, ec);
        throw Error("could not download the newest version");
    }
    if (ReadFile(tmp) == ReadFile(self)) {
        fs::remove(tmp, ec);
        std::cout << "rocketify " << kVersion << " is up to date\n";
        return 0;
    }
    fs::permissions(tmp, fs::status(self).permissions(), ec);
    if (kWindows) {  // a running program can be renamed, but not overwritten
        fs::remove(old, ec);
        fs::rename(self, old);
    }
    fs::rename(tmp, self, ec);
    if (ec) {
        if (kWindows) fs::rename(old, self);
        throw Error("could not replace " + self.string());
    }
    std::cout << "Updated: " << self.string() << "\n";
    return 0;
}

static int CmdDoctor() {
    int bad = 0;
    auto line = [&](const std::string& what, const std::string& value, const std::string& fix) {
        std::cout << (value.empty() ? "[missing] " : "[ok]      ") << what << ": " << (value.empty() ? fix : value) << "\n";
        if (value.empty()) ++bad;
    };
#ifdef _WIN32
    const bool tools = !Which("cmake").empty() && (!Which("g++").empty() || HasVisualStudio());
    line("Build tools", tools ? "ok" : "", "run: rocketify install dependencies");
#else
    line("Build tools", HasMingw() ? "ok" : HasBox() ? "distrobox '" + Box() + "'" : "", "run: rocketify install dependencies");
#endif
    std::string mods;
    try {
        mods = NeedModsDir().string();
        line("Game", mods, "");
        const std::string problem = LoaderProblem(mods);
        line("Loader", problem.empty() ? "ok" : "", problem);
    } catch (const Error& e) {
        line("Game", "", e.what());
    }
    return bad ? 1 : 0;
}

static int Help() {
    std::cout << "rocketify " << kVersion << " - make Rocketify mods\n\n"
                 "  rocketify install dependencies\n"
                 "                               install the tools needed to build mods\n"
                 "  rocketify new [NAME]         create a mod\n"
                 "  rocketify build              build the mod in this folder and put it into the game\n"
                 "      --no-install             only build it\n"
                 "      -i, --install            fail if it cannot be put into the game\n"
                 "      --debug                  debug build\n"
                 "      --clean                  build everything again\n"
                 "  rocketify install            put the built mod into the game again\n"
                 "  rocketify list               show the mods in the game\n"
                 "  rocketify remove NAME        remove a mod from the game\n"
                 "  rocketify update             update this tool\n"
                 "  rocketify config [KEY [VAL]] show or change settings\n"
                 "  rocketify doctor             check that everything is set up\n";
    return 0;
}

int main(int argc, char** argv) {
    const Args a(argv + (argc > 1 ? 2 : 1), argv + argc);
    const std::string cmd = argc > 1 ? argv[1] : "help";
    try {
        if (cmd == "new") return CmdNew(a);
        if (cmd == "build") {
            const int rc = CmdBuild(a);
            const auto has = [&](const char* o) { return std::find(a.begin(), a.end(), o) != a.end(); };
            if (rc || has("--no-install")) return rc;
            if (has("-i") || has("--install")) return CmdInstall();
            try {  // a finished mod goes into the game; building also works without one
                return CmdInstall();
            } catch (const std::exception& e) {
                std::cout << "Not installed: " << e.what() << "\n";
                return 0;
            }
        }
        if (cmd == "install") {
            if (a.empty()) return CmdInstall();
            if (a.size() == 1 && a[0] == "dependencies") return CmdDependencies();
            throw Error("use: rocketify install, or: rocketify install dependencies");
        }
        if (cmd == "list") return CmdList();
        if (cmd == "remove") return CmdRemove(a);
        if (cmd == "update") return CmdUpdate();
        if (cmd == "config") return CmdConfig(a);
        if (cmd == "doctor") return CmdDoctor();
        if (cmd == "help" || cmd == "--help" || cmd == "-h") return Help();
        if (cmd == "--version" || cmd == "-v") {
            std::cout << kVersion << "\n";
            return 0;
        }
        throw Error("unknown command " + cmd + " (see rocketify help)");
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
