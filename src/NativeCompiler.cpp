#include "toro/NativeCompiler.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace toro {
namespace {

struct ProcessResult {
    std::optional<int> exit_code;
    std::optional<int> signal;
    std::optional<int> exec_error;
};

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
#if defined(_WIN32)
        throw std::runtime_error(
            "native toolchain error: temporary directories are not supported on this host");
#else
        std::string pattern =
            (std::filesystem::temp_directory_path() / "toro-native-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (!created) {
            throw std::runtime_error(
                "native toolchain error: could not create temporary directory: "
                + std::string(std::strerror(errno)));
        }
        path_ = created;
#endif
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::optional<std::filesystem::path> find_on_path(std::string_view executable)
{
#if defined(_WIN32)
    static_cast<void>(executable);
    return std::nullopt;
#else
    const char* path_value = std::getenv("PATH");
    if (!path_value) {
        return std::nullopt;
    }
    std::string_view path(path_value);
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find(':', start);
        const std::string_view directory = path.substr(
            start,
            separator == std::string_view::npos
                ? path.size() - start
                : separator - start);
        const auto candidate = (directory.empty()
                ? std::filesystem::current_path()
                : std::filesystem::path(directory))
            / executable;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return std::nullopt;
#endif
}

ProcessResult run_process(const std::vector<std::string>& arguments)
{
#if defined(_WIN32)
    static_cast<void>(arguments);
    throw std::runtime_error(
        "native toolchain error: process execution is not supported on this host");
#else
    if (arguments.empty()) {
        throw std::runtime_error("native toolchain error: empty process command");
    }

    int error_pipe[2];
    if (::pipe(error_pipe) != 0) {
        throw std::runtime_error(
            "native toolchain error: could not create process pipe: "
            + std::string(std::strerror(errno)));
    }
    if (::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        const int error = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        throw std::runtime_error(
            "native toolchain error: could not configure process pipe: "
            + std::string(std::strerror(error)));
    }

    const pid_t process = ::fork();
    if (process == -1) {
        const int error = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        throw std::runtime_error(
            "native toolchain error: could not create process: "
            + std::string(std::strerror(error)));
    }
    if (process == 0) {
        ::close(error_pipe[0]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        const int error = errno;
        static_cast<void>(::write(error_pipe[1], &error, sizeof(error)));
        ::_exit(127);
    }

    ::close(error_pipe[1]);
    int exec_error = 0;
    std::size_t bytes_read = 0;
    int pipe_error = 0;
    while (bytes_read < sizeof(exec_error)) {
        const ssize_t count = ::read(
            error_pipe[0],
            reinterpret_cast<char*>(&exec_error) + bytes_read,
            sizeof(exec_error) - bytes_read);
        if (count > 0) {
            bytes_read += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno != EINTR) {
            pipe_error = errno;
            break;
        }
    }
    ::close(error_pipe[0]);

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(process, &status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == -1) {
        throw std::runtime_error(
            "native toolchain error: could not wait for process: "
            + std::string(std::strerror(errno)));
    }
    if (pipe_error != 0) {
        throw std::runtime_error(
            "native toolchain error: could not read process status: "
            + std::string(std::strerror(pipe_error)));
    }

    ProcessResult result;
    if (bytes_read == sizeof(exec_error)) {
        result.exec_error = exec_error;
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
    return result;
#endif
}

void write_c_source(
    const std::filesystem::path& path,
    std::string_view source)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error(
            "native toolchain error: could not write generated C file '"
            + path.string() + "'");
    }
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!output) {
        throw std::runtime_error(
            "native toolchain error: failed while writing generated C file '"
            + path.string() + "'");
    }
}

void compile_c(
    const std::filesystem::path& compiler,
    const std::filesystem::path& c_path,
    const std::filesystem::path& output_path)
{
    const auto result = run_process({
        compiler.string(),
        "-std=c11",
        c_path.string(),
        "-o",
        output_path.string(),
    });
    if (result.exec_error) {
        throw std::runtime_error(
            "C compiler process failure: could not execute '" + compiler.string()
            + "': " + std::string(std::strerror(*result.exec_error)));
    }
    if (result.signal) {
        throw std::runtime_error(
            "C compiler process failure: compiler terminated by signal "
            + std::to_string(*result.signal));
    }
    if (!result.exit_code || *result.exit_code != 0) {
        throw std::runtime_error(
            "C compiler failed with exit status "
            + std::to_string(result.exit_code.value_or(1)));
    }
}

} // namespace

NativeCompiler::NativeCompiler()
{
    if (const auto clang = find_on_path("clang")) {
        compiler_path_ = *clang;
    } else if (const auto cc = find_on_path("cc")) {
        compiler_path_ = *cc;
    } else {
        throw std::runtime_error(
            "native toolchain error: no system C compiler found; expected clang or cc in PATH");
    }
}

void NativeCompiler::build(
    std::string_view c_source,
    const std::filesystem::path& output_path) const
{
    TemporaryDirectory temporary;
    const auto c_path = temporary.path() / "program.c";
    write_c_source(c_path, c_source);
    compile_c(compiler_path_, c_path, std::filesystem::absolute(output_path));
}

int NativeCompiler::run(std::string_view c_source) const
{
    TemporaryDirectory temporary;
    const auto c_path = temporary.path() / "program.c";
    const auto executable_path = temporary.path() / "program";
    write_c_source(c_path, c_source);
    compile_c(compiler_path_, c_path, executable_path);

    const auto result = run_process({executable_path.string()});
    if (result.exec_error) {
        throw std::runtime_error(
            "runtime process failure: could not execute generated program: "
            + std::string(std::strerror(*result.exec_error)));
    }
    if (result.signal) {
        throw std::runtime_error(
            "runtime process failure: generated program terminated by signal "
            + std::to_string(*result.signal));
    }
    if (!result.exit_code) {
        throw std::runtime_error(
            "runtime process failure: generated program produced no exit status");
    }
    return *result.exit_code;
}

const std::filesystem::path& NativeCompiler::compiler_path() const
{
    return compiler_path_;
}

} // namespace toro
