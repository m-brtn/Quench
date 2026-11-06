#include <quench/core/FileSystem.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

namespace quench::fs {
namespace {

class MemoryReader final : public Reader {
public:
    explicit MemoryReader(Bytes bytes) : m_bytes(std::move(bytes)) {}

    std::uint64_t size() const override { return m_bytes.size(); }

    Result<std::size_t> read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset >= m_bytes.size()) return std::size_t(0);
        const std::size_t count = std::min<std::size_t>(out.size(), m_bytes.size() - std::size_t(offset));
        std::copy_n(m_bytes.begin() + std::ptrdiff_t(offset), count, out.begin());
        return count;
    }

private:
    Bytes m_bytes;
};

class StdReader final : public Reader {
public:
    StdReader(std::ifstream in, std::uint64_t size, std::string name)
        : m_in(std::move(in)), m_size(size), m_name(std::move(name)) {}

    std::uint64_t size() const override { return m_size; }

    Result<std::size_t> read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset >= m_size) return std::size_t(0);
        const std::size_t count = std::min<std::size_t>(out.size(), std::size_t(m_size - offset));
        m_in.clear();
        m_in.seekg(std::streamoff(offset));
        if (count > 0 && !m_in.read(reinterpret_cast<char*>(out.data()), std::streamsize(count)))
            return Error{"cannot read " + m_name};
        return count;
    }

private:
    std::ifstream m_in;
    std::uint64_t m_size = 0;
    std::string m_name;
};

class StdBackend final : public Backend {
public:
    StdBackend(std::filesystem::path assetRoot, std::filesystem::path userRoot)
        : m_assetRoot(std::move(assetRoot)), m_userRoot(std::move(userRoot)) {}

    Result<Bytes> read(Root root, std::string_view path) override {
        if (root == Root::User && m_userRoot.empty()) return Error{"no user directory"};
        const std::filesystem::path full = resolve(root, path);
        std::ifstream in(full, std::ios::binary);
        if (!in) return Error{"cannot open " + full.string()};
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) return Error{"cannot read " + full.string()};
        in.seekg(0, std::ios::beg);
        Bytes bytes(static_cast<std::size_t>(size));
        if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size))
            return Error{"cannot read " + full.string()};
        return bytes;
    }

    Result<std::unique_ptr<Reader>> open(Root root, std::string_view path) override {
        if (root == Root::User && m_userRoot.empty()) return Error{"no user directory"};
        const std::filesystem::path full = resolve(root, path);
        std::ifstream in(full, std::ios::binary);
        if (!in) return Error{"cannot open " + full.string()};
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) return Error{"cannot read " + full.string()};
        return std::unique_ptr<Reader>(std::make_unique<StdReader>(std::move(in), std::uint64_t(size), full.string()));
    }

    bool exists(Root root, std::string_view path) override {
        if (root == Root::User && m_userRoot.empty()) return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(resolve(root, path), ec);
    }

    Result<void> writeUser(std::string_view path, std::span<const std::uint8_t> data) override {
        if (m_userRoot.empty()) return Error{"no user directory"};
        const std::filesystem::path full = resolve(Root::User, path);
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        if (ec) return Error{"cannot create " + full.parent_path().string() + ": " + ec.message()};
        std::filesystem::path temp = full;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return Error{"cannot open " + temp.string()};
            out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            if (!out.flush()) return Error{"cannot write " + temp.string()};
        }
        std::filesystem::rename(temp, full, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return Error{"cannot replace " + full.string()};
        }
        return {};
    }

    std::optional<std::int64_t> modifiedTime(Root root, std::string_view path) override {
        if (root == Root::User && m_userRoot.empty()) return std::nullopt;
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(resolve(root, path), ec);
        if (ec) return std::nullopt;
        return std::int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
    }

private:
    std::filesystem::path resolve(Root root, std::string_view path) const {
        const std::filesystem::path& base = root == Root::Assets ? m_assetRoot : m_userRoot;
        return base.empty() ? std::filesystem::path(path) : base / std::filesystem::path(path);
    }

    std::filesystem::path m_assetRoot;
    std::filesystem::path m_userRoot;
};

std::unique_ptr<Backend>& backend() {
    static std::unique_ptr<Backend> b = makeStdBackend({}, {});
    return b;
}

std::string text(Bytes bytes) { return std::string(bytes.begin(), bytes.end()); }

} // namespace

std::unique_ptr<Reader> makeMemoryReader(Bytes bytes) { return std::make_unique<MemoryReader>(std::move(bytes)); }

std::unique_ptr<Backend> makeStdBackend(std::filesystem::path assetRoot, std::filesystem::path userRoot) {
    return std::make_unique<StdBackend>(std::move(assetRoot), std::move(userRoot));
}

void setBackend(std::unique_ptr<Backend> b) { backend() = b ? std::move(b) : makeStdBackend({}, {}); }

std::string checkPath(std::string_view path) {
    if (path.empty()) return "empty path";
    if (path.front() == '/') return "absolute path";
    if (path.find('\\') != std::string_view::npos) return "backslash in path";
    if (path.find(':') != std::string_view::npos) return "':' in path";
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = std::min(path.find('/', begin), path.size());
        const std::string_view segment = path.substr(begin, end - begin);
        if (segment.empty() || segment == "." || segment == "..") return "empty, '.' or '..' segment in path";
        begin = end + 1;
    }
    return {};
}

Result<Bytes> readAsset(std::string_view path) {
    if (std::string reason = checkPath(path); !reason.empty()) return Error{reason + ": " + std::string(path)};
    return backend()->read(Root::Assets, path);
}

Result<std::string> readAssetText(std::string_view path) {
    Result<Bytes> bytes = readAsset(path);
    if (!bytes) return Error{bytes.error()};
    return text(std::move(bytes).value());
}

Result<std::unique_ptr<Reader>> openAsset(std::string_view path) {
    if (std::string reason = checkPath(path); !reason.empty()) return Error{reason + ": " + std::string(path)};
    return backend()->open(Root::Assets, path);
}

bool assetExists(std::string_view path) { return checkPath(path).empty() && backend()->exists(Root::Assets, path); }

std::optional<std::int64_t> assetModifiedTime(std::string_view path) {
    if (!checkPath(path).empty()) return std::nullopt;
    return backend()->modifiedTime(Root::Assets, path);
}

Result<Bytes> readUser(std::string_view path) {
    if (std::string reason = checkPath(path); !reason.empty()) return Error{reason + ": " + std::string(path)};
    return backend()->read(Root::User, path);
}

Result<std::string> readUserText(std::string_view path) {
    Result<Bytes> bytes = readUser(path);
    if (!bytes) return Error{bytes.error()};
    return text(std::move(bytes).value());
}

bool userExists(std::string_view path) { return checkPath(path).empty() && backend()->exists(Root::User, path); }

Result<void> writeUser(std::string_view path, std::span<const std::uint8_t> data) {
    if (std::string reason = checkPath(path); !reason.empty()) return Error{reason + ": " + std::string(path)};
    return backend()->writeUser(path, data);
}

Result<void> writeUserText(std::string_view path, std::string_view text) {
    return writeUser(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

} // namespace quench::fs
