// zip в память как map путь -> байты. Настоящий формат проверяется в tests/infra
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "safebox/domain/ports/archive.hpp"

namespace safebox::test {

class MemZipWriter final : public domain::ZipWriter {
public:
    struct Archive {
        std::vector<std::string> order;
        std::set<std::string> directories;
        std::map<std::string, domain::Bytes> files;
        bool finished = false;
    };

    std::unique_ptr<domain::ZipStream> start(domain::ByteSink& out) override {
        archives.emplace_back();
        return std::make_unique<Stream>(archives.back(), out);
    }

    std::deque<Archive> archives;

private:
    class Stream final : public domain::ZipStream {
    public:
        Stream(Archive& archive, domain::ByteSink& out) : archive_(archive), out_(out) {}

        domain::Status addDirectory(std::string_view path, std::int64_t /*mtimeMs*/) override {
            archive_.directories.insert(std::string(path));
            archive_.order.emplace_back(path);
            return {};
        }

        domain::Status addFile(std::string_view path, std::uint64_t size, std::int64_t /*mtimeMs*/,
                               domain::ByteSource& source) override {
            domain::Bytes data;
            domain::Bytes buffer(64 * 1024);
            for (;;) {
                auto n = source.read(buffer);
                if (!n) {
                    return std::unexpected(n.error());
                }
                if (*n == 0) {
                    break;
                }
                data.insert(data.end(), buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(*n));
                if (auto st = out_.write(std::span<const std::byte>(buffer.data(), *n)); !st) {
                    return st;
                }
            }
            if (data.size() != size) {
                return domain::fail(domain::Error::Code::IntegrityError, "size mismatch");
            }
            archive_.files[std::string(path)] = std::move(data);
            archive_.order.emplace_back(path);
            return {};
        }

        domain::Status finish() override {
            archive_.finished = true;
            return {};
        }

    private:
        Archive& archive_;
        domain::ByteSink& out_;
    };
};

} // namespace safebox::test
